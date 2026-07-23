# KurenaiEngine

DirectX 11 / DirectX 12 の両方に対応した自作ゲームエンジン。RHI(Rendering Hardware Interface)抽象化レイヤーの上にDX11・DX12それぞれのバックエンド(デバイス、コマンドリスト、ImGui連携など)を実装しており、実行時に切り替えられます。assimp経由でglTF・FBXモデルの読み込み・描画に対応しています。描画はDeferred Shading(G-Buffer: Albedo/Normal/Metallic-Roughness + 深度)で、ライティングパスでCook-Torrance(GGX)によるPBR(メタリック/ラフネス)計算を行います。法線マッピングの接線は画面空間微分から近似計算しています。シャドウマッピングによる影の描画、太陽光の昼夜サイクルにも対応しています。

G-Bufferの深度バッファはReverse-Z(浮動小数点フォーマットD32_FLOATを使い、近平面をNDC z=1.0、遠平面をNDC z=0.0にマッピングして深度比較をGREATERで行う)で描画しており、標準的な深度マッピング(近平面=0.0/遠平面=1.0)よりも遠方のZ精度を確保してZファイティングを抑えています。正射影のシャドウマップは元々Zが線形分布のため対象外で、従来どおりのマッピング(D32_FLOAT、近平面=0.0/遠平面=1.0)のままです。

直接光(太陽光のCook-Torrance PBR、シャドウ適用済み)は専用の直接光パスでHDR(R32G32B32A32_Float)のレンダーターゲットへ書き出し、最終合成パスとSSILパスの両方がそれをサンプルする構成になっています。これによりSSILの間接拡散光もシャドウ・PBRの結果と整合の取れた値を反射光源として使えます。

環境光の遮蔽・間接光表現はSSAO(スクリーンスペース・アンビエントオクルージョン)とSSIL(Screen Space Indirect Lighting with Visibility Bitmask)の2手法を実行時に切り替えられます。SSAOはタンジェント空間の半球カーネルサンプリングによる遮蔽率のみを計算するのに対し、SSIL(Visibility Bitmask)はGTAO/HBAOと同様に法線周りのスライスごとにスクリーン空間の水平線サーチを行い、遮蔽を32セクタのビットマスクで表現します。これによりThickness Heuristic(遮蔽物に仮の厚みを持たせる)で薄いオブジェクトの裏に光を回り込ませつつ、新規に隠れたビット数を可視立体角の割合とみなして直接光パスの結果(シャドウ適用済み、環境光は含まない)を間接拡散光として加算します(Olivier Therrien et al. "Screen Space Indirect Lighting with Visibility Bitmask" (2023) を参考にした実装)。

描画パイプラインは シャドウパス(サンライト視点で深度のみ描画) → ジオメトリパス(G-Buffer書き込み) → 直接光パス(G-Buffer・シャドウマップからPBRの直接光をHDRで計算) → AO/GIパス(選択中の手法でNormal/Depth、SSILの場合はAlbedo・直接光バッファも読みAO・間接拡散光を計算しブラー) → 最終合成パス(Albedo・直接光・AO/GI・スカイボックスを合成しトーンマッピングしてSceneColorへ出力) → Presentパス(選択中のデバッグビューをバックバッファへ表示) の6パス構成です。シャドウ・AO/GIはそれぞれON/OFF可能で、OFF時はシャドウパス/AOパスをスキップします。G-Buffer/SceneColorの解像度はウィンドウサイズから独立しており(`Application`のコンストラクタ引数、既定は1280x720)、Presentパスでアスペクト比を保ったままウィンドウに収まるよう拡大縮小します(レターボックス/ピラーボックス)。

## 構成

- `Engine/` — エンジン本体(静的ライブラリ)。RHI抽象化レイヤー、DX11/DX12バックエンド、モデルローダーなど。
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

既定ではDX11バックエンドで起動します。`-dx12` 引数を付けて起動するとDX12バックエンドを使用します(再ビルド不要でDX11/DX12を比較できます)。現在どちらのバックエンドで動作しているかはウィンドウタイトル(例: `Kurenai Engine [DX12] - Sponza`)と「Scenes」パネルの表示で確認できます。

```
Sandbox.exe -dx12
```

## 操作方法

| 操作 | 入力 |
| --- | --- |
| 前後移動 | W / S |
| 左右移動 | A / D |
| 上下移動 | E / Q |
| 視点回転 | 右クリックを押しながらマウス移動 |
| 移動速度アップ | Shift (押している間) |
| 各種設定・切り替え | 画面左上のImGuiパネル群(下記) |

### ImGuiパネル

画面左上に常時表示される4つのImGuiパネルから各種設定を変更できます。

- **Scenes** — 現在使用中のグラフィックスAPI(DX11/DX12)を表示するほか、表示アセットの切り替えを行う。ボタンをクリックするとそのアセットを読み込みます。現在表示中のアセットに対応するボタンはグレーアウトされます。切り替え時はモデルとテクスチャを同期的に再読み込みするため、Bistroのような大容量アセットでは数秒〜数十秒ウィンドウが応答しなくなります(2回目以降はモデルキャッシュにより高速化されます)。読み込み完了後、タイトルバーに現在表示中のアセット名が表示されます。
  - Sponza
  - Bistro - Exterior
  - Bistro - Interior
  - Bistro - Interior (Wine Cellar)
  - White Surface Test(粗さ0〜1の球体列)
- **Post Processing** — AO/間接光のON/OFFと手法(Technique: SSAO / SSIL (Visibility Bitmask))を切り替え。SSAOは半径(Radius)/強さ(Power)、SSILは半径(Radius)/厚み(Thickness)/強さ(Intensity)/AOのコントラスト(AO Power)/スライス数(Slices)/ステップ数(Steps)を調整可能。シャドウのON/OFFもここで切り替え
- **Render Targets** — Presentパスで表示する内容をドロップダウンで選択(Final (Lit) / Albedo / Normal / Material / Depth / Direct Light / AO/GI - Indirect Light (RGB) / AO/GI - Occlusion (Alpha) / Shadow Map)。Direct Lightは直接光パスの結果(HDR)をトーンマッピングして表示。AO/GIバッファはrgb(間接拡散光)とa(遮蔽率)を別々に確認できる
- **Lighting** — 太陽光の時刻(Time of Day, 0〜24時)をスライダーで指定。Auto Advanceを有効にすると時刻が自動で進行(速度をSpeedで調整)

## Assetsフォルダについて

エンジンが読み込むモデル・テクスチャ類は `Assets/` フォルダで管理します。

- `Assets/Sponza/` — [glTF-Sample-Models](https://github.com/KhronosGroup/glTF-Sample-Models) のSponzaモデル(glTF形式)
- `Assets/Bistro/` — [Amazon Lumberyard Bistro](https://developer.nvidia.com/orca/amazon-lumberyard-bistro) のBistroモデル(FBX形式、`Textures/`にDDS/TGAテクスチャを同梱)
- `Assets/MaterialTest/` — PBRライティング検証用に生成した、粗さ(roughness)を0.0〜1.0で11段階に変えた白色(非金属)の球体を並べたglTFアセット。`Tools/generate_material_test.py` で再生成できる
- `Assets/Skybox/` — 背景表示用に生成した青空のキューブマップ(DDS形式)。`Tools/generate_sky_cubemap.py` で再生成できる

`Assets/` と `Samples/` はサイズが大きいためGit管理対象外(`.gitignore`)にしています。

モデルを読み込むと、同じ場所に `<元のファイル名>.kmodelcache` というバイナリキャッシュが自動生成されます。頂点/インデックス/マテリアル参照を解析済みの形で保持しており、2回目以降の読み込みではassimpによる解析をスキップして高速に読み込めます。元のモデルファイルが更新されると自動的に無効化され再生成されるため、手動での削除は基本的に不要です。
