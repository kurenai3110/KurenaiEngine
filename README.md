# KurenaiEngine

DirectX 11 / DirectX 12 の両方に対応した自作ゲームエンジン。**KurenaiEngine.dll** として
ビルドされます。すぐに使える完結型API `Kurenai::KurenaiEngine3D`(3D)/
`Kurenai::KurenaiEngine2D`(2D)に加え、RHI(Rendering Hardware Interface)抽象化レイヤー
(DX11・DX12それぞれのバックエンドを実行時に切り替え可能)やウィンドウ・カメラ・モデル読み込みと
いった低レベルAPI(`Kurenai::RHI` / `Kurenai::Core` / `Kurenai::Assets`、まとめて「Library」)も
公開しており、独自の描画パイプラインを組みたい場合はこちらを直接利用できます。RHI層はコンピュート
シェーダー(`RWTexture2D`/`RWStructuredBuffer`によるUAV書き込み)にもDX11/DX12両対応で対応しており
(詳細は[docs/KurenaiEngine.html](docs/KurenaiEngine.html) 4.3.1章を参照)、そのコンピュートシェーダー
対応を使ってHi-Zミップチェーン(深度のミップごとの縮小テクスチャ)を構築するAPIも用意しています
(4.3.2章を参照)。

`KurenaiEngine2D`はスプライト(`DrawSprite`)に加え、円・線分(`DrawCircle`/`DrawLine`)と
簡易なビットマップフォントによるテキスト描画(`DrawText`)も提供します。`KurenaiEngineBase`
(2D/3D共通)は、`GetAsyncKeyState`/`GetCursorPos`のようなグローバル入力ではなく
ウィンドウスコープのメッセージベース入力API(`WasKeyPressed`/`WasMouseButtonPressed`/
`IsMouseOverWindow`/`GetClientMousePosition`)と、XAudio2による簡易なサウンド再生API
(`LoadSound`/`PlaySound`、WAV/PCM)も公開しています(詳細は
[docs/KurenaiEngine.html](docs/KurenaiEngine.html) 3・5・6章を参照)。

`KurenaiEngine3D` はassimp経由でglTF・FBXモデルの読み込み・描画に対応した完結型の
3Dレンダラーです。描画はDeferred Shading(G-Buffer: Albedo/Normal/Metallic-Roughness + 深度)で、
ライティングパスでCook-Torrance(GGX)によるPBR(メタリック/ラフネス)計算を行います。
法線マッピングの接線は画面空間微分から近似計算しています。シャドウマッピングによる影の描画、
太陽光の昼夜サイクルにも対応しています。

G-Bufferの深度バッファはReverse-Z(浮動小数点フォーマットD32_FLOATを使い、近平面をNDC z=1.0、遠平面をNDC z=0.0にマッピングして深度比較をGREATERで行う)で描画しており、標準的な深度マッピング(近平面=0.0/遠平面=1.0)よりも遠方のZ精度を確保してZファイティングを抑えています。正射影のシャドウマップは元々Zが線形分布のため対象外で、従来どおりのマッピング(D32_FLOAT、近平面=0.0/遠平面=1.0)のままです。

直接光(太陽光のCook-Torrance PBR、シャドウ適用済み)は専用の直接光パスでHDR(R32G32B32A32_Float)のレンダーターゲットへ書き出し、最終合成パスとSSILパスの両方がそれをサンプルする構成になっています。これによりSSILの間接拡散光もシャドウ・PBRの結果と整合の取れた値を反射光源として使えます。

環境光の遮蔽・間接光表現はSSAO(スクリーンスペース・アンビエントオクルージョン)とSSIL(Screen Space Indirect Lighting with Visibility Bitmask)の2手法を実行時に切り替えられます。SSAOはタンジェント空間の半球カーネルサンプリングによる遮蔽率のみを計算するのに対し、SSIL(Visibility Bitmask)はGTAO/HBAOと同様に法線周りのスライスごとにスクリーン空間の水平線サーチを行い、遮蔽を32セクタのビットマスクで表現します。これによりThickness Heuristic(遮蔽物に仮の厚みを持たせる)で薄いオブジェクトの裏に光を回り込ませつつ、新規に隠れたビット数を可視立体角の割合とみなして直接光パスの結果(シャドウ適用済み、環境光は含まない)を間接拡散光として加算します(Olivier Therrien et al. "Screen Space Indirect Lighting with Visibility Bitmask" (2023) を参考にした実装)。

金属/滑らかな面の鏡面間接光(環境反射)はSSR(スクリーンスペースリフレクション)で計算します。最終合成パスの出力(SceneColor)を反射先の環境色として再利用し、G-Buffer(Normal/Material/Depth)を使ってワールド空間でレイマーチング(線形マーチ+2分探索によるヒット位置の精密化)を行います。スカイボックスへのフォールバックは、レイが画面内で実際に背景(深度なし)ピクセルへ到達したことを確認できた場合のみ行い、レイが画面外に外れた場合や最大距離まで判定がつかなかった場合は反射を追加しません(その先に何があるか不明なまま空を映り込ませると、洞窟のように周囲が遮蔽された空間でも誤って空が反射してしまうため)。ミップチェーンによるラフネスブラーは行っていないため、粗い面ほど反射の寄与を弱めてノイズ化を防いでいます。

描画パイプラインは シャドウパス(サンライト視点で深度のみ描画) → ジオメトリパス(G-Buffer書き込み) → Hi-Zパス(G-Buffer深度からコンピュートシェーダーでミップチェーンを構築) → 直接光パス(G-Buffer・シャドウマップからPBRの直接光をHDRで計算) → AO/GIパス(選択中の手法でNormal/Depth、SSILの場合はAlbedo・直接光バッファも読みAO・間接拡散光を計算しブラー) → 最終合成パス(Albedo・直接光・AO/GI・スカイボックスを合成しトーンマッピングしてSceneColorへ出力) → SSRパス(SceneColor・G-Bufferから鏡面反射を計算し加算) → Presentパス(選択中のデバッグビューをバックバッファへ表示) の8パス構成です。シャドウ・AO/GI・SSRはそれぞれON/OFF可能で、OFF時は該当パスをスキップします。G-Buffer/SceneColorの解像度はウィンドウサイズから独立しており(`KurenaiEngine3D`のコンストラクタ引数、既定は1280x720)、Presentパスでアスペクト比を保ったままウィンドウに収まるよう拡大縮小します(レターボックス/ピラーボックス)。

Hi-Zパスは、G-Buffer深度(単一ミップ)をコンピュートシェーダーで1x1になるまで縮小し、各ミップが2x2ブロックの最小値を持つミップチェーンを構築します。Reverse-Zでは値が小さいほど遠方を表すため、ブロックの最小値は「そのブロック内で最も遠い可視サーフェス」を意味し、将来のオクルージョンカリングやSSRのレイマーチング高速化の土台として使えます。現時点ではこのミップチェーンを利用する処理は未実装で、Presentパスのデバッグビュー(Render Targets - Hi-Z)でミップごとの内容を確認できるのみです。

## ドキュメント

`KurenaiEngine3D` / `KurenaiEngine2D` / Library(低レベルAPI)のAPIリファレンスは
**[docs/KurenaiEngine.html](docs/KurenaiEngine.html)** にまとめています。KurenaiEngineを使って
新しいアプリケーションを作る場合は、まずこのドキュメントを参照してください。

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
docs/                           APIリファレンス
ThirdParty/                     外部依存ライブラリ(Git Submodule)。imgui, DirectXTex, assimp
ThirdParty/SourceModels/        参考用にダウンロードしたサンプルアセット集(未使用のものを含む)
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

**起動確認・動作検証には `Sample3D.exe` を使用します**(旧`Sandbox.exe`はこのリファクタリングで
`Samples/Sample3D` に統合されました)。起動時にSponzaを読み込んで表示します。モデル・シェーダの
パスは実行中の`KurenaiEngine.dll`自身の場所を基準に解決しているため、`KurenaiEngine.dll`と
`Shaders\` / `Assets\` が同じフォルダに揃ってさえいれば、実行ファイルをどこに配置しても
読み込みに成功します(`Samples\Sample3D\Build\Bin\<Platform>\<Configuration>\` はビルド時に
自動でこの構成になります)。

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

- **Scenes** — 現在使用中のグラフィックスAPI(DX11/DX12)を表示するほか、表示アセットの切り替えを行う。ボタンをクリックするとそのアセットを読み込みます。現在表示中のアセットに対応するボタンはグレーアウトされます。切り替え時はモデルとテクスチャを同期的に再読み込みするため、Bistroのような大容量アセットでは数秒〜数十秒ウィンドウが応答しなくなります(2回目以降はモデルキャッシュにより高速化されます)。読み込み完了後、タイトルバーに現在表示中のアセット名が表示されます。
  - Sponza
  - Bistro - Exterior
  - Bistro - Interior
  - Bistro - Interior (Wine Cellar)
  - White Surface Test(粗さ0〜1の球体列)
- **Post Processing** — AO/間接光のON/OFFと手法(Technique: SSAO / SSIL (Visibility Bitmask))を切り替え。SSAOは半径(Radius)/強さ(Power)、SSILは半径(Radius)/厚み(Thickness)/強さ(Intensity)/AOのコントラスト(AO Power)/スライス数(Slices)/ステップ数(Steps)を調整可能。シャドウのON/OFFもここで切り替え。SSRのON/OFFと最大レイ距離(Max Distance)/ヒット判定の厚み(Thickness)/ラフネスカットオフ(Roughness Cutoff)もここで調整可能
- **Render Targets** — Presentパスで表示する内容をドロップダウンで選択(Final (Lit) / Albedo / Normal / Material / Depth / Depth (Raw) / Direct Light / AO/GI - Indirect Light (RGB) / AO/GI - Indirect Light (RGB, Before Blur) / AO/GI - Occlusion (Alpha) / AO/GI - Occlusion (Alpha, Before Blur) / Shadow Map / SSR (Final + Reflections) / Hi-Z (Depth Mip Chain))。Direct Lightは直接光パスの結果(HDR)をトーンマッピングして表示。Depth (Raw)は深度テクスチャの生値(0〜1)を加工せずそのまま表示(reverse-zの生値確認用。近平面が小さいためほとんどの距離で値が0付近になり、無加工ではほぼ黒く見える)。AO/GIバッファはrgb(間接拡散光)とa(遮蔽率)を別々に確認でき、Before Blur付きの項目はブラー前の生バッファ(タイル状ノイズが乗った状態)を表示する。Finalと同じくSSR無効時はSceneColorがそのまま表示される。Hi-Zを選択するとミップレベルを指定するスライダーが表示され、Hi-Zミップチェーンの指定ミップの生値をグレースケール表示する(ミップが上がるほど解像度が半分ずつになりレターボックス表示も追従する)
- **Lighting** — 太陽光の時刻(Time of Day, 0〜24時)をスライダーで指定。Auto Advanceを有効にすると時刻が自動で進行(速度をSpeedで調整)
- **Profiler** — FPS(指数移動平均)、CPUフレーム時間(Update+Render呼び出し時間)、GPUフレーム時間と各パス(Shadow/GBuffer/HiZ/DirectLight/AO/AOBlur/Lighting/SSR/Present)ごとのGPU実行時間をGPUタイムスタンプクエリで計測して表示。GPU側の計測はDX11/DX12とも数フレーム遅れの値が表示される(AO/AOBlurはAO/間接光が無効の間、SSRはSSRが無効の間は表示されない)

## サンプルプログラム

`Samples/` 以下に、`docs/KurenaiEngine.html` で説明している公開API(`KurenaiEngine3D` /
`KurenaiEngine2D`)を使ったサンプルプログラムを用意しています。それぞれ独立した`.sln`を持ち、
`KurenaiEngine.vcxproj`をプロジェクト参照します。実行ファイルの出力先は各サンプル自身の
`Samples\Sample3D\Build\Bin\x64\<Configuration>\` / `Samples\Sample2D\Build\Bin\x64\<Configuration>\`
で、ビルド後処理でKurenaiEngine.dllとShaders/Assetsが同じフォルダへコピーされます。

### Sample3D

`Kurenai::KurenaiEngine3D` をそのままインスタンス化して `Run()` を呼ぶだけの構成です
(`Samples/Sample3D/Source/Main.cpp`)。表示内容・操作方法は上記「実行」「操作方法」の
とおりで、旧 `Sandbox` と同じ内容(Deferred Shading・シャドウ・SSAO/SSIL・SSR・
ImGuiパネル一式)がそのまま動作します。**起動確認・動作検証はこのSample3Dを使用します。**

### Sample2D

`Kurenai::KurenaiEngine2D` を使い、画面内を跳ね回る半透明の色つきスプライトを描画するサンプルです。
単位クアッド1つを使い回し、スプライトごとの位置・大きさ・回転・色は `DrawSprite` の引数として
毎フレーム渡します(座標系・APIの詳細は `docs/KurenaiEngine.html` 3章を参照)。

| 操作 | 入力 |
| --- | --- |
| 終了 | Esc |

## Assetsフォルダについて

エンジンが読み込むモデル・テクスチャ類は `Assets/` フォルダで管理します。

- `Assets/Sponza/` — [glTF-Sample-Models](https://github.com/KhronosGroup/glTF-Sample-Models) のSponzaモデル(glTF形式)
- `Assets/Bistro/` — [Amazon Lumberyard Bistro](https://developer.nvidia.com/orca/amazon-lumberyard-bistro) のBistroモデル(FBX形式、`Textures/`にDDS/TGAテクスチャを同梱)
- `Assets/MaterialTest/` — PBRライティング検証用に生成した、粗さ(roughness)を0.0〜1.0で11段階に変えた白色(非金属)の球体を並べたglTFアセット。`Tools/generate_material_test.py` で再生成できる
- `Assets/Skybox/` — 背景表示用に生成した青空のキューブマップ(DDS形式)。`Tools/generate_sky_cubemap.py` で再生成できる

`Assets/` と `ThirdParty/SourceModels/` はサイズが大きいためGit管理対象外(`.gitignore`)にしています。

モデルを読み込むと、同じ場所に `<元のファイル名>.kmodelcache` というバイナリキャッシュが自動生成されます。頂点/インデックス/マテリアル参照を解析済みの形で保持しており、2回目以降の読み込みではassimpによる解析をスキップして高速に読み込めます。元のモデルファイルが更新されると自動的に無効化され再生成されるため、手動での削除は基本的に不要です。
