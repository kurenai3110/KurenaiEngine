---
name: shader-check
description: KurenaiEngineの全HLSLをまとめてコンパイル検証するときに使う。マージ後・PR取り込み後・シェーダを編集した後の確認用。C++のビルドが通ってもHLSLは未検証のままなので、これを通すまで「ビルドが通った」と言わないこと。Use after merging, after pulling a PR, or after editing any .hlsl to verify all shaders still compile.
---

# HLSLの一括コンパイル検証

## なぜ必要か

**シェーダは実行時に出力フォルダの`Shaders\`から読まれる。**
C++のビルドが通ってもHLSLは一切検証されていない。壊れたシェーダはアプリを起動して
その描画パスに到達して初めて分かる。マージやPR取り込みの直後は特に危ない。

## 実行

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File "$env:CLAUDE_PROJECT_DIR\.claude\skills\shader-check\scripts\check-shaders.ps1" `
  -ShaderRoot "<worktree>\KurenaiEngine\Shaders"
```

`-ShaderRoot`を省略すると`$env:CLAUDE_PROJECT_DIR\KurenaiEngine\Shaders`を見る。
worktreeで作業している場合は**必ず自分のworktreeのパスを明示すること**。

終了コード 0 = 全通過、1 = 1つ以上失敗。失敗時はファイル・エントリ・エラー行を一覧表示する。

現状(2026-08-04時点)の基準値: **31ファイル / 185エントリ / 失敗0**。

## スクリプトが何をしているか

エントリポイントはHLSLから**構造的に**検出する(固定の名前リストではない。
エンジンは`CSDownsample`・`PSMainBlur`・`CSClearHistogram`のようなサフィックス付きを使うため):

| ステージ | 検出条件 |
|---|---|
| mesh | `[outputtopology(...)]`が付いた関数 |
| amplification | `[numthreads(...)]`が付き、本体で`DispatchMesh()`を呼ぶ関数 |
| compute | それ以外で`[numthreads(...)]`が付いた関数 |
| pixel | `PS*`で戻り値セマンティクスが`SV_`、または`PSOutput`戻り、または`void` |
| vertex | `VS*`で引数に`SV_`があるか戻り値が構造体 |

### 2つのバックエンド = 2つのコンパイラ。両方通す必要がある

| 経路 | コンパイラ | プロファイル | 根拠 |
|---|---|---|---|
| DX11 | fxc | `vs_5_0`/`ps_5_0`/`cs_5_0` | `DX11Device.cpp`の`D3DCompileFromFile` |
| DX12 | dxc | `*_6_6` | `DX12Device::CreateShader` |

**DX12側をfxcの`*_5_0`で見てはいけない。** `DX12Device::CreateShader`は
`dxcompiler.dll`がロードできればRTに限らず**全ステージ**をdxcへ流す
(`D3DCompileFromFile`/`*_5_0`はdxcが無い・SM 6.0未満の環境向けのフォールバック)。
### `-HV 2018` を必ず渡す（エンジンに合わせる）

**dxc 1.7以降はHLSL 2021が既定**だが、**エンジンはHLSL 2018に固定している。**
`DX12ShaderCompiler::Compile`が無条件に`-HV 2018`を渡しており
(`Source/Library/RHI/DX12/DX12ShaderCompiler.cpp`)、理由もコメントにある——
dxcが将来既定を上げても、配布する`dxcompiler.dll`を差し替えただけで
既存シェーダーの意味が変わらないようにするため。

したがって**検証側も`-HV 2018`を渡さないと、実際には動くコードを失敗として報告する。**
2026-08-05にこれで14件の偽陽性を出した(`ReflectionProbe.hlsli:130`の
`(localR < 0.0f) ? -1.0f : 1.0f`。`localR`はfloat3)。スクリプトは修正済み。

つまり**HLSL 2021で禁止された書き方はエンジンでは合法**であり、直す必要はない:

- ベクタを条件にした三項演算子(`(v < 0) ? a : b`) … 2018では成分ごとのselectとして通る
- ベクタに対する`&&` / `||`

一方、これはfxcの5_0経路では出ないことがある:

- 全インクルードを合成して初めて分かるレジスタの二重割り当て

DX12パスは`-D KURENAI_BINDLESS=1`の**有無で2回**回す。エンジンはSM 6.6対応時だけこれを定義するため、
どちらの構成も出荷される。増幅/メッシュシェーダーは`ResourceDescriptorHeap`でジオメトリを引く
bindless経路専用なので、dxcで、定義ありの1回だけコンパイルする。

`RayQuery`・`ResourceDescriptorHeap`・`KURENAI_BINDLESS`などSM 5.0に無いものを使うファイルは
fxc(DX11経路)の対象から外す。DX11はこれらの描画パスをそもそも持たない。

## BOMの扱い（重要・fxcとdxcで逆）

| コンパイラ | BOM |
|---|---|
| **fxc** | **付いていると失敗**する (`error X3000: illegal character`) |
| **dxc** (SDK同梱) | **無いと失敗**する。BOM無しUTF-8をANSIとして読み、日本語コメントで `dxc failed : error code 0x80070459` (Unicode変換不可) |

そのため:
- **リポジトリ上の`.hlsl`/`.hlsli`は常にBOM無し**にする(PostToolUseフック`check-file-encoding.ps1`が検出する)
- スクリプトはdxcを使うとき、**シェーダツリー全体をBOM付きで一時ディレクトリへ複製**して
  そこからコンパイルする。インクルード(`.hlsli`)も同じ問題を起こすため、1ファイルではなく
  ツリーごと複製する必要がある。一時ディレクトリは実行後に削除する

## 使いどころ

- `origin/master`を取り込んだ直後
- PRをマージした直後
- `.hlsl`を編集したあと、アプリを起動する前
- 「ビルドが通った」と報告する前

`.hlsl`の変更だけを確認したい場合、**ビルドし直す必要はない**
(実行時に読まれるため、差し替えて起動するだけでよい)。詳しくは `ab-compare` スキル。
