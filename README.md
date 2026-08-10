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
AO/間接光・鏡面反射・平行光のシャドウ(いずれもスクリーンスペース手法(SSAO/SSIL・SSR・カスケードシャドウマップ)
またはレイトレーシングから選択。DX12かつDXR Tier 1.1対応環境ではレイトレーシングを選択可能)・
ベイク済み遮蔽マップ(glTFの`occlusionTexture`)・複数ライト(ポイント/スポット、カンデラ/ルクス単位)・
ポイント/スポットライトのスクリーンスペースシャドウ(シャドウマップを使わない接触影)・
タイルベースのライトカリング・メッシュレット描画(メッシュを頂点64個・三角形124個までの塊に分け、
増幅シェーダーが塊ごとに錐台・背面カリングしてからメッシュシェーダーで描く。
DX12かつメッシュシェーダー Tier 1対応環境で選択可能)・半透明描画(専用フォワードパスで
アルファブレンド合成)・トーンマッピング(AgX / ACES / Reinhardから選択)・自動露出・ブルーム・
TAA(モーションベクターによる再投影で複数フレームを蓄積する時間方向のアンチエイリアス)・
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
  構成・責務分割・インターフェース規約など、設計と仕様の全体像はこちらです。
- **[docs/ImplementationDetail.md](docs/ImplementationDetail.md)** — 実装詳細。既定値や
  しきい値の根拠、式の導出、検証手順といった具体的な内容と理由はこちらです。
- **[docs/ImplementationHistory.md](docs/ImplementationHistory.md)** — 実装経緯。現在の設計が
  なぜその形になったのか(以前どうだったか・何が問題として現れたか・どう直したか)を
  知りたい場合はこちらを参照してください。

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
  KurenaiShowEditor/ KurenaiShowEditor.sln  ドローンショー(.kshow)のオーサリングツール。
                                       独立ソリューション。エンジンの2つのDLLに依存し、
                                       エンジンでプレビューしながら編隊を作る
                  Build/               KurenaiShowEditor.exeの出力先(Git管理対象外)
docs/                           ドキュメント(APIリファレンス・実装者向け)
ThirdParty/                     外部依存ライブラリ(Git Submodule)。imgui, DirectXTex, assimp
Scenes/                         手書きの.kscene(シーンファイル)。小さなテキストのためGit管理対象
Shows/                          ドローンショーの.kshow。バイナリだが手で作る資産のためGit管理対象
                                 (.kmodel等の派生物とは違い、元になるファイルが他に無い)
Assets/                         アセット(Git管理対象外)
  Source/                        入力。ソースモデル(.gltf/.fbx等)
  Packed/                        出力。KurenaiPacker.exeが生成する.kmodel/.kgeom/.ktexと
                                   検証済みの.kscene・.kshow。KurenaiEngine3Dが実際に読み込むのはこちら
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

DX12バックエンドはシェーダーをdxc(DirectX Shader Compiler)でコンパイルするため、
Windows SDKに含まれる`dxcompiler.dll`と`dxil.dll`も同じ仕組みで実行ファイルの隣へコピーされます
(`KurenaiEngineLibrary`のPostBuildEventがWindows SDKの`bin\<SDKバージョン>\x64`から取得)。
この2つのDLLが無い場合はログに警告を出したうえで従来のd3dcompiler(シェーダーモデル5.0)で
動作しますが、レイトレーシング機能は無効になります。DX11バックエンドは常にd3dcompilerを使うため
これらのDLLを必要としません。

## 必要環境

- Windows 10 / 11
- Visual Studio 2022 (「C++によるデスクトップ開発」ワークロード)
- Windows SDK **10.0.26100 以降**
- CMake (Visual Studio付属のもので可)
- DX12バックエンドのレイトレーシングを使う場合: DXR Tier 1.1 / シェーダーモデル6.5に対応したGPUとドライバ
- DX12バックエンドのメッシュレット描画を使う場合: メッシュシェーダー Tier 1 / シェーダーモデル6.6に対応したGPUとドライバ

各`.vcxproj`は`<WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>`
(＝インストール済みの最新SDK)を指定しているため、SDKを入れれば設定の変更は要りません。

**10.0.26100を要求する理由はdxcのバージョンです。**
実行ファイルの隣へ配布される`dxcompiler.dll`はWindows SDKの`bin\<SDKバージョン>\x64`から
コピーされる(上記のPostBuildEvent)ため、SDKのバージョンがそのままdxcのバージョンになります。
シェーダーモデル6.6を知らないdxcではbindlessが無効になり、
bindlessでジオメトリを引くメッシュレット描画も連動して選べなくなります。

| Windows SDK | 同梱されるdxc | SM 6.6 (bindless / メッシュレット) |
| --- | --- | --- |
| 10.0.19041 | 1.5 (`10.0.19041.685`) | 使えない |
| 10.0.26100 | 1.8 (`1.8.2502.11`) | 使える |

なお**コンパイルだけ**なら10.0.19041でも通ります
(`D3D12_FEATURE_D3D12_OPTIONS7`・`ID3D12GraphicsCommandList6`・`d3dx12.h`のメッシュシェーダー用
サブオブジェクトはこのバージョンで揃っており、10.0.20348で追加された
`D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`はエンジン側で値を持っています)。
ただし上記のとおりdxcが1.5どまりなので、起動してもメッシュレット描画とbindlessは無効のままです。

## セットアップ手順

### 1. Submoduleの取得

```
git submodule update --init --recursive
```

`ThirdParty/`にはimgui・DirectXTex・assimp・xatlas・meshoptimizerが入ります。このうち
**xatlasとmeshoptimizerは別途ビルドが不要**です(KurenaiPackerが必要なソースを直接コンパイルします。
xatlasは遮蔽マップをベイクする際のライトマップUV生成に、meshoptimizerはメッシュレットの生成に使います)。

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

ビルドとアセットの準備が済んだら、`Samples\Sample3D\RunDX12.bat`(DX12)または
`RunDX11.bat`(DX11)をダブルクリックすれば起動できます(「[実行(Sample3D)](#実行sample3d)」参照)。

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
- マテリアルから取り込むテクスチャは**ベースカラー・法線・メタリック/ラフネス・自発光・遮蔽マップ**の
  5枚です。遮蔽マップ(ベイク済みアンビエントオクルージョン)はglTFの`occlusionTexture`(強度
  `strength`も反映)と、`AMBIENT_OCCLUSION`スロットを持つ形式から取り込みます。持たないマテリアルは
  「遮蔽なし」として扱われます
- **メッシュレット**(メッシュシェーダー用の分割情報)は既定で自動生成されます。メッシュを
  頂点64個・三角形124個までの塊に分け、塊ごとのバウンディング球と法線コーンを`.kgeom`へ書きます。
  生成しても従来の描画経路には影響せず、増えるのは`.kgeom`の容量だけです
  (20万三角形のモデルで約18%。メッシュシェーダー非対応の環境では読み込まれません)。
  あわせて頂点の並びをキャッシュ効率のよい順へ最適化し、インデックスをメッシュレット順に並べ替えます。
  `--no-meshlets`を付けると生成も並べ替えも行いません(見た目の異常がメッシュレット化に
  由来するかどうかを切り分けるためのオプション)
- `--bake-occlusion`を付けると、**遮蔽マップをモデルのジオメトリから生成**します。重なりの無い
  ライトマップUVを自動生成し、GPUのレイキャストでメッシュごとに遮蔽率を焼きます。
  元から`occlusionTexture`を持つモデルでも、焼けたメッシュはベイク結果が優先されます

```
Tools\KurenaiPacker\Build\Bin\x64\Release\KurenaiPacker.exe ^
  Assets\Source\Sponza\Sponza.gltf -o Assets\Packed\Sponza\Sponza.kmodel --bake-occlusion
```

  解像度とレイ本数は`--occlusion-resolution <N>`(既定512)・`--occlusion-rays <N>`(既定128)で
  変更できます。レイ本数を増やすほど滑らかになり、その分だけ時間がかかります。
  三角形数の多いモデルでは、レイ本数よりも解像度のほうが品質への影響が大きくなります
  (テクセル密度が足りないとUVチャートが細かく分かれて斑点状のノイズになるため)。
  **目安は「1三角形あたりのテクセル数」で、1を切るならレイ本数より先に解像度を上げてください。**
  87万三角形のChinese Dragonでは、1024×1024・256レイ(1三角形あたり約1.2テクセル)が斑点状に
  なり、総レイ数と所要時間をほぼ変えずに2048×2048・64レイへ振り直すと解消しました
- ベイク時間のほとんどはライトマップUVの生成に費やされます。三角形数の多い**単一メッシュ**の
  モデル(3Dスキャンなど)では、UV生成を内部で分割することで大幅に短縮しています。
  分割する三角形数の閾値は`--unwrap-split-threshold <N>`(既定50000、`0`で分割しない)、
  分割後の目標三角形数は`--unwrap-chunk-triangles <N>`(既定100000)で変更できます。
  閾値以下のメッシュは分割されず、ベイク結果も変わりません
- `--bake-occlusion`を付けると、あわせて**bent normal**も焼きます。遮蔽マップが「どれだけ
  隠れているか」というスカラーしか持たないのに対し、bent normalは「**どの方向が開いているか**」を
  持つベクトルです。これにより次の2つが改善します:
    - **スペキュラ遮蔽が方向を見るようになります。** 従来の近似は反射がどちらを向いていても
      同じだけ暗くしていましたが、壁際では壁を向いた反射だけが暗くなるのが正しい挙動です
    - **拡散光の方向バイアスが解消します。** 環境光を法線方向ではなく「開いている方向」で
      引くため、壁際・窓際で壁の側の色まで平均してしまうことがなくなります

  アプリのレンダリング設定に「スペキュラ遮蔽の方式」という3値の切り替えがあります
  (Frostbite近似 / 球冠交差 / 球面ガウス。既定は球面ガウス)。球冠交差は可視性を
  二値のコーンとして扱うため、金属の凹んだ部分が純黒へ潰れることがあります。
  球面ガウスは同じ交差計算を柔らかい分布に置き換えたもので、壁を向いた反射だけが
  暗くなる方向性を保ったまま、潰れを大きく減らします(実測でChinese Dragonの
  潰れ画素が464→80へ。ただし完全になくなるわけではありません。詳細は
  [docs/Architecture.html](docs/Architecture.html) 34.10〜34.11節)。
  **この切り替えはシェーダーとレンダリング設定だけの変更なので、再パックは不要です**

  レイ本数は`--bent-rays <N>`(既定256)で変えられます。AO側より多いのは、スカラーの平均より
  ベクトル和のほうが収束が遅いためです。`--bent-rays 0`で生成を無効にできます。
  ベイク時間への影響はごくわずかです(レイキャストはベイク全体の0.1%未満で、大半はUV展開のため)。

  bent normalは**接空間**で焼かれます。モデル空間で持つと「遮蔽なし」が法線そのものになるため、
  曲面では遮蔽が無くても隣り合うテクセルの向きが違い、ミップ生成で平均したときに打ち消し合って
  長さ(=遮蔽率)が縮みます。その結果、遠景ほど暗くなり細かい黒い点が出ます。接空間なら
  遮蔽なしは曲率によらず常に`(0,0,1)`なので、平均しても長さが保たれます。

  > **容量に注意**: bent normalは符号付きのベクトルのためBC4/BC7で圧縮できず、
  > `R16G16B16A16_FLOAT`の無圧縮で書き出します。512²でミップ込み**1メッシュあたり約2.7MB**、
  > 数百メッシュのモデルでは合計がGB級になります。まずは単一メッシュのモデルで試してください
- `--metallic <V>` / `--roughness <V>` / `--base-color <R,G,B>` で、全マテリアルのPBR係数を
  上書きできます。マテリアルを持たない生の`.obj`(3Dスキャンの配布物など)へ検証用の
  マテリアルを与えるためのものです

> **`.kmodel`の形式が v9、`.kgeom`が v3 になりました。** v6でライトマップUVを頂点へ追加し、
> v7でbent normalのテクスチャ参照をメッシュへ追加し、v8でbent normalの格納空間を
> モデル空間から接空間へ変更し(v8はレイアウトが同じでテクスチャの中身の意味だけが
> 変わるため、`--bake-occlusion`を付けた再ベイクが要ります)、v9でメッシュレットを
> `.kgeom`へ追加したためです。古い`.kmodel`はバージョン
> 不一致で読み込みを拒否されるため、既存の`Assets\Packed\`はKurenaiPacker.exeで
> **再パックが必要**です
> (テクスチャの`.ktex`はそのまま流用されるため、`--force`を付けなければ短時間で終わります)。

#### メッシュレット / bindless / レイトレーシングの確認用シーン(ドラゴン)

主役にはリポジトリに既にある**McGuireのChinese Dragon**(`Assets\Source\ChineseDragon\dragon.obj`、
87万三角形の3Dスキャン)をそのまま使います。`OcclusionBakeCompare.kscene`などと同じ`.kmodel`です。

`Tools\generate_meshlet_stage.py`は、それを置くステージ(鏡面の床と市松模様の背景壁)だけを
生成します。この2枚が要るのは、ドラゴン単体では確かめられないものが2つあるためです。

- **鏡面の床** — メッシュレットの色分けがラスタ描画とレイトレーシングで一致するかを見るには、
  同じドラゴンを映す相手が要ります
- **市松模様の背景壁** — `dragon.obj`はテクスチャを持たない3Dスキャンなので、
  ヒット面が単色でも「bindlessでテクスチャを引けていない」のか「もともと単色」なのかが
  区別できません。模様のある面が1枚あれば、その1点だけで判別が付きます

```
python Tools\generate_meshlet_stage.py
Tools\KurenaiPacker\Build\Bin\x64\Release\KurenaiPacker.exe ^
  Assets\Source\MeshletStage\MeshletStage.gltf -o Assets\Packed\MeshletStage\MeshletStage.kmodel
Tools\KurenaiPacker\Build\Bin\x64\Release\KurenaiPacker.exe ^
  Assets\Source\ChineseDragon\dragon.obj -o Assets\Packed\ChineseDragon\DragonPlain.kmodel
```

確認の手順は`Scenes\Dragon.kscene`の冒頭コメントに書いてあります。

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

`[Model]`に`Water = true`を付けると、そのインスタンスを水面として描画します。通常の
G-Bufferとは別の専用シェーディングになり、`[Water]`セクションで法線マップと波の見た目を指定できます
(反射(SSR/RTとの統合)は未対応です)。

```ini
[Model]
Path = MontSaintMichelStudy/Water.kmodel
Water = true

[Water]
NormalMap = MontSaintMichelStudy/WaterNormal.png  # Assetsルートからの相対パス。省略可(法線マップ無しのフラット水面になる)
WaveScale = 12.0                                  # 省略可(既定12.0)
WaveSpeed = 0.03                                  # 省略可(既定0.03)
WaveStrength = 0.25                               # 省略可(既定0.25)
```

対象インスタンスは「デバッグ表示」パネルの「水面マスク」で白く表示され確認できます。

### ドローンショー

`[DroneShow]`セクションを書くと、夜空を編隊飛行する発光ドローンの群れを描画します。
1機につきカメラへ正対するビルボードを1枚、加算合成で描き、編隊から編隊へ順に変形して
いきます。水面がある場合は編隊がそのまま映り込みます。

**ショーの中身は`.kshow`というデータファイルが持ちます。** 編隊の点そのもの・機体数・
保持時間・変形時間・明るさ・機体の半径・揺れ・再生速度・種はすべてそちらにあり、
`.kscene`が決めるのは「出すか」と「どこにどの大きさで置くか」だけです。
同じショーを別のシーンへ別の場所・別の規模で置けるのはこの分担があるためです。
`.kshow`を作る・編集するには`Tools\KurenaiShowEditor`(下記)を使います。

光芒はこのパスでは作らずHDRのまま出力してブルームに任せているため、
**`[Bloom]`も合わせて有効にしてください**(エンジンの既定は無効です)。
`[Stars]`で夜空に星を描けます(星は昼のシーンには影響しません)。

```ini
[Bloom]
Enabled = true
Strength = 0.15

[Stars]
Enabled = true

[DroneShow]
Enabled = true
Path = Shows/Standard.kshow   # Assetsルートからの相対パス([Model]Pathと同じ基準)
Center = 0, 200, 100          # 編隊の中心(ワールド座標)
Scale = 140.0                 # 編隊の代表半径[m]。.kshowの点は代表半径1へ正規化されている
```

すぐ試すには `Sample3D.exe -scene DroneShow` を起動してください
(`Scenes/DroneShow.kscene`。干潟の夜景の上空で1500機が飛びます)。

> **DX12(レイトレーシング反射)では水面に機体が映りません。** 機体は手続き的に展開する
> ビルボードで高速化構造(TLAS)に入っておらず、反射のレイからは原理的に見えないためです。
> 「レンダリング」パネルから反射の手法をスクリーンスペースへ切り替えると映ります
> (平面反射パスはそのときだけ登録されます)。DX11は既定でスクリーンスペースなので影響しません。

各キーの意味と範囲は[APIリファレンス](docs/KurenaiEngine.html)の`.kscene`書式の表を参照してください。

### ショーの作成(KurenaiShowEditor)

`.kshow`はバイナリのデータファイルで、`Tools\KurenaiShowEditor`が読み書きします。
エディタはエンジンをそのまま使ってプレビューするので、トーンマップ・ブルーム・露出は
本番と同じ経路を通ります(発光点は加算合成とACESの組み合わせで「上げても白く飛ぶだけ」の
領域があり、別経路のプレビューでは判断できません)。

```
MSBuild Tools\KurenaiShowEditor\KurenaiShowEditor.sln /p:Configuration=Release /p:Platform=x64

rem GUIで編集する(既定でDroneShowシーンとAssets\Shows\Standard.kshowを開く)
Tools\KurenaiShowEditor\Build\Bin\x64\Release\KurenaiShowEditor.exe

rem 標準の6形状(球・円環・二重らせん・格子・ハート・らせん)を書き出す。
rem 書いたあと読み直してバイト一致まで検査する
Tools\KurenaiShowEditor\Build\Bin\x64\Release\KurenaiShowEditor.exe ^
    --generate-standard Shows\Standard.kshow
```

リポジトリが持つ`.kshow`は`Scenes\*.kscene`と同じくリポジトリ直下の`Shows\`にあります。
ランタイムが読むのは`Assets\Packed\Shows\`側なので、更新したらそちらへコピーしてください
(`.kscene`を`Assets\Packed\Scenes\`へ置くのと同じ手順です)。

## 実行(Sample3D)

**起動確認・動作検証には `Sample3D.exe` を使用します。**起動時に`Assets\Packed\Scenes\`内の
`.kscene`をファイル名の昇順で列挙し、先頭のシーンを読み込んで表示します。

### 起動スクリプト

`Samples\Sample3D\`にある次のバッチファイルをダブルクリックするだけで起動できます。

| ファイル | 起動するバックエンド |
| --- | --- |
| `RunDX12.bat` | DX12 |
| `RunDX11.bat` | DX11 |

構成は`Release`を優先し、無ければ`Debug`へ自動的にフォールバックします。明示したい場合は
`-debug` / `-release`を付けてください(それ以外の引数は`Sample3D.exe`へそのまま渡されます)。

```
Samples\Sample3D\RunDX12.bat -debug
```

ビルドが済んでいない場合やシーン(`.kscene`)が用意できていない場合は、起動せずに必要な
コマンドを表示して止まります。起動の記録は`Samples\Sample3D\Run.log`に残ります。

### コマンドラインから直接起動する

既定ではDX11バックエンドで起動します。`-dx12` 引数を付けて起動するとDX12バックエンドを使用します(再ビルド不要でDX11/DX12を比較できます)。現在どちらのバックエンドで動作しているかはウィンドウタイトル(例: `Kurenai Engine [DX12] - Sponza`)とImGuiのメニューバーの表示で確認できます。

起動後でも「システム」パネルの**グラフィックスAPI**から再起動なしで切り替えられます(この引数は起動時にどちらで始めるかを決めるだけです)。

```
Sample3D.exe -dx12
```

ログは実行ファイルと同じフォルダに、バックエンドごとに分けて出力されます
(DX11は`KurenaiEngine_DX11.log`、DX12は`KurenaiEngine_DX12.log`)。DX11版とDX12版は同じ
実行ファイルなので、この2つを同時に起動して見比べてもログが混ざりません。実行中にAPIを
切り替えた場合も出力先がそのつど切り替わり、同じAPIへ戻ったときは前半のログを消さずに追記します。

ウィンドウの位置・サイズ・最大化状態は終了時に実行ファイルと同じフォルダの`window.ini`へ保存され、次回起動時に復元されます。既定のサイズで起動し直したいときは`window.ini`を削除してください。

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

- **シーン** — シーンの切り替えを行います。ボタンをクリックするとそのシーン(`.kscene`)を読み込みます。一覧は`Assets\Packed\Scenes\*.kscene`から自動的に構築されるため、`.kscene`を追加するだけで一覧に増えます(付属のシーンはSponza、Bistro (McGuire) - Exterior / Interior、Bistro (McGuire) - Interior (Lit)(内装に照明15灯と反射プローブ3個を配置し、反射プローブ・スクリーンスペースシャドウ・タイルライトカリングを実際のモデルの上で確認できるシーン)、White Furnace Test(スペキュラBRDFのエネルギー保存を目視で検証するシーン)、Material Test(粗さ0〜1の球体列+半透明ガラス球)、Light Test(ポイント/スポット/平行光の検証用シーン)、Reflection Probe Test(反射プローブの効果を目視で確認するシーン)、Multi Model Test(TRS配置の確認用)、Energy Compare - White Furnace / Sun Only(スペキュラのエネルギー補正の方式を比較するシーン。粗さ×F0の55球グリッドを、一様白環境と「環境光0+太陽光のみ」の2条件で見る)、Dragon(メッシュレット・bindless・レイトレーシングの確認用。数十万三角形の単一メッシュと、それを映す鏡面の床))。使用中のグラフィックスAPI(DX11/DX12)はメニューバーに常時表示されます
- **レンダリング** — シーンをどう照らすかに関わる品質設定を、次の7つの節に分けて調整します
  - **AO / 間接光** — ON/OFFと手法を「SSAO / SSIL (Visibility Bitmask) / レイトレーシング (RTAO/RTGI)」から選びます。どの手法も出力は共通(rgb=間接拡散光、a=遮蔽率)なので、切り替えても後段の扱いは変わりません
    - **SSAO** — 遮蔽率だけを求めます。最も軽量です
    - **SSIL (Visibility Bitmask)** — 遮蔽率に加えて近傍サーフェスからの間接拡散光も求めます。深度バッファをたどるため、画面に映っていない面は遮蔽物にも間接光の光源にもなりません
    - **レイトレーシング (RTAO/RTGI)** — 同じものを法線周りの半球へ飛ばしたレイの交差判定で求めます。画面外の遮蔽物・反射面も効きます。レイの最大距離・サンプル数・間接光の強さ・遮蔽の強さ・バウンス面へ影を落とすかを調整できます。**DX12かつDXR Tier 1.1対応のGPUでのみ選択でき**、それ以外の環境では選択肢自体が出ません
    - **マテリアルの遮蔽マップを使う**(既定ON) — アセットに焼き込まれた遮蔽(glTFの`occlusionTexture`)を間接光へ掛けるかどうかです。上のON/OFFや手法の選択とは**独立した別系統**で、AO / 間接光を無効にしても遮蔽マップは効き続けるためトグルを分けてあります。遮蔽マップの内容は「デバッグ表示」の「マテリアル」のBチャンネルで確認できます。反射プローブには焼いた時点の値が入っているため、切り替えを反射へ反映するにはプローブの焼き直しが必要です
  - **シャドウ** — 平行光(太陽)の影の手法を「なし / カスケードシャドウマップ (CSM) / レイトレーシング (RT)」から選びます
    - **カスケードシャドウマップ (CSM)** — ライト視点の深度バッファを4枚描いて深度比較します。PCSSのライトサイズで半影の広さを調整できます
    - **レイトレーシング (RT)** — ピクセルごとに太陽へ影レイを飛ばします。シャドウマップの解像度に縛られないため、石壁の目地のような細かい接触影まで出ます。1ピクセルあたりのレイ本数と太陽の見かけの角半径(大きいほど半影が広く柔らかい)を調整できます。**DX12かつDXR Tier 1.1対応のGPUでのみ選択でき**、それ以外の環境では選択肢自体が出ません。半透明サーフェスと反射プローブの影は、RTを選んでいるときもCSMを使います
  - **スクリーンスペースシャドウ**(既定OFF) — ポイント/スポットライトの影。シャドウマップを使わず、深度バッファをライトの方向へレイマーチして求めます。ON/OFF・レイのステップ数・最大レイ長・厚み・法線バイアス・画面端フェード・1ピクセルあたりに影を落とすライト数の上限を調整できます。深度バッファに写っている面しか遮蔽物として扱えないため、画面外の物や手前の面に隠れた物は影を落としません(得られるのは接触影・中距離の遮蔽です)。どのライトが影を落とすかは「ライティング」パネルのライトごとの設定で決めます
  - **IBL / 環境光** — ON/OFFと強度。拡散イラディアンスは既定でプリフィルタ済み鏡面の最終ミップから得ますが、検証用に専用のイラディアンスマップへ切り替えることもできます。スペキュラBRDFのマルチスキャッタリング・エネルギー補正の方式もここで選択できます(補正なし / Linear / Series / Kulla-Conty。既定はLinear)
    - **環境光の拡散倍率 / 環境光の鏡面倍率**(どちらも既定1.0)は、環境光(間接光)の拡散成分と鏡面成分に別々に掛かる倍率です。「IBL 強度」が両者へ一様に掛かるのに対し、こちらは**両者の比率を変える**ためのもので、環境からの照り返しを保ったまま映り込みだけを強める(あるいはその逆)といった調整ができます。IBLのON/OFFどちらでも効き、SSR・反射プローブ・半透明マテリアルにも同じ倍率が適用されます。直接光・自発光には掛かりません
  - **反射** — 手法を「なし / スクリーンスペース (SSR) / レイトレーシング (RT)」から選びます。どの手法も反射先の色で反射を**差し替える**だけなので、切り替えても全体の明るさは変わらず、反射の鮮明さと内容だけが変わります。既定はDXR対応GPUなら**レイトレーシング (RT)**、非対応なら**なし**です(SSRは画面端で反射が途切れる破綻が目立つため既定にしていません)
    - **スクリーンスペース (SSR)** — 深度バッファをレイマーチします。最大距離・厚み・粗さのしきい値を調整できます。画面に映っていないものは反射に映らず、その場合は反射プローブ(またはスカイボックス由来のIBL)の結果がそのまま残ります
    - **レイトレーシング (RT)** — シーン全体へレイを飛ばすため、画面外のものも反射に映ります。最大距離・粗さのしきい値・反射先へ影を落とすかを調整できます。**DX12かつDXR Tier 1.1対応のGPUでのみ選択でき**、それ以外の環境では選択肢自体が出ません。反射に映る面のベースカラー・自発光・メタリック/ラフネスのテクスチャも読みます(bindless。シェーダーモデル6.6に対応していない環境ではマテリアルの定数色へ縮退します)
  - **タイルドライトカリング** — 画面を16x16ピクセルのタイルに分け、タイルごとに「そのタイルに届くライト」の一覧を作ってライティングの走査対象を絞ります。見た目を変えない最適化のため、ON/OFFで画像は変わりません
  - **メッシュレット** — メッシュを頂点64個・三角形124個までの塊に分け、増幅シェーダーが塊ごとに錐台カリングと法線コーンによる背面カリングを行ってから、生き残った塊だけをメッシュシェーダーがラスタライザへ流します。従来の描画は`DrawIndexed`1回=メッシュ全体が単位で、画面外の三角形もすべてラスタライザまで到達していました。タイルドライトカリングと同じく**見た目を変えない最適化**のため、ON/OFFで画像は変わりません(変わる場合はメッシュシェーダー側の頂点変換が頂点シェーダーとずれています)。**DX12かつメッシュシェーダー Tier 1・シェーダーモデル6.6対応のGPUでのみ操作でき**、それ以外の環境では理由を表示して常に従来描画になります
    - **メッシュレットを色分けして表示** — 塊ごとに違う色でアルベドを塗ります。法線・深度・モーションベクターは通常どおり書くため、この表示のままでもTAAや遮蔽は破綻しません。灰色に見える面はメッシュレットを経由していない面(メッシュレットが焼かれていないモデル、または水面)です。反射をレイトレーシングにしていると**反射に映る面も同じ色分けになる**ため、同じ塊が同じ色で映るかどうかで「描画とレイトレーシングが同一のジオメトリを見ているか」を目視で確認できます
- **ポストプロセス** — トーンマップ(カーブはAgX / ACES / Reinhardから選択。既定はAgX。薄明視と出力ディザリングもここ)、TAA(ON/OFFと履歴ブレンド率・ジッター強度・シャープネス・静止時のちらつき抑制・履歴の棄却方法と許容幅)、ブルーム(強度・しきい値・ソフトニー)、自動露出(EV100の上下限・露出補正・測光値の上側クランプ・夜のロールオフ量・明順応/暗順応の速度・測光に使うパーセンタイル範囲)
  - **出力ディザリング・TAA・ブルーム・自動露出はいずれも既定でOFF**です。素の描画結果を基準に見られるようにするためで、必要なものだけこのパネルで有効にしてください
  - **TAA**(Temporal Anti-Aliasing、既定OFF)は、毎フレーム描画位置を1ピクセル未満だけずらしたうえで、前フレームの結果をモーションベクターで現在の画面位置へ引き当てて蓄積します。静止していれば十数フレームで収束し、斜めエッジのジャギーがグラデーションに置き換わります。**履歴ブレンド率**を下げるほど多くのフレームが平均されて滑らかになりますが、物陰から現れた部分の追従が遅れて残像が出やすくなります。**ジッター強度**を0にすると位置ずらしだけが止まり、蓄積によるノイズ低減は残ります。**シャープネス**は蓄積で失われる高域を戻す量で、上げすぎると輪郭に白いふちが出ます
  - **静止時のちらつき抑制**(既定1.0)は、止まっている画素に限って履歴の混ぜ方を穏やかにし、履歴の棄却判定を緩めます。速度が0の画素では再投影のずれが原理的に起きないため棄却は害にしかならず、これがエッジのちらつきの主な発生源になっています。動いている画素の扱いは変わらないので、**残像の出方は0にしたときと同じまま**です。**履歴の棄却方法**と**許容幅**は、残像とちらつきのどちらを優先するかを比較するためのもので、狭いほど残像に強くちらつきが増えます
  - 半透明メッシュはモーションベクターを書かないため、カメラを大きく動かすとガラス面に残像が出ることがあります。またアンテナや樹木の葉のような1画素未満の細い構造は、原理上ちらつきが残ります
  - **薄明視**(既定0=無効)は暗所視の再現量です。暗くなると目は色を判別できない桿体だけの視覚へ移るため、露出を下げるだけでは「暗いが色鮮やかな夜」になり肉眼の見え方と合いません。桿体の分光感度が短波長寄りなことから来るプルキンエ現象(赤が沈み青が明るく見える)も入ります。効果が強く好みが分かれるため既定は無効です
  - 自動露出の測光では**空(背景)を数えません**。空は被写体ではなく光源であり、これを数えると空が画面に占める割合で露出が2〜3.5段も振れてしまう(写真でいう逆光の露出問題)ためです。あわせて画作り用の調整項目が2つあります。**夜のロールオフ**(既定4.5EV)は暗いシーンをわざと暗いまま写すための補正量で、既定値は「肉眼で見た月明かりの夜」になるよう調整してあります(0にすると常に中庸なグレーへ合わせる挙動)。**測光上限(基準EVから)**(既定+2.0EV)は測光値が「太陽・月・空の照度から求めた、構図に依存しない基準EV」から何段上まで行くのを許すかで、明るい看板が画面の大半を占めるような極端なケースの保険です(16まで上げると無効)
- **ライティング** — 太陽(時刻・自動進行・方位角・太陽光のON/OFF)、月の位置、手続き空、シーン全体の露出、ライトの一覧と編集を行います
  - **月の方位角 / 仰角** は**時刻に連動せず、ここで指定した固定位置に居続けます**。実際の月は太陽と独立した周期で動くため手動指定にしてあります。仰角が0度以下なら月は地平線下にあり月光は出ません。平行光源の枠は太陽と共有しており、太陽が沈むと支配ライトが月へ切り替わります
  - **手続き空** はGPUで手続き生成する空のON/OFFです。無効にするとオフラインで焼いた`Sky.dds`に戻ります。なお`.kscene`で`[Scene]Skybox`を明示しているシーンでは、このトグルに関わらず常にそのDDSが使われます
  - **EV100** は実在の写真露出値です。太陽/環境光/ポイント・スポットライトすべてに一様にかかるシーン全体の露出で、自動露出が有効なときはバッファの数値レンジを決める基準値として働きます。シーン全体の自発光の強度倍率もここです
  - ポイント/スポットライトの一覧・追加・複製・削除・種別の切り替え・パラメータ編集(強度はカンデラ/ルクス)ができます。ライトごとにスクリーンスペースシャドウを落とすかも切り替えられます
- **レンダリング → DDGI (拡散グローバルイルミネーション)** — DDGIのON/OFF・強度・1フレームに更新するプローブ数。`.kscene`に`[GIVolume]`があるシーンでのみ動作します。**拡散の間接光(壁の色が床へ回り込むような効果)を出せるのはDDGIだけです**(反射プローブは鏡面専任です。下記)。無効にすると、あるいはDDGIボリュームの外に出ると、拡散はスカイボックス由来のグローバルIBLへ戻ります(加算ではなく差し替えです)
- **レンダリング → 水面** — 水面(`.kscene`の`[Model]Water = true`で指定したインスタンス)の法線マップのスクロールを止める「水面アニメを止める」トグルと、波のスケール・速さ・強さのつまみがあります。対象インスタンスは「デバッグ表示」の「水面マスク」で白く表示され確認できます。水面には不透明ジオメトリの鏡像(平面反射)と空・雲が映ります
- **反射プローブ** — 反射プローブのON/OFFと、プローブの一覧・追加・削除・位置や影響範囲(形状(球/箱)・半径・箱の半径・箱の向き・ブレンド距離)の編集ができます。**反射プローブは鏡面(映り込み)専任です**。以前は拡散の間接光もプローブ側で計算できましたが、その経路は廃止し、拡散はDDGIへ一本化しました。プローブの中身はシーンのジオメトリやライトに依存するため、シーン読み込み時と位置を動かしたときに自動で焼き直されます(影響範囲だけを変えた場合は焼き直し不要です)。ライトや時刻を変えた後に焼き直したい場合は「焼き直す」ボタンを押してください
  - 「更新モード」で焼き直しの方式を切り替えられます — **焼き込み**(シーン読み込み時と「焼き直す」ボタンのみ。実行時コストはゼロですが、ライトや時刻を動かしても反射は焼いた時点のまま止まります)、**変化を検出して焼き直す**(太陽・時刻・ライトの変化を検出して自動で焼き直す)、**毎フレーム少しずつ**(1つのプローブを12フレームかけて更新します。前半6フレームで1面ずつ撮り、後半6フレームで1面ぶんずつぼかしてから次のプローブへ回ります。ぼかしの処理は実測で1フレームあたり1〜3ミリ秒に収まり、分割していなかった頃の「6フレームに1回だけ20ミリ秒前後」という山は出ません)
  - ただし**時刻を大きく動かして場面全体の明るさが2倍以上変わったときだけは、「焼き込み」でも自動で焼き直します**。プローブには焼いた時点の明るさの数値が入っているため、これを持ち越すと反射だけが桁違いの明るさになってしまうためです
  - 「視差補正」「プローブのブレンド」は視差補正・プローブ間ブレンドの有無を切り替えるもので、無効にしたときの見た目(壁際での反射位置のずれ、影響範囲の境界の継ぎ目)と見比べられます
  - 「距離キューブを使う」は視差補正の方式です。有効にするとキャプチャ時に一緒に焼いた距離を辿って実際の形状に反射を当てます(無効なら「部屋を直方体とみなす」従来の方式)。強く曲がった鏡面で反射像が二重に割れるのが軽減される一方、距離の解像度に由来する階段状のエッジが反射に乗るため**既定は無効**です
  - 「遮蔽判定(光漏れの抑制)」も同じ距離を使い、プローブから見えない位置(壁の向こう)のピクセルでそのプローブの寄与を落とします。ただしプローブが少ないうちは落ちた分をより明るい空由来のIBLが埋めるため、物体の真下が逆に明るくなることがあり**既定は無効**です
- **デバッグ表示** — Presentパスで表示する内容をドロップダウンで選択(最終結果 / アルベド / 法線 / マテリアル(R=金属度, G=粗さ, B=遮蔽マップ) / 自発光 / 深度 / シャドウマップ / RTシャドウ(太陽の可視率) / IBL(プリフィルタ済み鏡面・BRDF LUT・検証用の拡散イラディアンス) / ブルーム / ライトタイル(タイルごとのライト数のヒートマップ) / 反射プローブ(キャプチャ結果・影響範囲の色分け・プローブから見た距離) / モーションベクター(TAAが使う速度バッファ。静止で灰色、カメラを動かすと移動方向に応じて色が付く) / シーンカラー(生HDR・トーンマップなし)(トーンマップもガンマも通さないリニア値をそのまま表示。値を実測したいとき用) / DDGI(イラディアンスと距離モーメントのオクタヘドラルアトラス) / 水面マスク(水面のマテリアルIDを白黒表示)等、各パス中間結果のデバッグ表示)。間接光のように値が小さいバッファを見るための輝度倍率も指定できます。中間バッファの精度構成(HDR / Legacy 8bit)を切り替えて画質を比較することもできます
- **システム** — 垂直同期、フレームレート制限(既定でON・60fps。30/60/120から選択可能)、内部レンダー解像度、グラフィックスAPI、パネルの表示/非表示、レイアウトの初期化、現在のUIスケール
  - **内部レンダー解像度**(既定1280x720) — G-Buffer以降すべての中間バッファの解像度です。**ウィンドウサイズとは独立**していて、表示時はアスペクト比を保ったままウィンドウへ拡大縮小します(余る側にレターボックス/ピラーボックスが出ます)。1280x720 / 1600x900 / 1920x1080 / 2560x1440 / 3840x2160 から選べるほか、「現在のウィンドウサイズに合わせる」で等倍表示にもできます(押した時点で1回だけ適用され、その後のウィンドウリサイズには追従しません)。上げるほどVRAM使用量とフレーム時間が増え、変更時にTAAの履歴は一度破棄されます
  - **グラフィックスAPI** — DX11 / DX12 を**再起動なしで切り替えられます**。切り替えるとエンジンを作り直すため、ウィンドウが一度閉じてシーンが読み直されます(数秒かかります)。シーンと内部レンダー解像度は引き継がれますが、**それ以外の設定は既定値へ戻ります**。レイトレーシング(RT反射 / RTシャドウ / RTAO)はDX12かつDXR Tier 1.1対応のGPUでしか選べないため、DX11へ切り替えるとそれらの選択肢は消えます
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

`Kurenai::KurenaiEngine2D` の公開APIを機能ごとのデモ画面として並べたサンプルです
(座標系・APIの詳細は `docs/KurenaiEngine.html` 3章を参照)。数字キーでデモ画面を切り替えます。

| 操作 | 入力 |
| --- | --- |
| デモ画面の切り替え | 数字キー(1〜) |
| 終了 | Esc |

Sample3Dと同様に、`-dx12` 引数を付けて起動するとDX12バックエンドになります(再ビルド無しで
DX11/DX12の描画結果を見比べるため)。引数なしの既定はDX11です。

| デモ画面 | 内容 |
| --- | --- |
| 1 | `DrawSprite` — 画面内を跳ね回る半透明の色つきスプライト |
| 2 | 入力 — 押下エッジ/解放エッジ/ホイール回転量。「押下→同じボタン上での解放」でクリックを確定するボタン |
| 3 | サウンド — `SetVoiceVolume` によるBGMのフェードイン/フェードアウトと、マスター音量 |
| 4 | 図形 — `DrawRoundedRect` の回転と、`DrawCircle` の枠線(塗りa=0でリングになること) |
| 5 | テクスチャアトラス — `GetTextureSize` で得たピクセルサイズから正規化UVを求め、`DrawSpriteUV` で区画を切り出す |
| 6 | テキスト — `\n` による複数行描画と、`GetLineHeight` / `MeasureTextBlock` で大きさを決めたパネル |
| 7 | 2Dカメラ — WASD/矢印でパン、ホイールでズーム、`V` で論理解像度(800x600)の入り切り、`R` で既定へ戻す |
| 8 | クリップ矩形 — `PushClipRect` でスクロールする一覧をパネル内へ切り落とす。ネストと、クリップ無しの比較 |

デモ5で使うアトラスも、初回起動時に `DemoAtlas.bmp`(4x4区画の256x256)を生成して読み込みます。

デモ3で鳴らす音は、初回起動時に実行ファイルと同じフォルダへ `SineWave.wav`(440Hzの正弦波)を
生成して読み込みます(リポジトリにバイナリ資産を持たせないため)。

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
- `Assets/Source/EnergyCompareTest/` — スペキュラのエネルギー補正の方式比較用グリッド。
  粗さ0.0〜1.0の11段 × `metallicFactor` 1.00/0.75/0.50/0.25/0.00 の5段(= F0 1.00〜0.04)の55球。
  環境光0のテストに使う真っ黒なキューブマップ(`Assets/Packed/Skybox/UniformBlack.dds`)と合わせて
  `Tools/generate_energy_compare.py` で再生成できる
- `Assets/Source/MaterialTest/` — PBRライティング検証用の球体列。粗さ0.0〜1.0の白色球11個と、
  半透明描画(`alphaMode=BLEND`)検証用の赤いガラス球1個。参照するテクスチャ(`GlassRed.png`)も含めて
  `Tools/generate_material_test.py` で再生成できる
- `Assets/Source/ChineseDragon/` — [McGuire Computer Graphics Archive](https://casual-effects.com/data/)のChinese Dragon
  (`ThirdParty/SourceModels/`内の`dragon.zip`を展開したもの)。テクスチャを持たない単一メッシュなので、
  遮蔽マップの効きがそのまま見た目の差になる。`Scenes/OcclusionBakeCompare.kscene`が
  4体を並べて比較する(左2体=ソースのマテリアル、右2体=リフレクタンス=1。各ペアの左が
  遮蔽マップ無し・右がベイク済み)。用意するコマンドは`.kscene`の先頭コメントに書いてある

- `Assets/Source/OcclusionTest/` — 遮蔽マップ(ベイク済みAO)の検証用シーン。縦縞の遮蔽マップを
  `occlusionTexture.strength`を1.0〜0.0まで変えながら割り当てた球11個(両端は遮蔽マップ無しの対照群)と、
  半透明(`alphaMode=BLEND`)のガラス球1個。参照するテクスチャも含めて
  `Tools/generate_occlusion_test.py` で再生成できる
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
