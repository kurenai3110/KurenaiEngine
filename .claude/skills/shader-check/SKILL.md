---
name: shader-check
description: HLSLをfxc/dxc単体で一括コンパイル検証するときに使う。通常の検証はビルド(KurenaiShaderPackerが全バリアントを焼く)が兼ねるので不要で、これを使うのはビルドを通さずにHLSLだけ見たいとき、パッカーとコンパイラのどちらが原因かを切り分けたいとき。Use to compile-check HLSL with fxc/dxc directly, when the normal build-time check is not available or when isolating a packer-vs-compiler problem.
---

# HLSLの一括コンパイル検証

## まずビルドを通すこと。これは切り分け用

**HLSLの一括検証はビルドが兼ねている。** `KurenaiShaderPacker` が
`KurenaiEngine3D` / `KurenaiEngine2D` のビルドイベントで全 `.hlsl` の全エントリを
3バリアント(SM 5.0 / SM 6.5 / SM 6.6+bindless)で焼き、1つでも失敗すればビルドが落ちる。
**通常は `build-run` スキルでビルドすれば済む。**

このスキルを使うのは次の場合:

- ビルドを通さずにHLSLだけ見たいとき(C++が壊れていてビルドが通らない、など)
- パッカーの不具合とHLSLの不具合を切り分けたいとき(fxc/dxcを直接叩いて確かめる)
- パッカーの除外規則(`Tools\KurenaiShaderPacker\Source\Main.cpp` の `kSkipDxil65Files` と
  SM6専用ファイルの判定)が実態と合っているかを確かめたいとき

### このスクリプトの取りこぼし(パッカーとの違い)

**このスクリプトは `.hlsl` しか走査せず、`#include` を展開しない。**
そのためエントリポイントの実体が `.hlsli` 側にあるものを検出できない
――例えば `GBuffer.hlsl` の `VSMain` は実体が `GBufferCommon.hlsli:224` にあり、
**このスクリプトでは一度も検証されていない**。パッカー側
(`Tools\KurenaiShaderPacker\Source\ShaderEntryScanner.cpp`)はインクルードを展開してから走査する。
検出規則を変えるときは**両方を直すこと。**

## 実行

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File "$env:CLAUDE_PROJECT_DIR\.claude\skills\shader-check\scripts\check-shaders.ps1" `
  -ShaderRoot "<worktree>\KurenaiEngine\Shaders"
```

`-ShaderRoot`を省略すると`$env:CLAUDE_PROJECT_DIR\KurenaiEngine\Shaders`を見る。
worktreeで作業している場合は**必ず自分のworktreeのパスを明示すること**。

終了コード 0 = 全通過、1 = 1つ以上失敗。失敗時はファイル・エントリ・エラー行を一覧表示する。

現状(2026-08-29時点)の基準値: **41ファイル / 237エントリ / 失敗0**。
パッカー(ビルド時)の基準値は別に取ること ―― インクルードを展開して走査するぶん、
検出されるエントリがこのスクリプトより多くなる。

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
| DX11 | fxc | `vs_5_0`/`ps_5_0`/`cs_5_0` | `.kshader`の`Dxbc50`バリアント |
| DX12 | dxc | `*_6_6` | `.kshader`の`Dxil66`バリアント |

**DX12側をfxcの`*_5_0`で見てはいけない。** DX12はDXILのバリアントを使い、
DXBC(`*_5_0`)へ落ちるのはデバイスがSM 6.5未満の場合だけ。

**パッカーとはプロファイルが1つずれている。** パッカーはbindless無しの段を `*_6_5` で焼くが
(SM 6.5のデバイスでも動く必要があるため)、このスクリプトは `*_6_6` で2回回す。
そのため `*_6_6` でしか通らないもの(コンピュートシェーダー内の微分など)を
このスクリプトは見逃す。パッカーの `kSkipDxil65Files` がその差を受け止めている。
### `-HV 2018` を必ず渡す（エンジンに合わせる）

**dxc 1.7以降はHLSL 2021が既定**だが、**このコードベースはHLSL 2018に固定している。**
`ShaderCompiler::CompileDxil`が無条件に`-HV 2018`を渡しており
(`Tools/KurenaiShaderPacker/Source/ShaderCompiler.cpp`)、理由もコメントにある——
dxcが将来既定を上げても、`dxcompiler.dll`を差し替えただけで
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

`RayQuery`・`TraceRay`・`RaytracingAccelerationStructure`・`ResourceDescriptorHeap` を使うファイルは
fxc(DX11経路)の対象から外す。DX11はこれらの描画パスをそもそも持たない。

### 除外の判定に `KURENAI_BINDLESS` を使ってはいけない

**そのマクロで「分岐している」だけのファイルは、マクロ未定義ならSM 5.0で完全にコンパイルできる。**
`Shadow.hlsl`がまさにそれで、**DX11は実行時に`D3DCompileFromFile`でこのファイルを読む。**
文字列を含むだけで除外すると、DX11経路が未検証のまま「失敗0」で通る。

2026-08-29に実際にこの穴を開けた。シャドウのアルファカットアウトでbindlessの分岐を足した結果、
`Shadow.hlsl`がfxcの対象から静かに外れた。`PSMainCutout`へ`WaveActiveAllTrue`(SM 6.0以降)を
入れても**「失敗0」で通る**ことを対照実験で確認している。
しかも`KurenaiEngine3D.cpp`のshadow系`CreateShader`はtry/catchに入っていないため、
SM 5.0で落ちる変更が入ると**DX11は起動時に例外で死ぬ**。

判定したいのは「マクロ未定義に展開してもSM 5.0の機能しか使わないか」で、これは字面では決まらない。
そこで**フォールバックを持たない(bindless専用の)シェーダーだけ、ファイル先頭に
`KURENAI_SHADER_BINDLESS_ONLY`と書く**ことにした(現状は`SoftwareRaster.hlsl`と
`SoftwareRasterResolve.hlsl`の2本)。**印を付け忘れたらfxcに掛かって失敗する**ので、
黙って検証から漏れる側には倒れない。

増幅/メッシュシェーダーのファイル(`GBufferMeshlet.hlsl`・`ShadowMeshlet.hlsl`)は
`ResourceDescriptorHeap`を直接使っているので自動的に外れる。

**「全部通った」を見たら母数も見ること。** 除外が増えれば失敗0のまま検証範囲だけが狭まる。

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
- **パッカーはこの複製を必要としない。** `dxc.exe` を外から叩くのではなく `dxcompiler.dll` を
  直接使い、自前のインクルードハンドラ(`Utf8IncludeHandler`)がBOM無しUTF-8を
  `CP_UTF8` と明示して読むため(`Tools/KurenaiShaderPacker/Source/ShaderCompiler.cpp`)

## 使いどころ

**通常は不要。** `.hlsl` を編集したら `build-run` スキルでビルドすれば、その中で全バリアントが焼かれ、
失敗すればビルドが落ちる。マージやPR取り込みの直後も同じ。

このスキルを使うのは上の「まずビルドを通すこと」の3ケースに当てはまるときだけ。
**`.hlsl`の変更だけでもビルドは必要**(出力フォルダには `.kshader` しか置かれず、
`.hlsl` を差し替えて起動し直すやり方は使えない)。詳しくは `ab-compare` スキル。
