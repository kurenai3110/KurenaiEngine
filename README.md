# KurenaiEngine

DirectX 11 / DirectX 12 の両方に対応した自作ゲームエンジン。**KurenaiEngine.dll** として
ビルドされます。すぐに使える完結型API `Kurenai::KurenaiEngine3D`(3D)/
`Kurenai::KurenaiEngine2D`(2D)に加え、RHI(Rendering Hardware Interface)抽象化レイヤーや
ウィンドウ・カメラ・モデル読み込みといった低レベルAPI(`Kurenai::RHI` / `Kurenai::Core` /
`Kurenai::Assets`、まとめて「Library」)も公開しており、独自の描画パイプラインを組みたい場合は
こちらを直接利用できます。

`KurenaiEngine3D`はassimp経由でglTF・FBXモデルの読み込み・描画に対応した、Deferred Shading・
シャドウ・SSAO/SSIL・SSRを備えた完結型3Dレンダラーです。`KurenaiEngine2D`はスプライト・図形・
テキスト描画を提供する軽量な2D APIです。内部の描画パイプラインや実装判断については
[実装者向けドキュメント](docs/Architecture.html)を参照してください。

## ドキュメント

- **[docs/KurenaiEngine.html](docs/KurenaiEngine.html)** — APIリファレンス。KurenaiEngineを
  使って新しいアプリケーションを作る場合は、まずこちらを参照してください。
- **[docs/Architecture.html](docs/Architecture.html)** — 実装者向けドキュメント。描画パイプラインの
  内部設計や実装判断について知りたい場合はこちらを参照してください。

## 構成

```
KurenaiEngine.sln              ルート: KurenaiEngine(DLL)単体のビルド確認用ソリューション
KurenaiEngine/
  KurenaiEngine.vcxproj        本体(DynamicLibrary)
  Source/Engine/                公開API(KurenaiEngine3D, KurenaiEngine2D, KurenaiTypes.h)
  Source/Library/                公開API(低レベル): RHI抽象化層, Window/Camera, モデル読み込みなど
  Shaders/                       KurenaiEngine3D/2Dが内部で使うHLSL一式
Samples/
  Sample3D/  Sample3D.sln       3Dサンプル(KurenaiEngine3Dを使用)。独立ソリューション
             Build/             Sample3D.exeの出力先(Git管理対象外)
  Sample2D/  Sample2D.sln       2Dサンプル(KurenaiEngine2Dを使用)。独立ソリューション
             Build/             Sample2D.exeの出力先(Git管理対象外)
docs/                           ドキュメント(APIリファレンス・実装者向け)
ThirdParty/                     外部依存ライブラリ(Git Submodule)。imgui, DirectXTex, assimp
Assets/                         KurenaiEngine3Dが読み込むモデル・テクスチャなどのアセット
Build/                          KurenaiEngine.dll単体の出力先(Git管理対象外)。
                                 Build\Bin\<Platform>\<Configuration>\ にDLLと、それが参照する
                                 Shaders/Assetsのコピーが揃う
```

KurenaiEngine.dllが実行時に参照するShaders/Assetsは、ビルド時のPostBuildEventでDLLと
同じフォルダへ自動的にコピーされます。Sample3D/Sample2Dも同様に、自身のビルド後に
KurenaiEngine.dllとShaders/Assetsを自分の出力フォルダへコピーするため、各実行ファイルは
`Samples\Sample3D\Build\...` / `Samples\Sample2D\Build\...` 以下だけで単独で動作します。

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

### 4. 本体・サンプルのビルド

`KurenaiEngine.sln` はKurenaiEngine(DLL)単体のビルド確認用です。実際に動かして確認したい場合は、
`Samples/Sample3D/Sample3D.sln` または `Samples/Sample2D/Sample2D.sln` をビルドしてください
(いずれもKurenaiEngine.vcxprojをプロジェクト参照しているため、KurenaiEngine.dllも一緒に
ビルドされます)。

```
MSBuild Samples\Sample3D\Sample3D.sln /p:Configuration=Debug /p:Platform=x64
```

`KurenaiEngine.dll`は `Build\Bin\x64\Debug\` に、`Sample3D.exe`/`Sample2D.exe`はそれぞれ
`Samples\Sample3D\Build\Bin\x64\Debug\` / `Samples\Sample2D\Build\Bin\x64\Debug\` に出力されます。
いずれのフォルダにもKurenaiEngine.dllと、それが参照するShaders/Assetsが自動でコピーされるため、
各フォルダはそれだけで完結して動作します。

## 実行(Sample3D)

**起動確認・動作検証には `Sample3D.exe` を使用します。**起動時にSponzaを読み込んで表示します。

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

- **Scenes** — 現在使用中のグラフィックスAPI(DX11/DX12)を表示するほか、表示アセットの切り替えを行います。ボタンをクリックするとそのアセットを読み込みます(初回読み込みは時間がかかる場合があります。詳細は「Assetsフォルダについて」参照)。
  - Sponza
  - Bistro (McGuire) - Exterior / Interior
  - White Surface Test(粗さ0〜1の球体列)
- **Post Processing** — AO/間接光のON/OFFと手法(SSAO / SSIL)、各パラメータを調整。シャドウ・SSRのON/OFFと各パラメータもここで調整できます。VSync、固定FPSモード(既定でON・60fps。30/60/120から選択可能)もここで切り替えられます
- **Render Targets** — Presentパスで表示する内容をドロップダウンで選択(Final (Lit) / Albedo / Normal / Material / Depth 等、各パス中間結果のデバッグ表示)
- **Lighting** — 太陽光の時刻(Time of Day)・自動進行(Auto Advance)・方位角(Sun Azimuth)を調整
- **Profiler** — FPS、CPU/GPUフレーム時間を各パスごとに表示

## サンプルプログラム

`Samples/` 以下に、`docs/KurenaiEngine.html` で説明している公開API(`KurenaiEngine3D` /
`KurenaiEngine2D`)を使ったサンプルプログラムを用意しています。それぞれ独立した`.sln`を持ち、
`KurenaiEngine.vcxproj`をプロジェクト参照します。

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
Git管理対象外(`.gitignore`)にしています。

- `Assets/Sponza/` — [glTF-Sample-Models](https://github.com/KhronosGroup/glTF-Sample-Models) のSponzaモデル(glTF形式)
- `Assets/BistroMcGuire/` — [Amazon Lumberyard Bistro](https://developer.nvidia.com/orca/amazon-lumberyard-bistro)のMorgan McGuire版OBJ配布([awesome-3d-meshes](https://github.com/Graphify-Labs/awesome-3d-meshes)経由)をglTFに変換したもの。変換手順は[実装者向けドキュメント](docs/Architecture.html)を参照
- `Assets/MaterialTest/` — PBRライティング検証用の白色球体列。`Tools/generate_material_test.py` で再生成できる
- `Assets/Skybox/` — 背景表示用の青空キューブマップ(DDS形式)。`Tools/generate_sky_cubemap.py` で再生成できる

モデル・テクスチャは初回読み込み時にディスクキャッシュ(`.kmodelcache` / `.ktexcache`)が自動生成され、
2回目以降はそのキャッシュを読むだけになるため高速に読み込めます。元ファイルが更新されると自動的に
再生成されるため、手動でのキャッシュ削除は基本的に不要です。
