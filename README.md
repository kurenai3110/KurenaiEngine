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
IBL(スカイボックスから焼いたプリフィルタ済み鏡面による環境光。拡散イラディアンスもその最終ミップから得る)・
スペキュラのマルチスキャッタリング・エネルギー補正・
DDGI(プローブ格子による位置ごとの拡散グローバルイルミネーション)・
SSAO/SSIL・SSR・複数ライト(ポイント/スポット、カンデラ/ルクス単位)・
ポイント/スポットライトのスクリーンスペースシャドウ(シャドウマップを使わない接触影)・
タイルベースのライトカリング・半透明描画(専用フォワードパスで
アルファブレンド合成)・トーンマッピング(AgX / ACES / Reinhardから選択)・自動露出・ブルーム・
昼夜サイクル(GPUで手続き生成する空、月光、太陽の移動に追従するIBLの動的再ベイク)を備えた
完結型3Dレンダラーです。
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
Scenes/                         手書きの.kscene(シーンファイル)。小さなテキストのためGit管理対象
Assets/                         アセット(Git管理対象外)
  Source/                        入力。ソースモデル(.gltf/.fbx等)
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

glTF・FBX・OBJインポータのみを有効にした静的ライブラリとしてビルドします。**assimpはKurenaiEngineの
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
  -DASSIMP_BUILD_OBJ_IMPORTER=ON ^
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
`.kscene`はリポジトリの`Scenes\`に含まれているため、clone直後に自分で書く必要はありません
(`Assets\Packed\Scenes\`へ配置するこのコマンドだけを実行してください)。

```
Tools\KurenaiPacker\Build\Bin\x64\Release\KurenaiPacker.exe ^
  --scene Scenes\Sponza.kscene -o Assets\Packed\Scenes\Sponza.kscene
```

`KurenaiEngine3D`は起動時に`Assets\Packed\Scenes\*.kscene`をファイル名の昇順で列挙し、
ImGuiの「シーン」パネルに並べます(シーンを追加するには`.kscene`を1つ置くだけです)。

```ini
# .ksceneの記述例(Scenes\BistroExterior.kscene)
[Scene]
Name = Bistro (McGuire) - Exterior

[Model]
Path = BistroMcGuire/Exterior.kmodel

[Camera]
Position = 21.5, 16.0, -53.5
Yaw = 0.0
```

`[ReflectionProbe]`セクションを書くと、その位置から周囲をキャプチャした環境マップ(反射プローブ)を
配置できます。屋内など、シーン全体で共通のスカイボックス由来の環境光では空が映り込んでしまう場所で、
実際の壁・柱・床が映るようになります。セクションを繰り返して複数(最大8個)配置できます。

影響範囲の形は球(`Sphere`)と直方体(`Box`)から選べます。

```ini
[ReflectionProbe]
Name = Atrium          # 省略可(ImGuiの一覧に表示される名前)
Position = 0.0, 5.0, 0.0
Shape = Sphere         # 省略可(既定Sphere)。SphereまたはBox
Radius = 50.0          # Shape = Sphere のときの影響範囲の半径
BlendDistance = 2.0    # 省略可(既定2.0)。影響範囲の境界から内側へこの距離をかけて
                       # プローブの重みが1まで立ち上がる。0にすると境界で不連続に切り替わる
```

```ini
[ReflectionProbe]
Name = Atrium West
Position = -8.0, 5.0, 0.0
Shape = Box
BoxExtents = 10.0, 6.5, 10.0  # 各軸の半径(ハーフエクステント)。Positionが箱の中心
Yaw = 0.0                     # 省略可(既定0)。Y軸まわりの回転(度)
BlendDistance = 1.0
```

`Shape = Box`にすると、影響範囲が部屋の形に沿うようになるだけでなく、**視差補正**が有効になります。
プローブのキューブマップは1点から撮ったものなので、プローブ位置から離れた壁際では映る像の位置が
本来の反射位置とずれます。視差補正は反射ベクトルをこの箱と交差させて、その交点の方向で
キューブマップを引き直すことでずれを打ち消します。`BoxExtents`は部屋の壁とおおよそ一致させてください
(箱を実際の壁から大きく離すと補正がかえってずれます)。

影響範囲が重なっているプローブ同士は`BlendDistance`に応じて滑らかに混ざります。どのプローブの
重みも1に満たない場所では、残りをスカイボックス由来のグローバルIBLが埋めるため、影響範囲の外へ
出るときも境界で切り替わらず徐々に元の環境光へ戻ります。

`[GIVolume]`セクションを書くと、その直方体の中にプローブを格子状に敷き詰め、**位置ごとに違う
拡散の間接光(DDGI)**を与えられます。反射プローブが鏡面の映り込みを担うのに対し、こちらは
「壁の色が床へ回り込む」ような間接拡散光を担当します。これが無いと、密閉された室内の壁にも
遮られていない空の環境光がそのまま乗ります。

```ini
[GIVolume]
Name = Hall                    # 省略可(ImGuiに表示される名前)
Origin = -11.0, 0.5, -5.0      # ボリュームの最小コーナー(中心ではありません)
ProbeSpacing = 2.0, 1.75, 2.0  # 各軸のプローブ間隔
ProbeCounts = 13, 5, 7         # 各軸のプローブ数(それぞれ2以上)。この例で 13x5x7 = 455 個
NormalBias = 0.25              # 省略可(既定0.25)。遮蔽判定の位置を面から浮かせる量
ViewBias = 0.10                # 省略可(既定0.10)。同じく視線方向へ寄せる量
Hysteresis = 0.97              # 省略可(既定0.97)。1に近いほど滑らかに追従するが収束は遅くなる
MaxRayDistance = 8.0           # 省略可(既定8.0)。記録する距離の上限。プローブ間隔の数倍が目安
```

プローブ位置は`Origin + (i, j, k) * ProbeSpacing`です。壁の内側にプローブが1個も入らないと
そこだけ間接光が外挿になるため、`Origin`は壁から少し内側に置いてください。
`NormalBias`を0に近づけると画面全体が一様に暗くなります(面が、自分を照らしているプローブから
見えていないと誤判定するためで、強度で持ち上げるのではなくこのバイアスで直します)。

ボリュームは現状**1個だけ**使われます(複数書いた場合は2つ目以降を警告付きで無視します)。
明るさは時間をかけて収束します(バウンスが1フレームに1回ずつ積み上がるため、
455プローブの既定設定でおよそ数十秒)。

## 実行(Sample3D)

**起動確認・動作検証には `Sample3D.exe` を使用します。**起動時に`Assets\Packed\Scenes\`内の
`.kscene`をファイル名の昇順で列挙し、先頭のシーンを読み込んで表示します。

既定ではDX11バックエンドで起動します。`-dx12` 引数を付けて起動するとDX12バックエンドを使用します(再ビルド不要でDX11/DX12を比較できます)。現在どちらのバックエンドで動作しているかはウィンドウタイトル(例: `Kurenai Engine [DX12] - Sponza`)とImGuiのメニューバーの表示で確認できます。

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
| 各種設定・切り替え | ImGuiパネル群(下記) |
| ImGuiパネルの表示/非表示切り替え | F1 |
| スライダーへ数値を直接入力 | Ctrl + クリック |
| 設定を既定値に戻す | 項目を右クリック |
| 項目の説明を見る | 項目にマウスを置く |

テキスト入力欄を編集している間はWASD等のキー入力がカメラ移動に取られません(F1だけは常に効きます)。入力欄からフォーカスを外すにはEscを押すか、別の場所をクリックします。

### ImGuiパネル

8つのパネルから各種設定を変更できます(F1キーで表示/非表示を切り替え可能)。

パネルはドッキングに対応しています。タイトルバーをドラッグすると画面端へ吸着したり、別のパネルへ重ねてタブにまとめたりできます。配置とサイズは実行ファイルと同じフォルダの`imgui.ini`へ自動保存され、次回起動時に復元されます。元に戻したいときはメニューバーの「ウィンドウ」→「レイアウトを初期化」、または「システム」パネルの同名のボタンを使います(閉じたパネルも一緒に戻ります)。中央の何も置かれていない領域には3D映像がそのまま表示されます。

- **シーン** — シーンの切り替えを行います。ボタンをクリックするとそのシーン(`.kscene`)を読み込みます。一覧は`Assets\Packed\Scenes\*.kscene`から自動的に構築されるため、`.kscene`を追加するだけで一覧に増えます(付属のシーンはSponza、Bistro (McGuire) - Exterior / Interior、Bistro (McGuire) - Interior (Lit)(内装に照明15灯と反射プローブ3個を配置し、反射プローブ・スクリーンスペースシャドウ・タイルライトカリングを実際のモデルの上で確認できるシーン)、White Furnace Test(スペキュラBRDFのエネルギー保存を目視で検証するシーン)、Material Test(粗さ0〜1の球体列+半透明ガラス球)、Light Test(ポイント/スポット/平行光の検証用シーン)、Reflection Probe Test(反射プローブの効果を目視で確認するシーン)、Multi Model Test(TRS配置の確認用))。使用中のグラフィックスAPI(DX11/DX12)はメニューバーに常時表示されます
- **レンダリング** — シーンをどう照らすかに関わる品質設定を、次の6つの節に分けて調整します
  - **AO / 間接光** — ON/OFFと手法(SSAO / SSIL)、各パラメータ
  - **シャドウ** — 平行光のシャドウマップのON/OFFとPCSSのライトサイズ
  - **スクリーンスペースシャドウ** — ポイント/スポットライトの影。シャドウマップを使わず、深度バッファをライトの方向へレイマーチして求めます。ON/OFF・レイのステップ数・最大レイ長・厚み・法線バイアス・画面端フェード・1ピクセルあたりに影を落とすライト数の上限を調整できます。深度バッファに写っている面しか遮蔽物として扱えないため、画面外の物や手前の面に隠れた物は影を落としません(得られるのは接触影・中距離の遮蔽です)。どのライトが影を落とすかは「ライティング」パネルのライトごとの設定で決めます
  - **IBL / 環境光** — ON/OFFと強度。拡散イラディアンスは既定でプリフィルタ済み鏡面の最終ミップから得ますが、検証用に専用のイラディアンスマップへ切り替えることもできます。スペキュラBRDFのマルチスキャッタリング・エネルギー補正もここです
  - **スクリーンスペース反射** — ON/OFFと最大距離・厚み・粗さのしきい値。画面内で反射先が見つかった場合にその色で反射を差し替えます。見つからなかった場合は反射プローブ(またはスカイボックス由来のIBL)の結果がそのまま残るため、ON/OFFで全体の明るさは変わらず、反射の鮮明さだけが変わります
  - **タイルドライトカリング** — 画面を16x16ピクセルのタイルに分け、タイルごとに「そのタイルに届くライト」の一覧を作ってライティングの走査対象を絞ります。見た目を変えない最適化のため、ON/OFFで画像は変わりません
- **ポストプロセス** — トーンマップ(カーブはAgX / ACES / Reinhardから選択。既定はAgX。薄明視と出力ディザリングもここ)、ブルーム(強度・しきい値・ソフトニー)、自動露出(EV100の上下限・露出補正・測光値の上側クランプ・夜のロールオフ量・明順応/暗順応の速度・測光に使うパーセンタイル範囲)
  - **薄明視**(既定0=無効)は暗所視の再現量です。暗くなると目は色を判別できない桿体だけの視覚へ移るため、露出を下げるだけでは「暗いが色鮮やかな夜」になり肉眼の見え方と合いません。桿体の分光感度が短波長寄りなことから来るプルキンエ現象(赤が沈み青が明るく見える)も入ります。効果が強く好みが分かれるため既定は無効です
  - 自動露出の測光では**空(背景)を数えません**。空は被写体ではなく光源であり、これを数えると空が画面に占める割合で露出が2〜3.5段も振れてしまう(写真でいう逆光の露出問題)ためです。あわせて画作り用の調整項目が2つあります。**夜のロールオフ**(既定4.5EV)は暗いシーンをわざと暗いまま写すための補正量で、既定値は「肉眼で見た月明かりの夜」になるよう調整してあります(0にすると常に中庸なグレーへ合わせる挙動)。**測光上限(基準EVから)**(既定+2.0EV)は測光値が「太陽・月・空の照度から求めた、構図に依存しない基準EV」から何段上まで行くのを許すかで、明るい看板が画面の大半を占めるような極端なケースの保険です(16まで上げると無効)
- **ライティング** — 太陽(時刻・自動進行・方位角・太陽光のON/OFF)、月の位置、手続き空、シーン全体の露出、ライトの一覧と編集を行います
  - **月の方位角 / 仰角** は**時刻に連動せず、ここで指定した固定位置に居続けます**。実際の月は太陽と独立した周期で動くため手動指定にしてあります。仰角が0度以下なら月は地平線下にあり月光は出ません。平行光源の枠は太陽と共有しており、太陽が沈むと支配ライトが月へ切り替わります
  - **手続き空** はGPUで手続き生成する空のON/OFFです。無効にするとオフラインで焼いた`Sky.dds`に戻ります。なお`.kscene`で`[Scene]Skybox`を明示しているシーンでは、このトグルに関わらず常にそのDDSが使われます
  - **EV100** は実在の写真露出値です。太陽/環境光/ポイント・スポットライトすべてに一様にかかるシーン全体の露出で、自動露出が有効なときはバッファの数値レンジを決める基準値として働きます。シーン全体の自発光の強度倍率もここです
  - ポイント/スポットライトの一覧・追加・複製・削除・種別の切り替え・パラメータ編集(強度はカンデラ/ルクス)ができます。ライトごとにスクリーンスペースシャドウを落とすかも切り替えられます
- **レンダリング → DDGI (拡散グローバルイルミネーション)** — DDGIのON/OFF・強度・1フレームに更新するプローブ数。`.kscene`に`[GIVolume]`があるシーンでのみ動作します。無効にすると拡散の環境光が従来どおりグローバルIBL/反射プローブのイラディアンスに戻ります(加算ではなく差し替えです)
- **反射プローブ** — 反射プローブのON/OFFと、プローブの一覧・追加・削除・位置や影響範囲(形状(球/箱)・半径・箱の半径・箱の向き・ブレンド距離)の編集ができます。プローブの中身はシーンのジオメトリやライトに依存するため、シーン読み込み時と位置を動かしたときに自動で焼き直されます(影響範囲だけを変えた場合は焼き直し不要です)。ライトや時刻を変えた後に焼き直したい場合は「焼き直す」ボタンを押してください
  - 「更新モード」で焼き直しの方式を切り替えられます — **焼き込み**(シーン読み込み時と「焼き直す」ボタンのみ。実行時コストはゼロですが、ライトや時刻を動かしても反射は焼いた時点のまま止まります)、**変化を検出して焼き直す**(太陽・時刻・ライトの変化を検出して自動で焼き直す)、**毎フレーム1面ずつ**(毎フレーム1面ずつ焼き直し、6面揃うごとに次のプローブへ回る)
  - 「視差補正」「プローブのブレンド」は視差補正・プローブ間ブレンドの有無を切り替えるもので、無効にしたときの見た目(壁際での反射位置のずれ、影響範囲の境界の継ぎ目)と見比べられます
  - 「距離キューブを使う」は視差補正の方式です。有効にするとキャプチャ時に一緒に焼いた距離を辿って実際の形状に反射を当てます(無効なら「部屋を直方体とみなす」従来の方式)。強く曲がった鏡面で反射像が二重に割れるのが軽減される一方、距離の解像度に由来する階段状のエッジが反射に乗るため**既定は無効**です
  - 「遮蔽判定(光漏れの抑制)」も同じ距離を使い、プローブから見えない位置(壁の向こう)のピクセルでそのプローブの寄与を落とします。ただしプローブが少ないうちは落ちた分をより明るい空由来のIBLが埋めるため、物体の真下が逆に明るくなることがあり**既定は無効**です
- **デバッグ表示** — Presentパスで表示する内容をドロップダウンで選択(最終結果 / アルベド / 法線 / マテリアル / 深度 / IBL(プリフィルタ済み鏡面・BRDF LUT・検証用の拡散イラディアンス) / ブルーム / ライトタイル(タイルごとのライト数のヒートマップ) / 反射プローブ(キャプチャ結果・影響範囲の色分け・プローブから見た距離) / DDGI(イラディアンスと距離モーメントのオクタヘドラルアトラス)等、各パス中間結果のデバッグ表示)。間接光のように値が小さいバッファを見るための輝度倍率も指定できます。中間バッファの精度構成(HDR / Legacy 8bit)を切り替えて画質を比較することもできます
- **システム** — 垂直同期、フレームレート制限(既定でON・60fps。30/60/120から選択可能)、パネルの表示/非表示、レイアウトの初期化、現在のUIスケール
- **プロファイラ** — FPS、CPU/GPUフレーム時間をパスごとに表示

**UIの大きさはWindowsのディスプレイ設定の拡大率に追従します。** ウィンドウ自体の大きさも、ディスプレイを移動したときのWindowsの既定の挙動に任せているため、両者が同じ比率で変化します。現在のUIスケールとWindowsの拡大率は「システム」パネルで確認できます。

UI全体をもう少し大きく/小さくしたい場合は`KurenaiEngine/Source/Engine/UI/UITheme.h`の`kUIScaleMultiplier`を変更してビルドし直してください。
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
(手順5「アセットの準備」参照)。手書きの`.kscene`だけは`Assets/`の外の`Scenes/`にあり、
こちらはリポジトリに含まれています。

- `Assets/Source/Sponza/` — [glTF-Sample-Models](https://github.com/KhronosGroup/glTF-Sample-Models) のSponzaモデル(glTF形式)
- `Assets/Source/BistroMcGuire/` — [Amazon Lumberyard Bistro](https://developer.nvidia.com/orca/amazon-lumberyard-bistro)のMorgan McGuire版OBJ配布([awesome-3d-meshes](https://github.com/Graphify-Labs/awesome-3d-meshes)経由、`ThirdParty/SourceModels/`内の7z/zipアーカイブを展開したもの)。
  KurenaiPacker内蔵のassimp OBJインポータで直接パックする(`--scale 0.01`が必要)。詳細・展開手順は[実装者向けドキュメント](docs/Architecture.html)11章を参照
- `Assets/Source/FurnaceTest/` — White Furnace Test用の金属球列(`metallic=1.0`、粗さ0.0〜1.0の11個)。
  一様な放射輝度のキューブマップ(`Assets/Packed/Skybox/UniformWhite.dds`)と合わせて
  `Tools/generate_furnace_test.py` で再生成できる
- `Assets/Source/MaterialTest/` — PBRライティング検証用の球体列。粗さ0.0〜1.0の白色球11個と、
  半透明描画(`alphaMode=BLEND`)検証用の赤いガラス球1個。参照するテクスチャ(`GlassRed.png`)も含めて
  `Tools/generate_material_test.py` で再生成できる
- `Assets/Source/LightTest/` — ポイント/スポット/平行光の検証用シーン(床・壁・粗さ違いの球4個)。
  `Tools/generate_light_test.py` で再生成できる
- `Assets/Source/ProbeTest/` — 反射プローブの検証用シーン。中央の仕切り壁で「密閉の西室(暖色)」と
  「天井が開いた東室(寒色・太陽光)」に分かれたホールと、その間を貫く金属球列(`metallic=1.0`、
  粗さ0.05)。床は磨いた石(粗さ0.06)で、壁のエミッシブ帯の映り込みから視差補正の効きを読み取る。
  各室には半透明のガラス板(`alphaMode=BLEND`)も1枚ずつ立ててあり、半透明にはSSRが効かないため
  プローブの有無がそのまま映り込みの違いとして現れる。
  `Tools/generate_probe_test.py` で再生成できる
- `Scenes/` — `.kscene`(シーンファイル)。`Assets/`の外にあり**Git管理対象**。
  このうち`BistroInteriorLit.kscene`は、Bistro内装の照明器具の実際の位置に合わせてポイントライトを15灯置き、
  部屋の形に合わせた反射プローブを3個置いたシーンです。反射プローブ・スクリーンスペースシャドウ・
  タイルライトカリングを、テスト用の合成シーンではなく実際のモデルの上で確認できます
  (`BistroInterior.kscene`はライトもプローブも持たない素の読み込み確認用として残してあります)。
  `ScreenSpaceShadowTest.kscene`(接触影の目視確認用)と`ManyLightsTest.kscene`(タイルライトカリング用に
  ポイントライトを格子状に64灯配置)の2つだけは手書きではなく`Tools/generate_shadow_test_scenes.py`で
  生成します(ジオメトリは`LightTest.kmodel`を流用するため、生成されるのは`.kscene`だけです)
- `Assets/Packed/` — 上記をKurenaiPacker.exeで変換した`.kmodel`/`.kgeom`/`.ktex`と、検証済みの`.kscene`
- `Assets/Packed/Skybox/` — 背景表示・IBLの入力となるHDR空キューブマップ(DDS形式、R16G16B16A16_Float、既に圧縮済みのためパッカーを通さず直接ここへ出力する)。`Tools/generate_sky_cubemap.py`(要`pip install numpy`)で再生成できる。既定では空をGPUで手続き生成するため通常は使われず、Procedural Skyを無効にしたときのフォールバックと、`[Scene]Skybox`を明示するシーン向けのアセットとして残っている

`.kmodel`/`.ktex`は元ファイルのタイムスタンプを見て自動生成・自動更新されることはありません
(実行時のディスクキャッシュではなく、KurenaiPacker.exeが生成する配布可能なアセットのため)。
ソースモデルを更新した場合は、KurenaiPacker.exeを再実行してください(`--force`を付けない限り、
既存の`.ktex`はスキップして高速に再パックします)。
