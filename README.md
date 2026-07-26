# KurenaiEngine

DirectX 11 / DirectX 12 の両方に対応した自作ゲームエンジン。**KurenaiEngine.dll** として
ビルドされます。すぐに使える完結型API `Kurenai::KurenaiEngine3D`(3D)/
`Kurenai::KurenaiEngine2D`(2D)に加え、RHI(Rendering Hardware Interface)抽象化レイヤーや
ウィンドウ・カメラ・モデル読み込みといった低レベルAPI(`Kurenai::RHI` / `Kurenai::Core` /
`Kurenai::Assets`、まとめて「Library」)も公開しており、独自の描画パイプラインを組みたい場合は
こちらを直接利用できます。

`KurenaiEngine3D`はKurenaiEngine専用モデルパッケージ(`.kmodel`)とシーンファイル(`.kscene`)の
読み込み・描画に対応した、Deferred Shading・シャドウ・SSAO/SSIL・SSRを備えた完結型3Dレンダラーです。
`.gltf`/`.fbx`等のソースモデルは、付属のオフラインツール**KurenaiPacker.exe**で`.kmodel`へ事前変換して
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
KurenaiEngine.sln              ルート: KurenaiEngine(DLL)単体のビルド確認用ソリューション
KurenaiEngine/
  KurenaiEngine.vcxproj        本体(DynamicLibrary)。assimpには依存しない
  Source/Engine/                公開API(KurenaiEngine3D, KurenaiEngine2D, KurenaiTypes.h)
  Source/Library/                公開API(低レベル): RHI抽象化層, Window/Camera, モデル/シーン読み込みなど
  Shaders/                       KurenaiEngine3D/2Dが内部で使うHLSL一式
Samples/
  Sample3D/  Sample3D.sln       3Dサンプル(KurenaiEngine3Dを使用)。独立ソリューション
             Build/             Sample3D.exeの出力先(Git管理対象外)
  Sample2D/  Sample2D.sln       2Dサンプル(KurenaiEngine2Dを使用)。独立ソリューション
             Build/             Sample2D.exeの出力先(Git管理対象外)
Tools/
  KurenaiPacker/  KurenaiPacker.sln   アセットビルドツール(Application)。独立ソリューション。
                                       assimp/DirectXTexに依存(KurenaiEngine.dllとは別依存)
                  Build/               KurenaiPacker.exeの出力先(Git管理対象外)
docs/                           ドキュメント(APIリファレンス・実装者向け)
ThirdParty/                     外部依存ライブラリ(Git Submodule)。imgui, DirectXTex, assimp
Assets/                         アセット(Git管理対象外)
  Source/                        入力。ソースモデル(.gltf/.fbx等)と手書きの.kscene
  Packed/                        出力。KurenaiPacker.exeが生成する.kmodel/.kgeom/.ktexと
                                   検証済みの.kscene。KurenaiEngine3Dが実際に読み込むのはこちら
Build/                          KurenaiEngine.dll単体の出力先(Git管理対象外)。
                                 Build\Bin\<Platform>\<Configuration>\ にDLLと、それが参照する
                                 Shaders/Assets(Packed)のコピーが揃う
```

KurenaiEngine.dllが実行時に参照するShaders/Assets(`Assets\Packed\`の中身)は、ビルド時の
PostBuildEventでDLLと同じフォルダへ自動的にコピーされます。Sample3D/Sample2Dも同様に、自身の
ビルド後にKurenaiEngine.dllとShaders/Assetsを自分の出力フォルダへコピーするため、各実行ファイルは
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

glTF・FBXインポータのみを有効にした静的ライブラリとしてビルドします。**assimpは
`KurenaiEngine.dll`にはリンクされず、後述のKurenaiPacker.exe(アセット変換ツール)のビルドに
のみ必要**です。KurenaiEngine本体・サンプルだけをビルドする場合はこの手順は不要です。

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
いずれのフォルダにもKurenaiEngine.dllと、それが参照するShaders/Assets(`Assets\Packed\`の中身)が
自動でコピーされるため、各フォルダはそれだけで完結して動作します。**この時点では`Assets\Packed\`が
空のため、次の「アセットの準備」を行うまでSample3Dは表示するモデルがありません。**

### 5. アセットの準備(KurenaiPacker)

`KurenaiEngine3D`が読み込めるのは`.kmodel`(KurenaiEngine専用モデルパッケージ)のみです。
`.gltf`/`.fbx`等のソースモデルは`Assets\Source\`に置き、`KurenaiPacker.exe`で`Assets\Packed\`へ
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

- **Scenes** — 現在使用中のグラフィックスAPI(DX11/DX12)を表示するほか、シーンの切り替えを行います。ボタンをクリックするとそのシーン(`.kscene`)を読み込みます。一覧は`Assets\Packed\Scenes\*.kscene`から自動的に構築されるため、`.kscene`を追加するだけで一覧に増えます(付属のシーンはSponza、Bistro (McGuire) - Exterior / Interior、White Surface Test(粗さ0〜1の球体列)、Multi Model Test(TRS配置の確認用))
- **Post Processing** — AO/間接光のON/OFFと手法(SSAO / SSIL)、各パラメータを調整。シャドウ・SSRのON/OFFと各パラメータもここで調整できます
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
Git管理対象外(`.gitignore`)にしています。`Assets/Source/`(入力)と`Assets/Packed/`(出力)に
分かれており、`KurenaiEngine3D`が実際に読み込むのは常に`Assets/Packed/`側です
(手順5「アセットの準備」参照)。

- `Assets/Source/Sponza/` — [glTF-Sample-Models](https://github.com/KhronosGroup/glTF-Sample-Models) のSponzaモデル(glTF形式)
- `Assets/Source/BistroMcGuire/` — [Amazon Lumberyard Bistro](https://developer.nvidia.com/orca/amazon-lumberyard-bistro)のMorgan McGuire版OBJ配布([awesome-3d-meshes](https://github.com/Graphify-Labs/awesome-3d-meshes)経由)をglTFに変換したもの。変換手順は[実装者向けドキュメント](docs/Architecture.html)を参照
- `Assets/Source/MaterialTest/` — PBRライティング検証用の白色球体列。`Tools/generate_material_test.py` で再生成できる
- `Assets/Source/Scenes/` — 手書きの`.kscene`(シーンファイル)
- `Assets/Packed/` — 上記をKurenaiPacker.exeで変換した`.kmodel`/`.kgeom`/`.ktex`と、検証済みの`.kscene`
- `Assets/Packed/Skybox/` — 背景表示用の青空キューブマップ(DDS形式、既に圧縮済みのためパッカーを通さず直接ここへ出力する)。`Tools/generate_sky_cubemap.py` で再生成できる

`.kmodel`/`.ktex`は元ファイルのタイムスタンプを見て自動生成・自動更新されることはありません
(実行時のディスクキャッシュではなく、KurenaiPacker.exeが生成する配布可能なアセットのため)。
ソースモデルを更新した場合は、KurenaiPacker.exeを再実行してください(`--force`を付けない限り、
既存の`.ktex`はスキップして高速に再パックします)。
