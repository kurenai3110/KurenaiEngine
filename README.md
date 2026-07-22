# KurenaiEngine

DirectX 11 / DirectX 12 対応を目指す自作ゲームエンジン。RHI(Rendering Hardware Interface)抽象化レイヤーの上にDX11バックエンドを実装しており、assimp経由でglTF・FBXモデルの読み込み・描画に対応しています。描画はDeferred Shading(G-Buffer: Albedo/Normal/Metallic-Roughness + 深度)で、ライティングパスでCook-Torrance(GGX)によるPBR(メタリック/ラフネス)計算を行います。法線マッピングの接線は画面空間微分から近似計算しています。

描画パイプラインは ジオメトリパス(G-Buffer書き込み) → ライティングパス(G-Bufferを読みSceneColorへ出力) → Presentパス(SceneColorをバックバッファへ表示) の3パス構成です。G-Buffer/SceneColorの解像度はウィンドウサイズから独立しており(`Application`のコンストラクタ引数、既定は1280x720)、Presentパスでアスペクト比を保ったままウィンドウに収まるよう拡大縮小します(レターボックス/ピラーボックス)。

## 構成

- `Engine/` — エンジン本体(静的ライブラリ)。RHI抽象化レイヤー、DX11バックエンド、モデルローダーなど。
- `Sandbox/` — 動作確認用の実行ファイル。シェーダ(`Shaders/`)を含む。
- `ThirdParty/` — 外部依存ライブラリ(Git Submodule)。imgui, DirectXTex, assimp。
- `Assets/` — エンジンが実際に読み込むモデル・テクスチャなどのアセット。
- `Samples/` — 参考用にダウンロードしたサンプルアセット集(まだ`Assets/`に取り込んでいないものを含む)。
- `Build/` — ビルド生成物の出力先(Git管理対象外)。

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

glTF・FBXインポータのみを有効にした静的ライブラリとしてビルドします。

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

### 4. 本体のビルド

`KurenaiEngine.sln` をVisual Studioで開いてビルドするか、MSBuildで直接ビルドします。

```
MSBuild KurenaiEngine.sln /p:Configuration=Debug /p:Platform=x64
```

生成された実行ファイルは `Build\Bin\x64\Debug\Sandbox.exe` に出力されます。

## 実行

`Sandbox.exe` を実行すると起動時にSponzaを読み込んで表示します。モデルパスは実行ファイルの場所から4階層上をリポジトリルートとみなして解決しているため、`Build\Bin\<Platform>\<Configuration>\` 以外の場所に実行ファイルを配置すると読み込みに失敗します。

## 操作方法

| 操作 | 入力 |
| --- | --- |
| 前後移動 | W / S |
| 左右移動 | A / D |
| 上下移動 | E / Q |
| 視点回転 | 右クリックを押しながらマウス移動 |
| 移動速度アップ | Shift (押している間) |
| 表示アセットの切り替え | 画面左上のImGui「Scenes」パネルのボタン |

### 表示アセット一覧(Scenesパネル)

- Sponza
- Bistro - Exterior
- Bistro - Interior
- Bistro - Interior (Wine Cellar)
- White Surface Test(粗さ0〜1の球体列)

画面左上に常時表示されるImGuiの「Scenes」パネルのボタンをクリックするとそのアセットを読み込みます。現在表示中のアセットに対応するボタンはグレーアウトされます。

切り替え時はモデルとテクスチャを同期的に再読み込みするため、Bistroのような大容量アセットでは数秒〜数十秒ウィンドウが応答しなくなります(2回目以降はモデルキャッシュにより高速化されます)。読み込み完了後、タイトルバーに現在表示中のアセット名が表示されます。

## Assetsフォルダについて

エンジンが読み込むモデル・テクスチャ類は `Assets/` フォルダで管理します。

- `Assets/Sponza/` — [glTF-Sample-Models](https://github.com/KhronosGroup/glTF-Sample-Models) のSponzaモデル(glTF形式)
- `Assets/Bistro/` — [Amazon Lumberyard Bistro](https://developer.nvidia.com/orca/amazon-lumberyard-bistro) のBistroモデル(FBX形式、`Textures/`にDDS/TGAテクスチャを同梱)
- `Assets/MaterialTest/` — PBRライティング検証用に生成した、粗さ(roughness)を0.0〜1.0で11段階に変えた白色(非金属)の球体を並べたglTFアセット。`Tools/generate_material_test.py` で再生成できる
- `Assets/Skybox/` — 背景表示用に生成した青空のキューブマップ(DDS形式)。`Tools/generate_sky_cubemap.py` で再生成できる

`Assets/` と `Samples/` はサイズが大きいためGit管理対象外(`.gitignore`)にしています。

モデルを読み込むと、同じ場所に `<元のファイル名>.kmodelcache` というバイナリキャッシュが自動生成されます。頂点/インデックス/マテリアル参照を解析済みの形で保持しており、2回目以降の読み込みではassimpによる解析をスキップして高速に読み込めます。元のモデルファイルが更新されると自動的に無効化され再生成されるため、手動での削除は基本的に不要です。
