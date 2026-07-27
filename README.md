# KurenaiEngine

DirectX 11 / DirectX 12 の両方に対応した自作ゲームエンジン。**KurenaiEngineLibrary.dll**
(共通基盤)・**KurenaiEngine3D.dll**・**KurenaiEngine2D.dll**の3つのDLLに分かれてビルドされます。
すぐに使える完結型API `Kurenai::KurenaiEngine3D`(3D)/`Kurenai::KurenaiEngine2D`(2D)に加え、
RHI(Rendering Hardware Interface)抽象化レイヤーやウィンドウ・カメラ・モデル読み込みといった
低レベルAPI(`Kurenai::RHI` / `Kurenai::Core` / `Kurenai::Assets`、まとめて「Library」)も
KurenaiEngineLibrary.dllから公開しており、独自の描画パイプラインを組みたい場合はこちらを
直接利用できます。3D/2Dは互いに依存しないため、2Dのみのアプリは`KurenaiEngineLibrary.dll` +
`KurenaiEngine2D.dll`だけで動作し、`KurenaiEngine3D.dll`やAssetsを同梱する必要はありません。

`KurenaiEngine3D`はKurenaiEngine専用モデルパッケージ(`.kmodel`)とシーンファイル(`.kscene`)の
読み込み・描画に対応した、Deferred Shading・HDRレンダリング・カスケードシャドウマップ(PCF/PCSS)・
IBL(スカイボックスから焼いた拡散イラディアンス・プリフィルタ済み鏡面による環境光)・
SSAO/SSIL・SSR・複数ライト(ポイント/スポット、カンデラ/ルクス単位)・半透明描画(専用フォワードパスで
アルファブレンド合成)を備えた完結型3Dレンダラーです。
`.gltf`/`.fbx`/`.obj`等のソースモデルは、付属のオフラインツール**KurenaiPacker.exe**で`.kmodel`へ事前変換して
から使います(下記「アセットの準備(KurenaiPacker)」参照)。`KurenaiEngine2D`はスプライト・図形・
テキスト描画を提供する軽量な2D APIです。内部の描画パイプラインや実装判断については
[実装者向けドキュメント](docs/Architecture.html)を参照してください。

## ドキュメント

- **[docs/KurenaiEngine.html](docs/KurenaiEngine.html)** — APIリファレンス。KurenaiEngineを
  使って新しいアプリケーションを作る場合は、まずこちらを参照してください。
- **[docs/Architecture.html](docs/Architecture.html)** — 実装者向けドキュメント。描画パイプラインの
  内部設計や実装判断について知りたい場合はこちらを参照してください。

## 構成

```
KurenaiEngine.sln              ルート: 3つのDLL単体のビルド確認用ソリューション
KurenaiEngine/
  KurenaiEngineLibrary.vcxproj  共通基盤(DynamicLibrary)。RHI抽象化層・Window/Camera・
                                 モデル/シーン読み込み・KurenaiEngineBaseを含む。assimpには依存しない
  KurenaiEngine3D.vcxproj       3D API(DynamicLibrary)。KurenaiEngineLibraryにのみ依存
  KurenaiEngine2D.vcxproj       2D API(DynamicLibrary)。KurenaiEngineLibraryにのみ依存
  Source/Engine/                 公開API(KurenaiEngine3D, KurenaiEngine2D, KurenaiEngineBase, KurenaiTypes.h)
  Source/Library/                公開API(低レベル): RHI抽象化層, Window/Camera, モデル/シーン読み込みなど
  Shaders/3D/                    KurenaiEngine3Dが内部で使うHLSL一式
  Shaders/2D/                    KurenaiEngine2Dが内部で使うHLSL(Sprite2D.hlsl)
Samples/
  Sample3D/  Sample3D.sln       3Dサンプル(KurenaiEngine3Dを使用)。独立ソリューション
             Build/             Sample3D.exeの出力先(Git管理対象外)
  Sample2D/  Sample2D.sln       2Dサンプル(KurenaiEngine2Dを使用)。独立ソリューション
             Build/             Sample2D.exeの出力先(Git管理対象外)
Tools/
  KurenaiPacker/  KurenaiPacker.sln   アセットビルドツール(Application)。独立ソリューション。
                                       assimp/DirectXTexに依存(KurenaiEngineの各DLLとは別依存)
                  Build/               KurenaiPacker.exeの出力先(Git管理対象外)
docs/                           ドキュメント(APIリファレンス・実装者向け)
ThirdParty/                     外部依存ライブラリ(Git Submodule)。imgui, DirectXTex, assimp
Assets/                         アセット(Git管理対象外)
  Source/                        入力。ソースモデル(.gltf/.fbx等)と手書きの.kscene
  Packed/                        出力。KurenaiPacker.exeが生成する.kmodel/.kgeom/.ktexと
                                   検証済みの.kscene。KurenaiEngine3Dが実際に読み込むのはこちら
Build/                          3つのDLL単体の出力先(Git管理対象外)。
                                 Build\Bin\<Platform>\<Configuration>\<プロジェクト名>\ にDLLと、
                                 それが参照するShadersのコピーが揃う
```

KurenaiEngine3D.dll/KurenaiEngine2D.dllが実行時に参照するShadersは、ビルド時のPostBuildEventで
それぞれのDLLと同じフォルダへ自動的にコピーされます。Sample3D/Sample2Dも同様に、自身のビルド後に
必要なDLLとShadersを自分の出力フォルダへコピーするため、各実行ファイルは
`Samples\Sample3D\Build\...` / `Samples\Sample2D\Build\...` 以下だけで単独で動作します。
Sample3Dはさらに`Assets\Packed\`もコピーしますが、Sample2Dは`KurenaiEngine3D.dll`・Assetsのどちらも
必要としないため同梱しません。

## 必要環境

- Windows 10 / 11
- Visual Studio 2022 (「C++によるデスクトップ開発」ワークロード、Windows 10 SDK)
- CMake (Visual Studio付属のもので可)

## セットアップ手順

### 1. Submoduleの取得

```
git submodule update --init --recursive
```

### 2. assimpのビルド (CMake)

glTF・FBXインポータのみを有効にした静的ライブラリとしてビルドします。**assimpはKurenaiEngineの
いずれのDLLにもリンクされず、後述のKurenaiPacker.exe(アセット変換ツール)のビルドにのみ必要**です。
KurenaiEngine本体・サンプルだけをビルドする場合はこの手順は不要です。

```
cmake -S ThirdParty/assimp -B ThirdParty/assimp/build -G "Visual Studio 17 2022" -A x64 ^
  -DBUILD_SHARED_LIBS=OFF ^
  -DASSIMP_BUILD_TESTS=OFF ^
  -DASSIMP_BUILD_ASSIMP_TOOLS=OFF ^
  -DASSIMP_INSTALL=OFF ^
  -DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF ^
  -DASSIMP_BUILD_GLTF_IMPORTER=ON ^
  -DASSIMP_BUILD_FBX_IMPORTER=ON ^
  -DASSIMP_BUILD_ZLIB=ON ^
  -DASSIMP_NO_EXPORT=ON ^
  -DASSIMP_WARNINGS_AS_ERRORS=OFF

cmake --build ThirdParty/assimp/build --config Debug --target assimp
cmake --build ThirdParty/assimp/build --config Release --target assimp
```

### 3. DirectXTexのビルド

`DirectXTex_Desktop_2022.vcxproj` のビルド前処理が `Shaders\CompileShaders.cmd` を実行し、内部で `fxc.exe` をPATH経由で呼び出します。事前にWindows SDKの `fxc.exe` があるディレクトリをPATHに追加してください(例: `C:\Program Files (x86)\Windows Kits\10\bin\<SDKバージョン>\x64`)。

```
MSBuild ThirdParty\DirectXTex\DirectXTex\DirectXTex_Desktop_2022.vcxproj /p:Configuration=Debug /p:Platform=x64
MSBuild ThirdParty\DirectXTex\DirectXTex\DirectXTex_Desktop_2022.vcxproj /p:Configuration=Release /p:Platform=x64
```

### 4. 本体・サンプルのビルド

`KurenaiEngine.sln` は3つのDLL(KurenaiEngineLibrary/KurenaiEngine3D/KurenaiEngine2D)単体の
ビルド確認用です。実際に動かして確認したい場合は、`Samples/Sample3D/Sample3D.sln` または
`Samples/Sample2D/Sample2D.sln` をビルドしてください(Sample3DはKurenaiEngineLibrary+
KurenaiEngine3D、Sample2DはKurenaiEngineLibrary+KurenaiEngine2Dをそれぞれプロジェクト参照して
いるため、必要なDLLも一緒にビルドされます)。

```
MSBuild Samples\Sample3D\Sample3D.sln /p:Configuration=Debug /p:Platform=x64
```

各DLLは `Build\Bin\x64\Debug\<プロジェクト名>\` に、`Sample3D.exe`/`Sample2D.exe`はそれぞれ
`Samples\Sample3D\Build\Bin\x64\Debug\` / `Samples\Sample2D\Build\Bin\x64\Debug\` に出力されます。
Sample3Dの出力フォルダにはKurenaiEngineLibrary.dll・KurenaiEngine3D.dllと、それが参照する
Shaders/Assets(`Assets\Packed\`の中身)が、Sample2Dの出力フォルダにはKurenaiEngineLibrary.dll・
KurenaiEngine2D.dllとSprite2D.hlslのみが自動でコピーされるため、各フォルダはそれだけで完結して
動作します。**この時点では`Assets\Packed\`が空のため、次の「アセットの準備」を行うまでSample3Dは
表示するモデルがありません。**

### 5. アセットの準備(KurenaiPacker)

`KurenaiEngine3D`が読み込めるのは`.kmodel`(KurenaiEngine専用モデルパッケージ)のみです。
`.gltf`/`.fbx`/`.obj`等のソースモデルは`Assets\Source\`に置き、`KurenaiPacker.exe`で`Assets\Packed\`へ
変換してから使います。まずKurenaiPacker自身をビルドします(assimp・DirectXTexのビルドが
事前に必要。手順2・3参照)。

```
MSBuild Tools\KurenaiPacker\KurenaiPacker.sln /p:Configuration=Release /p:Platform=x64
```

モデルをパックするには、入力モデルと`-o`で出力する`.kmodel`のパスを指定します。出力先の
親ディレクトリが、対になる`.kgeom`・参照テクスチャ(`.ktex`)のミラー先ルートになります。

```
Tools\KurenaiPacker\Build\Bin\x64\Release\KurenaiPacker.exe ^
  Assets\Source\Sponza\Sponza.gltf -o Assets\Packed\Sponza\Sponza.kmodel
```

- 既に出力済みの`.ktex`はスキップして高速に再パックします。強制的に再圧縮する場合は`--force`を付けます
- テクスチャ処理は既定で論理コア数(上限8)のワーカースレッドを使って並列に行われます。`--jobs <N>`で変更できます
- 個々のテクスチャの読み込みに失敗しても、そのテクスチャだけフォールバック(白/フラット法線)として扱いパックは続行します
- `.obj`/`.mtl`も入力に使えます。OBJ形式は単位情報を持たないため、センチメートル単位で作成された
  アセットはそのままだと100倍の大きさになります。その場合は`--scale <係数>`で補正してください
  (例: Amazon Lumberyard Bistroの`.obj`配布は`--scale 0.01`)

複数のモデルとカメラ・太陽光の初期値をまとめる`.kscene`(シーンファイル)は、`--scene`を付けて
検証・配置します(書式の詳細は[docs/KurenaiEngine.html](docs/KurenaiEngine.html) 4.7節を参照)。

```
Tools\KurenaiPacker\Build\Bin\x64\Release\KurenaiPacker.exe ^
  --scene Assets\Source\Scenes\Sponza.kscene -o Assets\Packed\Scenes\Sponza.kscene
```

`KurenaiEngine3D`は起動時に`Assets\Packed\Scenes\*.kscene`をファイル名の昇順で列挙し、
ImGuiの「Scenes」パネルに並べます(シーンを追加するには`.kscene`を1つ置くだけです)。

```ini
# .ksceneの記述例(Assets\Source\Scenes\BistroExterior.kscene)
[Scene]
Name = Bistro (McGuire) - Exterior

[Model]
Path = BistroMcGuire/Exterior.kmodel

[Camera]
Position = 21.5, 16.0, -53.5
Yaw = 0.0
```

## 実行(Sample3D)

**起動確認・動作検証には `Sample3D.exe` を使用します。**起動時に`Assets\Packed\Scenes\`内の
`.kscene`をファイル名の昇順で列挙し、先頭のシーンを読み込んで表示します。

既定ではDX11バックエンドで起動します。`-dx12` 引数を付けて起動するとDX12バックエンドを使用します(再ビルド不要でDX11/DX12を比較できます)。現在どちらのバックエンドで動作しているかはウィンドウタイトル(例: `Kurenai Engine [DX12] - Sponza`)と「Scenes」パネルの表示で確認できます。

```
Sample3D.exe -dx12
```

## 操作方法(Sample3D)

| 操作 | 入力 |
| --- | --- |
| 前後移動 | W / S |
| 左右移動 | A / D |
| 上下移動 | E / Q |
| 視点回転 | 右クリックを押しながらマウス移動 |
| 移動速度アップ | Shift (押している間) |
| 各種設定・切り替え | 画面左上のImGuiパネル群(下記) |
| ImGuiパネルの表示/非表示切り替え | F1 |

### ImGuiパネル

画面左上に表示される5つのImGuiパネルから各種設定を変更できます(F1キーで表示/非表示を切り替え可能)。

- **Scenes** — 現在使用中のグラフィックスAPI(DX11/DX12)を表示するほか、シーンの切り替えを行います。ボタンをクリックするとそのシーン(`.kscene`)を読み込みます。一覧は`Assets\Packed\Scenes\*.kscene`から自動的に構築されるため、`.kscene`を追加するだけで一覧に増えます(付属のシーンはSponza、Bistro (McGuire) - Exterior / Interior、White Surface Test(粗さ0〜1の球体列)、Light Test(ポイント/スポット/平行光の検証用シーン)、Multi Model Test(TRS配置の確認用))
- **Post Processing** — AO/間接光のON/OFFと手法(SSAO / SSIL)、各パラメータを調整。シャドウ・IBL・SSRのON/OFFと各パラメータもここで調整できます(IBLはON/OFFに加えて強度も調整可能)。VSync、固定FPSモード(既定でON・60fps。30/60/120から選択可能)もここで切り替えられます
- **Render Targets** — Presentパスで表示する内容をドロップダウンで選択(Final (Lit) / Albedo / Normal / Material / Depth / IBL(拡散イラディアンス・プリフィルタ済み鏡面・BRDF LUT) 等、各パス中間結果のデバッグ表示)
- **Lighting** — 太陽光の時刻(Time of Day)・自動進行(Auto Advance)・方位角(Sun Azimuth)・
  EV100(実在の写真露出値。太陽/環境光/ポイント・スポットライトすべてに一様にかかるシーン全体の
  露出)を調整するほか、ポイント/スポットライトの一覧・追加・削除・型切替・パラメータ編集(強度は
  カンデラ/ルクス)ができます
- **Profiler** — FPS、CPU/GPUフレーム時間を各パスごとに表示

## サンプルプログラム

`Samples/` 以下に、`docs/KurenaiEngine.html` で説明している公開API(`KurenaiEngine3D` /
`KurenaiEngine2D`)を使ったサンプルプログラムを用意しています。それぞれ独立した`.sln`を持ち、
Sample3DはKurenaiEngineLibrary.vcxproj+KurenaiEngine3D.vcxprojを、Sample2Dは
KurenaiEngineLibrary.vcxproj+KurenaiEngine2D.vcxprojをプロジェクト参照します。

### Sample3D

`Kurenai::KurenaiEngine3D` をそのままインスタンス化して `Run()` を呼ぶだけの構成です
(`Samples/Sample3D/Source/Main.cpp`)。表示内容・操作方法は上記「実行」「操作方法」の
とおりです。**起動確認・動作検証はこのSample3Dを使用します。**

### Sample2D

`Kurenai::KurenaiEngine2D` を使い、画面内を跳ね回る半透明の色つきスプライトを描画するサンプルです
(座標系・APIの詳細は `docs/KurenaiEngine.html` 3章を参照)。

| 操作 | 入力 |
| --- | --- |
| 終了 | Esc |

## Assetsフォルダについて

エンジンが読み込むモデル・テクスチャ類は `Assets/` フォルダで管理します。サイズが大きいため
Git管理対象外(`.gitignore`)にしています。`Assets/Source/`(入力)と`Assets/Packed/`(出力)に
分かれており、`KurenaiEngine3D`が実際に読み込むのは常に`Assets/Packed/`側です
(手順5「アセットの準備」参照)。

- `Assets/Source/Sponza/` — [glTF-Sample-Models](https://github.com/KhronosGroup/glTF-Sample-Models) のSponzaモデル(glTF形式)
- `Assets/Source/BistroMcGuire/` — [Amazon Lumberyard Bistro](https://developer.nvidia.com/orca/amazon-lumberyard-bistro)のMorgan McGuire版OBJ配布([awesome-3d-meshes](https://github.com/Graphify-Labs/awesome-3d-meshes)経由)をglTFに変換したもの。変換手順は[実装者向けドキュメント](docs/Architecture.html)を参照
- `Assets/Source/MaterialTest/` — PBRライティング検証用の白色球体列。`Tools/generate_material_test.py` で再生成できる
- `Assets/Source/LightTest/` — ポイント/スポット/平行光の検証用シーン(床・壁・粗さ違いの球4個)。
  `Tools/generate_light_test.py` で再生成できる
- `Assets/Source/Scenes/` — 手書きの`.kscene`(シーンファイル)
- `Assets/Packed/` — 上記をKurenaiPacker.exeで変換した`.kmodel`/`.kgeom`/`.ktex`と、検証済みの`.kscene`
- `Assets/Packed/Skybox/` — 背景表示・IBLの入力となるHDR空キューブマップ(DDS形式、R16G16B16A16_Float、既に圧縮済みのためパッカーを通さず直接ここへ出力する)。`Tools/generate_sky_cubemap.py`(要`pip install numpy`)で再生成できる

`.kmodel`/`.ktex`は元ファイルのタイムスタンプを見て自動生成・自動更新されることはありません
(実行時のディスクキャッシュではなく、KurenaiPacker.exeが生成する配布可能なアセットのため)。
ソースモデルを更新した場合は、KurenaiPacker.exeを再実行してください(`--force`を付けない限り、
既存の`.ktex`はスキップして高速に再パックします)。
