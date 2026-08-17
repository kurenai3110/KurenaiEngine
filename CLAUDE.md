# KurenaiEngine 固有のルール

全プロジェクト共通のルール(日本語コメント・エラーハンドリング・PIDの扱い・実カーソル禁止・
文字コード・worktree運用・コミットの作法)は `~/.claude/CLAUDE.md` にある。
ここにはKurenaiEngineでしか成り立たない前提だけを書く。

# 手順はスキルに置いてある(読まずに自己流でやらない)

このリポジトリが持つのは、KurenaiEngine固有の手順だけ。

| やること | スキル |
|---|---|
| ビルドと起動 | `build-run` |
| HLSLの一括コンパイル検証(マージ後・PR取り込み後・`.hlsl`編集後) | `shader-check` |
| 描画結果のA/B比較 | `ab-compare` |
| 実在の風景・建物を参考画像に近づける | `reference-match` |

アプリの起動・撮影・入力(`verify-app`)、コミットとPR(`commit-flow`)、worktreeの棚卸し
(`worktree-audit`)、コードベースの調査(`graphify`)は**横断スキル**で、`~/.claude/skills/` にある。

# 構成とビルド

- **DX11とDX12の両対応**。`Source/Library/RHI/` のRHI抽象化層で吸収しており、
  **RHIのインターフェースを変えたら DX11 / DX12 の両方を直す**。片方だけ直すと、
  もう片方は起動して初めて壊れていることが分かる
- **3つのDLLに分かれる**(`KurenaiEngineLibrary` = 共通基盤 / `KurenaiEngine3D` / `KurenaiEngine2D`)。
  3Dと2Dは互いに依存しない
- **`KurenaiEngine.sln` は3DLL単体のビルド確認用**。実際に動かして確認するなら
  `Samples/Sample3D/Sample3D.sln`(または `Sample2D.sln`)を叩く
- ビルドと起動の手順は `build-run` スキル(`.claude/skills/build-run/`)にある。読まずに自己流で
  やらないこと。sln の使い分け・assimp/DirectXTexの事前ビルド・fxcのPATHはこのプロジェクト固有

# 描画上の前提(誤認しやすいもの)

- **深度バッファは Reverse-Z。近平面が NDC z=1.0、遠平面が z=0.0。**
  「**深度値が小さいほど遠い**」「**背景(ジオメトリ無し)は深度0**」であり、
  最も手前を取るなら `max`、最も遠くを取るなら `min`。逆に書いても絵は出るので、
  静かに間違ったまま進みやすい。詳細は [docs/Architecture.html](docs/Architecture.html) 3章
- **描画はDeferred Shadingの10パス構成**(シャドウ→ジオメトリ→Hi-Z→直接光→AO/GI→最終合成→
  半透明フォワード→SSR→Tonemap→Present)。**半透明(`alphaMode=BLEND`)だけはG-Bufferに書かず
  専用のフォワードパスへ回る**ため、スクリーンスペース系の効果(SSR等)が効かない
- **シェーダはビルド対象ではない。** `.hlsl` は実行時に出力フォルダの `Shaders/` から読まれる。
  **C++のビルドが通ってもHLSLは一切検証されていない**ので、シェーダを触ったら必ず起動して確かめる。
  逆に、シェーダだけの変更ならビルド不要(出力フォルダの `Shaders/` を差し替えて起動し直せばよい)

# アセット

- **`Assets/` はGit管理外**(180MB規模のため)。**エンジンが実際に読むのは常に `Assets/Packed/`**。
  `Assets/Source/`(`.gltf`等)を `KurenaiPacker.exe` で変換して作る
- **`.kmodel` は v9 / `.kgeom` は v3。バージョン不一致は読み込みを拒否される。**
  フォーマットを触ったら既存の `Assets/Packed/` は再パックが要る
- `Assets/Source/` のうち **Sponza 以外は `Tools/*.py` で再生成できる**(検証用シーンはすべて
  スクリプト生成)。手順は [README.md](README.md)「手順5. アセットの準備」
- 手書きの `.kscene` だけは `Assets/` の外の `Scenes/` にあり、**こちらはGit管理対象**

# 環境依存の前提

- **dxc のバージョン = Windows SDK のバージョン。** `dxcompiler.dll` はSDKの
  `bin\<SDKバージョン>\x64` からPostBuildEventでコピーされる。**10.0.26100未満だと SM6.6 が無く、
  bindless と、bindlessでジオメトリを引くメッシュレット描画が連動して無効になる**
- **性能の話の基準になる実機は Intel UHD Graphics 620**(i5-8250U内蔵 / 専用VRAM無し / 60Hz)。
  この実機は **DXR・メッシュシェーダー・bindless(SM6.6)のいずれも非対応**なので、
  RT反射/RTシャドウ/RTAO とメッシュレット経路は**この環境では検証できない**。
  「遅い/速い」を語るときは、どの実機・どのシーン・どの構成での実測かを必ず添える
- **測るときは Release**。`docs/ImplementationHistory.md` にはDebugビルドでの計測値も残っているので、
  過去の数値と比べるときは構成を確認する

# ドキュメントの書き分け

機能を実装したら、内容に応じて書く先を分ける(READMEの「ドキュメント」節がこの分担を定義している)。

| 書く先 | 内容 |
|---|---|
| `README.md` | 使う側に必要なこと(必要環境・セットアップ・起動・操作・アセットの準備) |
| `docs/KurenaiEngine.html` | APIリファレンス。エンジンを使ってアプリを作る人向け |
| `docs/Architecture.html` | 設計と仕様。描画パイプラインの構成・責務分割・インターフェース規約 |
| `docs/ImplementationDetail.md` | 既定値やしきい値の**根拠**、式の導出、検証手順 |
| `docs/ImplementationHistory.md` | **経緯**。以前どうだったか・何が問題として現れたか・どう直したか |

# 起動と確認

- 起動・PIDの扱い・スクリーンショットは `verify-app` スキルに従う
  (**プロセスは必ずPIDで扱う。名前指定の終了は禁止**)
- ログは実行ファイルと同じフォルダに**バックエンドごとに分かれて**出る
  (`KurenaiEngine_DX11.log` / `KurenaiEngine_DX12.log`)。既定はDX11で、`-dx12` でDX12始動
- **worktreeで作業中は、ビルド出力もログも自分のworktree配下のものを見る**。
  本体と各worktreeで別物になっている

# Claude向け資産の置き場所

- 横断的なもの(共通`CLAUDE.md`・skills・agents・hooks・`WinAutomation.ps1`)は
  **`~/.claude/` にあり、このリポジトリには入っていない**。worktreeを作ってもどのプロジェクトを
  開いても効くようにするため。`git clean -xdf` 等で消さないよう注意する。
  別PCへの引き継ぎは `kurenai-claude-config` リポジトリの `Install-ClaudeConfig.ps1` で行う
- このリポジトリがGit管理するのは、**このファイルと `.claude/skills/` 配下の固有スキル
  (`build-run` / `shader-check` / `ab-compare` / `reference-match`)だけ**。
  worktreeやcloneに自動で付いてくる必要があるプロジェクト固有の情報だから
- `.claude/settings.local.json`(PCごとの許可リスト)、worktreeの実体(`.worktrees/` と
  `.claude/worktrees/` の両方。PCによって置き場所が違う)、
  動作確認・解析用の使い捨てスクリプト置き場(`Claude/`)は `.gitignore` 対象
