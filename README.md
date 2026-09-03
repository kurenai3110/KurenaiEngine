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
増幅シェーダーが塊ごとに錐台・背面カリング、および深度プリパスの深度から作ったHi-Zによる
オクルージョンカリング(視界内だが手前の何かに完全に隠れている塊を落とす)を行ってから
メッシュシェーダーで描く。マテリアルをbindlessのテーブルから引くことで、1モデルを
G-Buffer・深度プリパス・シャドウとも1ドローで描く。
モデル単位でも同じ判定をコンピュートシェーダーで行い、生き残ったモデルだけを
GPU自身に発行させる(ExecuteIndirect)。
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
  Shaders/3D/                    KurenaiEngine3Dが内部で使うHLSL一式(ビルド時に.kshaderへコンパイルされる)
  Shaders/2D/                    KurenaiEngine2Dが内部で使うHLSL(Sprite2D.hlsl / Polyline2D.hlsl)
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
  KurenaiShaderPacker/                 シェーダービルドツール(Application)。HLSLを事前コンパイルして
                                       .kshaderを生成する。KurenaiEngine3D/2Dのビルドイベントから
                                       自動で呼ばれる(独立ソリューションは持たない)
                  Build/               KurenaiShaderPacker.exeとdxcランタイムの出力先(Git管理対象外)
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

**シェーダーはビルド時にコンパイルされます。** `KurenaiEngine3D`/`KurenaiEngine2D`のビルドイベントで
`KurenaiShaderPacker.exe`が走り、`Shaders/3D`・`Shaders/2D`のHLSLを`.kshader`
(事前コンパイル済みパッケージ)へ焼いてそれぞれのDLLと同じフォルダへ置きます。
**実行時にHLSLをコンパイルすることはなく、出力フォルダに`.hlsl`は置かれません。**
Sample3D/Sample2Dも同様に、自身のビルド後に必要なDLLと`.kshader`を自分の出力フォルダへコピーするため、
各実行ファイルは`Samples\Sample3D\Build\...` / `Samples\Sample2D\Build\...` 以下だけで単独で動作します。
Sample3Dはさらに`Assets\Packed\`もコピーしますが、Sample2Dは`KurenaiEngine3D.dll`・Assetsのどちらも
必要としないため同梱しません。

1つの`.kshader`には、実行環境の能力に応じて選ぶ3つのバリアントが入ります。

| バリアント | コンパイラ | プロファイル | 使う環境 |
|---|---|---|---|
| `Dxbc50` | d3dcompiler | `vs_5_0`/`ps_5_0`/`cs_5_0` | DX11の全経路。DX12でもシェーダーモデル6.5未満のデバイス |
| `Dxil65` | dxc | `*_6_5` | DX12・bindless非対応 |
| `Dxil66` | dxc | `*_6_6`(`KURENAI_BINDLESS=1`) | DX12・bindless対応 |

どのバリアントが選ばれたかは起動時のログに残ります
(`事前コンパイル済みシェーダー: DXIL / SM 6.6(bindless有効)を使用します` など)。

dxc(DirectX Shader Compiler)が必要なのは**ビルド時のこのツールだけ**です。
`dxcompiler.dll`と`dxil.dll`はWindows SDKの`bin\<SDKバージョン>\x64`から
`KurenaiShaderPacker`の出力フォルダへコピーされ、**実行ファイルの隣には配布されません。**

## 必要環境

- Windows 10 / 11
- Visual Studio 2022 (「C++によるデスクトップ開発」ワークロード)
- Windows SDK **10.0.26100 以降**
- CMake (Visual Studio付属のもので可)
- DX12バックエンドのレイトレーシングを使う場合: DXR Tier 1.1 / シェーダーモデル6.5に対応したGPUとドライバ
- DX12バックエンドのメッシュレット描画を使う場合: メッシュシェーダー Tier 1 / シェーダーモデル6.6に対応したGPUとドライバ
- DX12バックエンドのソフトウェアラスタライザ(比較用の経路)を使う場合: シェーダーモデル6.6 / 64bit整数のシェーダー演算(`Int64ShaderOps`) / bindlessに対応したGPUとドライバ

各`.vcxproj`は`<WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>`
(＝インストール済みの最新SDK)を指定しているため、SDKを入れれば設定の変更は要りません。

**10.0.26100を要求する理由はdxcのバージョンです。これはビルドマシン側の要件です。**
`KurenaiShaderPacker`が使う`dxcompiler.dll`はWindows SDKの`bin\<SDKバージョン>\x64`から
コピーされる(上記のPostBuildEvent)ため、SDKのバージョンがそのままdxcのバージョンになります。
シェーダーモデル6.6を知らないdxcでは`Dxil66`バリアントを焼けず、そのビルド成果物では
bindlessが無効になり、bindlessでジオメトリを引くメッシュレット描画とソフトウェアラスタライザも
連動して選べなくなります(ビルド自体は警告を出して続行します)。

| Windows SDK | 同梱されるdxc | SM 6.6 (bindless / メッシュレット / SWラスタ) |
| --- | --- | --- |
| 10.0.19041 | 1.5 (`10.0.19041.685`) | 使えない |
| 10.0.26100 | 1.8 (`1.8.2502.11`) | 使える |

なお**コンパイルだけ**なら10.0.19041でも通ります
(`D3D12_FEATURE_D3D12_OPTIONS7`・`ID3D12GraphicsCommandList6`・`d3dx12.h`のメッシュシェーダー用
サブオブジェクトはこのバージョンで揃っており、10.0.20348で追加された
`D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`はエンジン側で値を持っています)。
ただし上記のとおりdxcが1.5どまりなので、起動してもメッシュレット描画・bindless・ソフトウェアラスタライザは無効のままです。

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
いるため、必要なDLLも一緒にビルドされます)。シェーダービルドツール`KurenaiShaderPacker`も
各DLLから参照されているので、明示的にビルドする必要はありません
(**HLSLのコンパイルエラーはここでビルドが落ちる形で現れます**)。

```
MSBuild Samples\Sample3D\Sample3D.sln /p:Configuration=Debug /p:Platform=x64
```

各DLLは `Build\Bin\x64\Debug\<プロジェクト名>\` に、`Sample3D.exe`/`Sample2D.exe`はそれぞれ
`Samples\Sample3D\Build\Bin\x64\Debug\` / `Samples\Sample2D\Build\Bin\x64\Debug\` に出力されます。
Sample3Dの出力フォルダにはKurenaiEngineLibrary.dll・KurenaiEngine3D.dllと、それが参照する
`Shaders\*.kshader`・Assets(`Assets\Packed\`の中身)が、Sample2Dの出力フォルダには
KurenaiEngineLibrary.dll・KurenaiEngine2D.dllと`Shaders\*.kshader`のみが自動でコピーされるため、
各フォルダはそれだけで完結して動作します。**この時点では`Assets\Packed\`が空のため、次の「アセットの準備」を行うまでSample3Dは
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
  (**増やしても速くなりません**。BC7圧縮はGPU側で直列化されており、実測でワーカー8本のうち
  7本は待っているだけでした。詳細は[docs/ImplementationDetail.md](docs/ImplementationDetail.md) 51.6節)
- `--timing`を付けると、解析と書き出しのフェーズ別内訳を追加で印字します。「パックが遅い」の
  律速は入力によって正反対になる(テクスチャの多いモデルは書き出しが98%、テクスチャ0枚の
  PLATEAU LOD1タイルは解析が63%)ため、推測せずに測るためのものです。読み方は
  [docs/ImplementationDetail.md](docs/ImplementationDetail.md) 51章
- 個々のテクスチャの読み込みに失敗しても、そのテクスチャだけフォールバック(白/フラット法線)として扱いパックは続行します
- `.obj`/`.mtl`も入力に使えます。OBJ形式は単位情報を持たないため、センチメートル単位で作成された
  アセットはそのままだと100倍の大きさになります。その場合は`--scale <係数>`で補正してください
  (例: Amazon Lumberyard Bistroの`.obj`配布は`--scale 0.01`)
- `--origin <X,Y,Z>` で、頂点位置とバウンズからその座標を引きます(`--scale`を掛ける**前**に
  引くので、ソースの単位のまま指定できます)。**地理座標系のように原点が遠く離れた絶対座標で
  作られたモデル**を原点付近へ寄せるためのものです(Project PLATEAUのFBXは平面直角座標系の
  絶対値で、系原点から数十km離れています)。頂点はfloat32であるうえ、`.kmodel`のAABBもそのまま
  巨大になり、シーンAABBの対角から自動決定される遠クリップ面(`farZ = max(100, 対角×4)`)が
  桁で狂います。

  > **複数のモデルを並べるなら、全部に同じ値を指定してください。** タイルごとに
  > 「自分のAABBの中心」で寄せると、タイル同士の相対的な位置関係が壊れて街が崩れます。
  > 自動で中心化せず明示指定にしているのはこのためです
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
- **メッシュレットの離散LOD**も既定で生成します(`--meshlet-lods <N>`、既定4・上限4、
  1で原寸のみ、0は`--no-meshlets`と同じ)。段ごとに三角形を半分へ簡略化した独立のメッシュレット群を
  作り、それ以上潰せなくなった時点で打ち切ります。**全段が同じ頂点バッファを共有する**ため
  頂点は1つも増えず、増えるのはメッシュレットの3ブロックだけです。
  `.kgeom`のインデックスは従来どおりLOD0の三角形のみで、従来の描画経路とレイトレーシングには
  影響しません
- **FBXに埋め込まれたテクスチャ**(画像ファイルを同梱せず、モデルファイル自体が画像を持つ形式)も
  取り込みます。マテリアルが指すパスに実ファイルが無い場合に埋め込みを探し、一時ファイルへ
  取り出してから通常どおりミップ生成とBC7圧縮を行います。出力は
  `_Embedded/<.kmodelの名前>/`配下で、複数のモデルを同じ出力先へパックしても衝突しません
  (Project PLATEAUのLOD2はこの形式で、1タイルに1,714枚のJPEGが埋め込まれています)
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
- `--alpha-cutout <マテリアル名>=<しきい値>` で、指定した名前のマテリアルをアルファカットアウトに
  します(ベースカラーのアルファがしきい値未満の画素を捨てます)。複数指定できます。
  **glTF以外の形式で葉や草を正しく描くにはこれが要ります** — アルファモード(OPAQUE/MASK/BLEND)は
  glTFにしかない情報で、FBX/OBJでは「アルファで抜く前提のマテリアル」を解析だけでは判別できず、
  指定しないと不透明な板として描かれます
- `--emissive <マテリアル名>=<R,G,B>` で、指定した名前のマテリアルへ自発光の係数を与えます。
  複数指定できます。**照明器具を光らせるにはこれが要ることがあります** — WavefrontMTLの`Ke`を
  持たないアセットは`EmissiveFactor`が0のまま出て、G-Bufferが
  「自発光テクスチャ × EmissiveFactor × EmissiveIntensity」で合成する以上、
  自発光テクスチャを持つマテリアルすら光りません(Bistro屋外は132マテリアル全部の`Ke`が0でした)。
  **値は0〜1に収めません** — ライトの色はCPU側で露出を掛けてから送られるのに対し、
  自発光は露出を通らずそのまま加算されるため、露出済みの輝度に相当する値(裸電球で数百)を
  与えます。求め方は`docs/ImplementationDetail.md` 61.7h
- `--specular-as-orm` を付けると、`aiTextureType_SPECULAR`のテクスチャをメタリック/ラフネスと
  遮蔽マップとして読みます。**SpecularColorスロットへORM(R=遮蔽/G=ラフネス/B=メタリック)を
  格納する規約のFBX向け**です(NVIDIA Emerald Squareがこれ)。チャンネルの割り当てがglTFの
  metallicRoughnessと一致するため、テクスチャの加工は要りません。
  ただし**SpecularColorが本来の鏡面反射色であるアセットに指定すると全面が金属になります**。
  そのため既定では無効で、指定したときだけ有効になります
- `--inspect` を付けると、**パッケージを書き出さずに**assimpが読んだ直後のシーン構造を印字して
  終わります(`-o`は不要)。単位系(FBXの`UnitScaleFactor`)・上方向軸・ルート変換行列・ノード数・
  スケール適用後のバウンズ・マテリアルごとのテクスチャスロット・埋め込みテクスチャの一覧、
  **頂点属性(法線 / UV / 接線)を持つメッシュ数**が出ます。
  **外部から持ち込んだモデルの`--scale`を決めるとき、テクスチャがどのスロットへ入ったかを
  確かめるとき**に使います。パッカーが読まないスロットには`[パッカー未使用]`が付きます

  法線を持つメッシュが1つも無いモデルには警告が出ます。パッカーは`aiProcess_GenSmoothNormals`で
  法線を生成する(既に持つメッシュには何もしない)ため、そのまま焼いても陰影は出ますが、
  元データの素性を知るための情報として印字しています。Project PLATEAUのFBXが実際にこれで、
  原本のCityGMLが法線を持たないため全メッシュで欠けています

  ```
  Tools\KurenaiPacker\Build\Bin\x64\Release\KurenaiPacker.exe ^
    ThirdParty\SourceModels\EmeraldSquare_v4_1\EmeraldSquare_Day.fbx --inspect
  ```

- テクスチャが**ブロック圧縮(BC1〜BC7)で幅か高さが4未満**の場合は、警告を出してそのスロットを
  フォールバック(白1x1/フラット法線)として扱います。ブロック圧縮は4x4ピクセル単位で符号化される
  ため、1x1などはGPUのシェーダリソースビュー作成が失敗します。配布アセットには「法線マップ無し」を
  表す1x1のダミーが圧縮形式のまま置かれていることがあり(Emerald Squareの法線マップ115枚中6枚)、
  パックの時点で弾かないと実行のたびに転送失敗のエラーが出ます

> **`.kmodel`の形式が v10、`.kgeom`が v4 になりました。** v6でライトマップUVを頂点へ追加し、
> v7でbent normalのテクスチャ参照をメッシュへ追加し、v8でbent normalの格納空間を
> モデル空間から接空間へ変更し(v8はレイアウトが同じでテクスチャの中身の意味だけが
> 変わるため、`--bake-occlusion`を付けた再ベイクが要ります)、v9でメッシュレットを
> `.kgeom`へ追加し、v10で**メッシュ単位のAABB・マテリアルテーブル(`MaterialEntry`)・
> メッシュレットの材質番号・メッシュレットの離散LOD**の4つをまとめて追加したためです
> (バージョンを上げるたびに全アセットの再パックが要るので、フォーマットの変更は一度にまとめました)。
> 古い`.kmodel`はバージョン
> 不一致で読み込みを拒否されるため、既存の`Assets\Packed\`はKurenaiPacker.exeで
> **再パックが必要**です
> (テクスチャの`.ktex`はそのまま流用されるため、`--force`を付けなければ短時間で終わります)。

#### 現代の街並みの確認用シーン(NVIDIA Emerald Square)

商業街区4ブロック(約230m四方)の屋外シーンです。中低層のビルとストリートファニチャ、
SpeedTreeの植生、バス、高さ108mの展望塔が入ります。**取得から配布までは
`Tools\import_emerald_square.ps1`が一括で行います**(既にあるものは飛ばすので何度実行しても構いません):

```
Tools\import_emerald_square.ps1
Samples\Sample3D\Build\Bin\x64\Release\Sample3D.exe -scene EmeraldSquare
```

手で行う場合は、ZIPを`ThirdParty\SourceModels\`へ展開してから次を実行します
(カットアウト対象は`.DoubleSided`で終わる29マテリアル。一覧はスクリプト内にあります):

```
curl -L -o EmeraldSquare_v4_1.zip https://developer.nvidia.com/emerald-square
Tools\KurenaiPacker\Build\Bin\x64\Release\KurenaiPacker.exe ^
  ThirdParty\SourceModels\EmeraldSquare_v4_1\EmeraldSquare_Day.fbx ^
  -o Assets\Packed\EmeraldSquare\Day.kmodel ^
  --specular-as-orm --alpha-cutout "Grass_blades.DoubleSided=0.5" (...29件)
Tools\KurenaiPacker\Build\Bin\x64\Release\KurenaiPacker.exe ^
  --scene Scenes\EmeraldSquare.kscene -o Assets\Packed\Scenes\EmeraldSquare.kscene
```

- **`--scale`は要りません。** このFBXは`UnitScaleFactor=100`と宣言していますが、assimpが読んだ
  直後のルート変換行列は単位行列で、頂点はそのままメートルです(`--inspect`で確認できます:
  バウンズ230.1×113.2×230.2m、展望塔の高さ108.25m)。`--scale 0.01`を付けると1/100になります
- **`--specular-as-orm`が要ります。** ORMがFBXのSpecularColorスロットに入っているため、
  既定の解決では220マテリアル全部が1枚も拾えません(テクスチャ要求が225枚→336枚に変わります)
- **`--alpha-cutout`が要ります。** 付けないと葉や草が不透明な板として描かれます
- ダウンロードURLは302で署名付きURLへ飛びます。トークンは短時間で失効するので、直リンクを
  控えず必ず`/emerald-square`からリダイレクトを辿ってください(認証は不要です)
- 法線マップ3枚(`Painted_Metal` / `Emissive_Light_2` / `Emissive_Light_Inst`)は1x1のブロック圧縮
  なのでフォールバックされ、パック時に警告が出ます。配布物側の作りで、絵への影響は軽微です

> **ライセンス: CC BY-NC-SA 3.0 Unported(非商用のみ・継承あり)。**
> 出典: NVIDIA ORCA - NVIDIA Emerald Square v4.1 / Nicholas Hull, Kate Anderson, Nir Benty (2017)
> <https://developer.nvidia.com/orca/nvidia-emerald-square> 。植生はSpeedTreeのORCAアセットです。
> 商用利用が必要な場合はこのアセットを使えません。

#### 大規模な都市の確認用シーン(Project PLATEAU 東京都23区)

東京23区全域を、**建築物(LOD1/LOD2)・地形・道路・橋梁の4種**で構成したシーンです。
`[Model]`は**767件**、広がりは約33km×32kmで、`.kmodel`を多数並べたときの読み込み・カリング・
モデルLOD・ストリーミングの確認に使います。
**取得から配布までは`Tools\import_plateau.ps1`が一括で行います**
(既にあるものは飛ばすので何度実行しても構いません):

```
Tools\import_plateau.ps1
Samples\Sample3D\Build\Bin\x64\Release\Sample3D.exe -scene PlateauTokyo23ku
```

スクリプトは ダウンロード(約2.99GB) → 5種の展開 → 共通原点の算出 → 847タイルのパック →
`.kscene`の生成と配置 → 出力フォルダへの配布 を順に行います。
`.kscene`は`Tools\plateau_scene.py`が機械生成します(767個の`[Model]`を手で書けないため)。

| 種別 | タイル | メッシュコード | テクスチャ |
|---|---:|---|---|
| 建築物 LOD1 | 671 | 3次(8桁 / 約1km四方) | 無し |
| 建築物 LOD2 | 80 | 同上 | **あり(JPG 33,353枚)** |
| 地形(dem) | 14 | 2次(6桁 / 約10km四方) | 無し |
| 道路(tran) | 14 | 同上 | 無し |
| 橋梁(brid) | 68 | 3次 | あり(TIF 713枚) |

- **建築物のうち26タイルは2段LOD**です。`Path`にLOD2(テクスチャ付き)、`LODPath`にLOD1(箱)を置き、
  `LODDistance = 1500`で切り替えます。実測(DX11 / Release / 1280x720 / RTX 4070 Ti)で、
  近景のG-Bufferドローコールが**2,948**、LODDistanceの外へ出ると**698**まで落ちます

  > **LOD2があっても使っていないタイルが54件あります。** PLATEAUのLOD2は
  > 「そのタイル全体」が整備されているとは限らず、一部の建物だけのものが混ざります
  > (`53394515`はLOD1が384セルを占めるのにLOD2は3セル)。そのまま`Path`に据えると
  > **近づくほど建物が消える**ため、`Tools\plateau_lod2_coverage.py`が測る占有被覆率が
  > 0.95以上のものだけを採用しています。**AABBの被覆率では測れません** ―― 建物が2棟でも
  > タイルの対角にあればAABBは満杯になり、実測でAABB被覆1.000・占有被覆0.01のタイルが
  > 混ざっていました

- **`[Scene] StreamingDistance`を指定しています。** 未指定だとストリーミングが丸ごと無効になり、
  `SceneLoader`が全LOD段を起動時に確保します(LOD2だけで約10GB)。指定すると各インスタンスの
  「現在のLOD段」だけを読むので、LOD2はLODDistanceの内側にしか載りません。
  値はシーン対角(47,380m)で、距離による切り捨てを起こさせない意図です

  > **副作用: メッシュ単位のフラスタムカリングが無効になります。**
  > `MeshWorldBoundsList`は読み込み済みの実体からしか作れず、ストリーミング時は空のまま
  > 構築が終わります(描画側は保守側=間引かない方へ倒れます)。実測でメッシュ単位は
  > 判定5,964 / 間引き0(0.0%)、モデル単位は85.9%です。**0%はカリングの不具合ではありません**

- **パックは既定で8プロセス同時に走ります**(`-Parallel <N>`で変更、1で直列)。LOD1のタイルは
  テクスチャを持たずGPUも内部スレッドも使わないため、プロセスを並べるとそのまま効きます
  (671タイルの実測で139.9秒 → 22.3秒。出力は直列実行とSHA256が一致します)。
  子プロセスの出力はタイルごとに`%TEMP%\kurenai_plateau_<日時>\`へ分けて残ります

- **`--origin`が要ります。** このFBXはEPSG:6677(JGD2011 平面直角座標系 第9系)の絶対座標で、
  系原点から北へ最大52km離れています。**全タイルで同じ値**(`-8096,0,-36118`)を引きます。
  値は`Tools\plateau_mesh.py origin`が全タイルのメッシュコードから算出します
- **`--scale`は要りません。** `UnitScaleFactor=1`でそのままメートルです
- **軸は実測で確定させました。** FBXはZ-upで「X=東 / Y=北 / Z=標高」ですが、assimpがZ-up→Y-upへ
  変換し`aiProcess_ConvertToLeftHanded`も入るため、パッカーが扱う時点では
  **「X=東 / Y=標高 / Z=北」**になります。西新宿タイル(`53394525`)を`--inspect`した実測値
  `X -13218.99〜-12034.59 / Y 27.34〜267.02 / Z -35171.98〜-34122.55`が、メッシュコードから
  計算した期待範囲と符号ごと一致することを確認しています。**街が鏡像になっていても一見
  気づけない**ため、画像ではなく座標の数値で突き合わせています
- **`[Scene] ShadowDistance = 500`を指定しています。** このシーンの`farZ`は190kmで、
  指定しないと第1カスケードが数kmを2048²の1枚で覆うことになり、近景の影が事実上消えます
- **`[Scene] CameraSpeed = 150`を指定しています。** 対角47kmから自動決定させると600m/s級になり、
  LODの切り替わりを目で追えません
- LOD1の建築物は「航空レーザ測量の高さで押し出した箱」で、**マテリアルは`DefaultMaterial`
  1件のみ・テクスチャは0枚**です。灰色に見えるのが正しい状態です
  (LOD2へ切り替わる26タイルと、下の丸の内シーンでは実写テクスチャが付きます)
- **初期カメラはLOD2地区が見える位置に置いています**(`-1500, 300, -2400`)。
  以前の高度900mの俯瞰は、大気遠近で全面が白く飛ぶうえ、LOD2がLODDistanceの外なので
  一度も出ませんでした

##### メッシュレットLODの検証用シーン(dem / 地形)

メッシュレットLODが効くのは「三角形そのものが支配的コスト」のモデルだけです。
PLATEAU でそれに当たるのは **dem(地形)** で、1タイルが2次メッシュ(約10km四方)の1メッシュ、
三角形は100万の桁にのぼる一方でテクスチャを1枚も持ちません。
ビル街を混ぜるとドローコールとテクスチャのコストが上乗せされ、三角形を減らした効果が
総フレーム時間から読めなくなるため、dem だけのシーンを別に用意しています。

```
python Tools\plateau_dem_scene.py Assets\Packed\Plateau\Dem Scenes\PlateauDem.kscene
Samples\Sample3D\Build\Bin\x64\Release\Sample3D.exe -dx12 -scene PlateauDem
```

- **`-dx12` が要ります。** 段を選ぶのは増幅シェーダーなので、DX11 では一度も実行されません
- **カメラをモデルの外接球の外へ置いています。** 段は外接球の投影サイズで決まるため、
  球の内側にカメラがあると投影サイズが振り切れて常に原寸(段0)になり、
  段の選択が一度も走らないまま「効かなかった」と読み違えます

> **ライセンス: 公共データ利用規約(PDL1.0) / CC BY 4.0 互換。商用利用可・要出典表示。**
> 出典: 国土交通省 Project PLATEAU「3D都市モデル(Project PLATEAU)東京都23区」
> <https://www.geospatial.jp/ckan/dataset/plateau-tokyo23ku> 。
> 著作権は各地方公共団体に帰属します。

#### テクスチャ付きの近景シーン(Project PLATEAU 東京駅・丸の内 LOD2)

同じ配布物の**建築物LOD2**です。3次メッシュ`53394600`/`53394601`(東京駅・丸の内)の建築物と橋梁、
それを含む2次メッシュ`533946`の地形・道路をあわせた6モデル・**3,418メッシュ / 頂点316万 /
テクスチャ3,412枚**のシーンで、**実写テクスチャの付いた街並み**を見られます。
`Scenes\PlateauMarunouchi.kscene`。

```
KurenaiPacker.exe <展開先>\bldg\lod2\53394600_bldg_6677.fbx ^
  -o Assets\Packed\Plateau\BldgLod2\53394600.kmodel --origin -8096,0,-36118
Samples\Sample3D\Build\Bin\x64\Release\Sample3D.exe -scene PlateauMarunouchi
```

dem/tran/brid も出力先を`Dem\` `Tran\` `Brid\`へ変えるだけで同じコマンドです。
**`--origin`は23区シーンとまったく同じ値**にしてあるので、両シーンの座標はそのまま対応します。

- **テクスチャはFBXに埋め込まれています。** ファイルとしては1枚も配布されておらず、
  マテリアルが指すのは実在しない一時パスです。パッカーが埋め込みから取り出して
  `_Embedded\<モデル名>\`へ`.ktex`を書きます(丸の内2タイルで3,412枚・失敗0)
- **`[Scene] ShadowDistance = 500` と `[Scene] CameraSpeed = 30` を明示しています。**
  dem/tranは6桁=2次メッシュ(約10km四方)で丸の内だけに切り出せないため、シーン対角が
  約16.5kmになります。放置すると`farZ`が66km級へ伸びて近景の影が消え、カメラ速度も
  自動決定だと約240 m/sになって見たい2.3km四方に対して速すぎます
- 実測(DX11 / Release / 1280x720 / RTX 4070 Ti、初期カメラのまま撮影も入力もせず放置・94サンプル)で
  **FPS 57.9 / CPU 1.75ms / GPU 5.50ms / VRAM 893MB**。メッシュ単位フラスタムカリングの
  間引き率は**59.7%**で、これは1メッシュ=1棟というこのアセットの形によるものです
  (Emerald Squareでは1.5%)
- **地表は霞で白く飛びます。** 建物が宙に浮いて見えますが地形は描かれています。
  同じ6モデルを霞(`[Fog]Enabled`)と雲(`[Cloud]Coverage`)を切った固定カメラで並べた
  `Scenes\PlateauMarunouchiVerify.kscene` があり、そちらでは手前から地形の端まで地表が見えます
  (メッシュ単位カリングのON/OFF比較にも使います)

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

#### インスタンシングの確認用シーン

`Scenes\InstancingTest.kscene`は、同じ`.kmodel`を格子状に256体並べてインスタンシングの
効き方を測るためのシーンです。`Tools\generate_instancing_test.py`が生成します。

```
python Tools\generate_instancing_test.py Scenes\InstancingTest.kscene
Tools\KurenaiPacker\Build\Bin\x64\Release\KurenaiPacker.exe ^
  --scene Scenes\InstancingTest.kscene -o Assets\Packed\Scenes\InstancingTest.kscene
```

上のメッシュレット確認用ステージ(`MeshletStage.kmodel`)を並べるので、
モデルの用意はそちらの手順で済んでいます。

**なぜ専用シーンが要るのか**: 同じ`.kmodel`を多重配置しているシーンは`MultiModelTest.kscene`
(3配置)しかなく、ドローコールの削減が「3 → 1」では計測誤差に埋もれます。PLATEAU 東京23区も
Sponza も Bistro も全モデルがユニークで、インスタンシングは一度も発動しません。

格子の32体はX軸のみ負スケール(ミラーリング)にしてあります。ワインディングが反転するため
別のパイプラインステートで描く必要があり、**インスタンシングでも別のバッチへ分かれなければ
なりません**。まとめると片方が裏面として全部捨てられ、絵から消えます。

「レンダリング」パネルの**ジオメトリ → インスタンシング**でON/OFFを切り替えられます。
絵は一致したままドローコール数だけが変わるのが正しい挙動です
(実測: DX11 / RTX 4070 Ti / Release / 1280x720 で 2,364 → 34)。

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

`[Scene]`に`ShadowDistance`を書くと、**カスケードシャドウの分割範囲をその距離で打ち切れます**
(単位はメートル、1〜100000)。影の遠クリップだけが手前になり、シーンの描画距離は変わりません。

```ini
[Scene]
Name = 広いシーン
ShadowDistance = 500
```

遠クリップ面はシーンの大きさから自動で決まるため(`farZ = max(100, AABBの対角×4)`)、
数十km規模のシーンでは`farZ`が100km級になり、**第1カスケードが数kmを2048x2048の1枚で覆うことに
なって近景の影が消えます**。これを避けるためのものです。**書かなければ従来どおりの挙動**なので、
既存のシーンの見え方は変わりません。
`[Model]`に`LODPath`と`LODDistance`を対で書くと、**カメラからの距離でモデルを差し替えます**
(モデルLOD)。`Path`が最も詳細な段で、`LODPath`は粗くなっていく順に最大3つまで
(`Path`を含めて4段まで)書けます。

```ini
[Model]
Path        = Plateau/BldgLod2/53394601.kmodel   # 近景(テクスチャ付き)
LODPath     = Plateau/BldgLod1/53394601.kmodel   # 遠景(粗い箱)
LODDistance = 1200                               # 1200mを超えたらLODPathへ
```

- 距離は**インスタンスのAABBの最近接点まで**で測ります。中心距離だと1km四方のような
  大きなタイルで「足元のタイルだけ粗くなる」逆転が起きます
- 切り替え点の**±5%は不感帯**です(境界で往復してちらつかないようにするため)
- 切り替えの前後で2段を短時間**クロスディザ**で重ねてポップを目立たなくします。
  2段は同じノイズの補集合を描くため、重なり(Zファイティング)も隙間もできません
- **影・反射プローブ・DDGIは常に最も粗い段**を使います(どれもテクスチャを読まないため)
- `LODDistance`は昇順で書いてください。降順や、`LODPath`と対になっていない指定はエラーになります
- **書かなければ従来どおり**で、既存のシーンの見え方は変わりません

`[Scene]`に`StreamingDistance`を書くと、**カメラからその距離以内のモデルだけを常駐させます**
(モデルのストリーミング、単位はメートル、1〜100000)。

```ini
[Scene]
Name = 広いシーン
StreamingDistance = 2000
```

- 読み込み時はモデルの実体を読まず、`.kmodel`のヘッダにあるAABBだけで配置を決めます。
  実体はカメラが近づいたときに読み込みスレッドが読みます
- 距離の測り方はモデルLODと同じ(**インスタンスのAABBの最近接点まで**)
- カメラが `StreamingDistance` の**1.25倍**より遠ざかったモデルは破棄されます
  (読み込みと同じ距離で捨てると境界で往復するため、不感帯を置いています)
- **書かなければ従来どおり全部常駐**で、既存のシーンの挙動は変わりません
- レイトレーシングは**常駐の増減に追随します**。読み込み時点では高速化構造を作らず、
  最初のモデルが常駐してから作り、以降は常駐が変わるたびに作り直します
  (走行中に焼き直し続けないよう、増減が0.5秒止まってから作り直します)
- 現状の制限: **モデルファイルに埋め込まれたライトは無視されます**(読み込み前は位置が
  分からないため。該当するとログに警告が出ます)

`[Scene]`に`TextureStreaming = true`を書くと、**カメラからの距離に応じてテクスチャの常駐ミップを
減らします**(テクスチャストリーミング)。VRAMの使用量が下がり、見た目は変わりません。

```ini
[Scene]
Name = 街のシーン
TextureStreaming = true
TextureStreamingBias = -2   # 省略可(既定 -2)。負なら安全側=より詳細なミップを残す
```

必要なミップ段はCPUで見積もります(メッシュのUV密度・カメラからの距離・内部レンダー解像度から)。
足りているぶんだけを`.ktex`から読み直してGPUリソースを作り直すため、遠くにあるテクスチャほど
小さくなります。DX12でタイルリソース(Tier 2以上)が使える場合は、リソースを作り直さず
タイルの貼り替えだけで済ませます。**書かなければ全ミップ常駐のまま**で、既存のシーンの
見え方もVRAMの使用量も変わりません。

**モデルのストリーミング(`StreamingDistance`)と併用できます。** 後から常駐したモデルの
テクスチャも追跡対象に入り、遠ざかって破棄されたぶんは外れます。読み出しはモデルの
読み込みと同じスレッドで行うため、スレッドが増えることはありません。

**どれだけ減るかはシーンによって桁で違います。**
決めるのは「そのテクスチャが画面上で何倍に過剰か」で、シーンの大きさではありません。

| シーン | テクスチャ | 全ミップ | 常駐 | 常駐率 |
|---|---:|---:|---:|---:|
| Bistro Exterior(小物に4K) | 413枚 | 1,339.5MB | 283.1MB | **21.1%** |
| PLATEAU 丸の内 LOD2(既定カメラ・タイルの内側) | 3,412枚 | 321.0MB | 296.1MB | 92.2% |
| PLATEAU 丸の内 LOD2(約1,490m 離れる。式から計算) | 3,380枚 | 299.8MB | 82.4MB | **27.5%** |

PLATEAU の LOD2 は写真テクスチャが建物の壁一面を覆っており(解像度の中央値は256)、
近距離では mip0 でようやく画面の画素密度に届くため落とせる段がありません。
**減らないのは正常です。**

**見た目は変わりません。** 同じ起動・同じカメラで切り替えて比べたところ、常駐を 60.9% まで
落とした状態でも、街と地形の画素は全ミップ常駐と**完全に一致**しました(差のある画素 0.000%)。
落としているのは、そもそもハードウェアが選ばないミップだからです。

常駐の状況は「システム」パネルの「テクスチャストリーミング」でサイズ帯ごとに、
場所ごとの分布は「ストリーミング」パネルの俯瞰図(色分けを
「テクスチャの常駐ミップ」に切り替える)で確認できます。

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
LODCount = 1                   # 省略可(既定1)。クリップマップLODの段数(1〜4)
FollowCamera = false           # 省略可(既定false)。各LODの原点をカメラへ追従させるか
```

プローブ位置は`Origin + (i, j, k) * ProbeSpacing`です。壁の内側にプローブが1個も入らないと
そこだけ間接光が外挿になるため、`Origin`は壁から少し内側に置いてください。
`NormalBias`を0に近づけると画面全体が一様に暗くなります(面が、自分を照らしているプローブから
見えていないと誤判定するためで、強度で持ち上げるのではなくこのバイアスで直します)。

ボリュームは現状**1個だけ**使われます(複数書いた場合は2つ目以降を警告付きで無視します)。
明るさは時間をかけて収束します(バウンスが1フレームに1回ずつ積み上がるため、
455プローブの既定設定でおよそ数十秒)。

**`LODCount` を2以上にすると、1つのボリュームの中がクリップマップLODの入れ子になります。**
LOD k は間隔が `ProbeSpacing * 2^k`、プローブ数は全段共通なので、覆う範囲は段が1つ上がるごとに
2倍になります。「近くは密・遠くは粗く」を1つのボリュームで表すためのもので、広いシーンで
格子の間隔が現実的でなくなる問題を解きます。プローブの総数は `ProbeCounts の積 × LODCount` です
(上限は8192個)。

`FollowCamera = true` にすると、各LODの原点が毎フレームカメラ位置から**そのLOD自身の格子へ
スナップして**決まります。スナップするのでプローブのワールド座標は動かず、動くのは
「どのプローブが範囲に入っているか」だけです。範囲から抜けて反対側へ回ったプローブは
焼き直されるまでサンプリングから外れる(1段粗いLODかグローバルIBLへ落ちる)ので、
カメラを速く動かしても別の場所の色が出ることはありません ―― 間接光の追従が遅れるだけです。
**`FollowCamera = true` のとき `Origin` は配置に使われません**(書式上の必須キーとして残ります)。

`NormalBias` / `ViewBias` は段ごとには変えられず1つの値が全段に効くため、
**いちばん細かい段(LOD0)の間隔に合わせて決めてください**。粗い段に合わせると照会点が
大きくずれて別の場所の光を引きます。実績のある比は間隔の12.5% / 5%です
(`Scenes/MontSaintMichel.kscene` が3段構成の例で、LOD0の6mに対し 0.75 / 0.30 を使っています)。

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

検証用に次の起動オプションもあります(いずれも省略可)。GUIのコンボを人手で操作せずに同じ手順を再現するためのもので、通常の利用では不要です。

`-megalightsaccum` と `-megalightsdump` は、確率的サンプリングの正しさ(平均が参照実装へ寄るか)を測るためのものです。**画面キャプチャでは測れません** — 8bitかつトーンマップ後なので、丸めだけで誤差に下限が生まれ、そこに隠れて読めなくなります。

| オプション | 意味 |
|---|---|
| `-scene <名前>` | 起動時に開くシーン(拡張子を除いたファイル名。例: `-scene MontSaintMichel`) |
| `-debugview <番号>` | デバッグ表示を番号で選ぶ(並びはUIの「デバッグ表示」コンボと同じ。例: `26` = DDGIのイラディアンスアトラス) |
| `-ddgiraster` | DDGIのレイの取得をラスタライズへ固定する(レイトレーシングとのA/B比較用) |
| `-ddgithreshold <値>` | プローブ分類のしきい値を上書きする(`0` で分類を無効にする。A/B比較用) |
| `-megalights <番号>` | MegaLightsの手法を選ぶ(`0` = なし、`1` = 参照実装、`2` = 確率的サンプリング)。DX12かつDXR Tier 1.1が要る |
| `-megalightsrays <本数>` | MegaLightsが1灯あたりに撃つ影レイの本数。`0` にすると影を撃たず、従来のライトループと数値的に一致するはずの状態になる(移植の検証用) |
| `-megalightssamples <M>` | 確率的サンプリングが1ピクセルあたりに候補プールから引く数(RISのM)。影レイの本数はこれとは独立で常に1本 |
| `-megalightstemporal <0\|1>` | 時間再利用(前フレームの自分が選んだ灯を再投影して借りる)の有無。**既定は有効** — 1枚あたりの\|相対誤差\|中央値が6.0倍良くなる |
| `-megalightstemporalmclamp <上限>` | 履歴のMの上限(**既定は64**)。上げるとフレーム間は静かになるが、各画素の当選灯が凍結して影の縁に点描状の空間ノイズが固定される。ちらつきはデノイザの時間累積が吸うので、上げて抑える必要はない |
| `-perfdump <パス>` / `-perfdumpframes <枚数>` | **計測専用**。GPUの区間計測をウォームアップ後に平均してCSVへ書き出す。Perfログは0.05ms未満を落とし1フレームの代表値しか出さないので性能測定には使えない |
| `-megalightsdenoise <0\|1>` | デノイザ(時間累積 + エッジ停止付き a-trous)の有無。**既定は有効** |
| `-megalightsdenoiseatrous <段数>` | a-trous の段数(0で時間累積のみ)。**既定は3**。分散を段ごとに畳んで次段へ渡すようにするまでは段を増やすほど悪化していた(未フィルタの時間分散を全段で使い回していたため)。直したあとは3段と4段が底で、5段で悪化に転じる |
| `-megalightsdenoisesigma <値>` | 輝度のエッジ停止の強さ(SVGFのσ_l)。**既定は1.5**。本家の慣例値4.0まで上げると誤差もエネルギー損失も一貫して悪化する |
| `-megalightsfirefly <k>` | 時間累積の前に、5x5近傍の刈り込み平均の k 倍で上側だけ頭打ちにする。**既定は0(無効)** — 入れて測ったが、外れ値が空間的に固まっていて近傍の基準ごと押し上げるため効果がほとんど無く、エネルギーだけ失った |
| `-megalightsdenoiseframes <上限>` | 時間累積の上限フレーム数。TAAより短くすること |
| `-megalightsperturb <0\|1\|2>` | **検証専用**。蓄積開始時にシーンへ摂動を加える(`1` = 全ライトを消す / `2` = 露出を+2段跳ばす)。時間再利用の追従を測るためのもの |
| `-megalightsspatial <0\|1>` | 空間再利用(近傍が選んだ灯を借りる)の有無。**既定は有効** — 初期可視レイと組で、1本の影レイの当たり外れが支配する分散を削る。片方だけでは効かない(根拠は `docs/ImplementationDetail.md` 61.7f) |
| `-megalightsspatialmis <0\|1>` | 空間再利用の結合方式(`0` = confidence重み、`1` = 不偏化) |
| `-megalightsspatialneighbors <k>` | 借りる近傍の数 |
| `-megalightsspatialradius <ピクセル>` | 近傍を探す半径 |
| `-megalightsspatialiters <回数>` | 空間再利用を何回繰り返すか(上限2)。**既定は2** — 2回目は1回目の出力を入力にするので実効的な近傍が k から k² へ広がり、1枚あたりの\|相対誤差\|中央値(参照実装が分母)が静止で 0.0286 → 0.0244、遮蔽解除の直後に相当する条件で 0.0663 → 0.0522 になる。近傍の型板は反復ごとに変える。**時間再利用が前提**で、切ると2回目が未検証のサンプルを重ねて数えて +22% 明るくなるため、時間再利用が無いときは自動で1回へ落とす |
| `-megalightsinitialvis <0\|1>` | 初期サンプルへの可視レイ(遮蔽されたサンプルをリザーバごと殺す)の有無。**既定は有効** — 空間再利用と組で使う。殺した灯の番号を持ち回り、不偏化の分母はバイアス補正レイで厳密に数える(根拠は `docs/ImplementationDetail.md` 61.7f) |
| `-megalightsaccum <枚数>` | MegaLightsの出力を**線形空間で**その枚数だけ足し込み、達したら止める。デバッグ表示「MegaLights - 蓄積平均」と対で使う |
| `-megalightsdump <パス>` | 足し終えた合計を生データで書き出す。形式は `'K','M','L','A'` + uint32×4(幅 / 高さ / フレーム数 / 予約)+ float32×4 が幅×高さ個。**フレーム数で割ると平均になる** |
| `-autoexposure <0\|1>` | 自動露出の有効/無効。UIパネルと同じ状態を起動時から作るためのもの(画面で見ていた設定と計測の設定を揃える) |
| `-renderres <幅>x<高さ>` | 内部レンダー解像度(例: `1920x1080`)。タイル単位の処理は解像度でタイルと形状の噛み合いが変わるため、比較する2回は必ず揃えること |

環境変数 `KURENAI_NO_RT` を設定して起動すると、DX12でレイトレーシングの高速化構造(BLAS/TLAS)を構築しません。DXR対応GPUでも非対応時と同じ経路(シャドウはラスタのカスケード、反射はスクリーンスペース)で動くため、**高速化構造がVRAMと性能へ与える影響を同じ実機で切り分けられます**。23区シーンでは高速化構造だけで5.16GBを占め、RTX 4070 Tiでも専用VRAMの予算を超えます(実測は`docs/ImplementationDetail.md` 58章)。

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
| 移動速度アップ | Shift (押している間。通常の4倍) |
| 各種設定・切り替え | ImGuiパネル群(下記) |
| ImGuiパネルの表示/非表示切り替え | F1 |
| スライダーへ数値を直接入力 | Ctrl + クリック |
| 設定を既定値に戻す | 項目を右クリック |
| 項目の説明を見る | 項目にマウスを置く |

テキスト入力欄を編集している間はWASD等のキー入力がカメラ移動に取られません(F1だけは常に効きます)。入力欄からフォーカスを外すにはEscを押すか、別の場所をクリックします。

**カメラの移動速度はシーンの規模に合わせて自動で決まります。** 基準はEmeraldSquare(対角344.6m)の5 m/sで、それより大きいシーンは対角に比例して速くなり、小さいシーンは5 m/sのまま据え置きます(Project PLATEAU 東京23区は対角45kmあり、653 m/s = Shiftで2,613 m/sになります)。シーンパネルのスライダで実行中に変えられるほか、`.kscene`の`[Scene] CameraSpeed`で明示的に指定できます。

### ImGuiパネル

9つのパネルから各種設定を変更できます(F1キーで表示/非表示を切り替え可能)。

なお、シーンの読み込み中だけは画面中央に進捗(「N / M モデル」)が出ます。読み込みは専用スレッドで走り、その間は旧シーンを手放しているため画面にはUIと空しか映りません。671モデルのシーンでは数十秒かかるため、進んでいるかどうかを見られるようにしてあります(同じ内容は1秒ごとにログにも出ます)。

パネルはドッキングに対応しています。タイトルバーをドラッグすると画面端へ吸着したり、別のパネルへ重ねてタブにまとめたりできます。配置とサイズは実行ファイルと同じフォルダの`imgui.ini`へ自動保存され、次回起動時に復元されます。元に戻したいときはメニューバーの「ウィンドウ」→「レイアウトを初期化」、または「システム」パネルの同名のボタンを使います(閉じたパネルも一緒に戻ります)。中央の何も置かれていない領域には3D映像がそのまま表示されます。

- **シーン** — シーンの切り替えを行います。ボタンをクリックするとそのシーン(`.kscene`)を読み込みます。一覧は`Assets\Packed\Scenes\*.kscene`から自動的に構築されるため、`.kscene`を追加するだけで一覧に増えます(付属のシーンはSponza、Bistro (McGuire) - Exterior / Interior、Bistro (McGuire) - Interior (Lit)(内装に照明15灯と反射プローブ3個を配置し、反射プローブ・スクリーンスペースシャドウ・タイルライトカリングを実際のモデルの上で確認できるシーン)、White Furnace Test(スペキュラBRDFのエネルギー保存を目視で検証するシーン)、Material Test(粗さ0〜1の球体列+半透明ガラス球)、Light Test(ポイント/スポット/平行光の検証用シーン)、Reflection Probe Test(反射プローブの効果を目視で確認するシーン)、Multi Model Test(TRS配置の確認用)、Energy Compare - White Furnace / Sun Only(スペキュラのエネルギー補正の方式を比較するシーン。粗さ×F0の55球グリッドを、一様白環境と「環境光0+太陽光のみ」の2条件で見る)、Dragon(メッシュレット・bindless・レイトレーシングの確認用。数十万三角形の単一メッシュと、それを映す鏡面の床))。使用中のグラフィックスAPI(DX11/DX12)はメニューバーに常時表示されます。`.kscene`の再読み込み、現在のカメラを`[Camera]`の書式でコピー、**カメラの移動速度**(右クリックの「シーンから再計算」でシーン対角由来の自動値へ戻ります)もここにあります
- **レンダリング** — シーンをどう照らすかに関わる品質設定を、次の8つの節に分けて調整します
  - **AO / 間接光** — ON/OFFと手法を「SSAO / SSIL (Visibility Bitmask) / レイトレーシング (RTAO/RTGI)」から選びます。どの手法も出力は共通(rgb=間接拡散光、a=遮蔽率)なので、切り替えても後段の扱いは変わりません
    - **SSAO** — 遮蔽率だけを求めます。最も軽量です。半径・強度に加えて**サンプル数**(1〜16、既定16)を指定できます。AOパスのコストはほぼこの数に比例し、Intel UHD Graphics 620 / 1280x720 での実測では16→4でAOパスが5.82ms→2.22msになりました。減らすほど遮蔽の推定は粗くなりますが、画素ごとにカーネルをランダム回転させたうえで後段のブラーで均すため、最終画にどれだけ差が出るかはSSAO半径と間接光の強さ次第です
      - コストの内訳はサンプル数に比例する深度フェッチが主で、1サンプルあたりの計算自体は切り詰めてあります。比較に使うのはビュー空間のZ(カメラからの距離)だけなので、深度バッファの値から射影行列を使って直接求めます(ワールド座標へ戻してからビュー空間へ戻す経路より4x4の行列積1回と除算1回ぶん短く、結果は同じです)。Intel UHD Graphics 620での実測ではAOパスが8.2%短くなり、画像は最大1/255しか動きませんでした
      - このブラーはAO / 間接光の全手法で共用する5x5の分離可能フィルタで、バイリニアの補間を使って9タップで評価します(1軸の重みは`{0.125, 0.25, 0.25, 0.25, 0.125}`)。Intel UHD Graphics 620ではこのパスがサンプル命令の数だけで律速しており、16タップで書いていた頃と同じカーネルのまま9タップへ畳んだところ、同一フレーム内の他パスとの比で0.58倍になりました(画像差は最大1/255・差の出る画素0.02〜0.05%)
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
  - **ソフトウェアラスタライザ**(既定OFF) — 三角形をコンピュートシェーダーで自前にラスタライズする**比較用の経路**です。ハードウェアがブラックボックスで行っている処理——頂点変換・背面カリング・スクリーン空間への投影・エッジ関数による被覆判定・深度テスト・透視補正補間——を明示的なコードとして持ち、G-Bufferと直接突き合わせられるようにするためのものです。**既存の描画経路には一切寄与せず**、結果は「デバッグ表示」の「SWラスタ」3種でのみ見ます。**DX12かつシェーダーモデル6.6・64bit整数のシェーダー演算(`Int64ShaderOps`)・bindless(`ResourceDescriptorHeap`)のすべてに対応したGPUでのみ操作でき**、それ以外の環境では理由を表示して常に無効です(基準実機のIntel UHD Graphics 620では動きません)
    - 深度と三角形IDを1つの64bit値へ詰め、`InterlockedMax`1回で深度テストと書き込みを同時に行います(visibility buffer)。深度テストとIDの書き込みを別々のアトミックにすると「勝者が深度を書いた後で敗者がIDを書く」競合が必ず起きるためです。Reverse-Zにより「大きいz=手前」なので`InterlockedMax`がそのまま最近傍を選び、深度が同値なら三角形番号の大きい方が勝つので**結果は完全に決定的**です
    - 頂点とインデックスは、描画に使っているバッファそのものをbindlessで直接読みます(専用のコピーを持ちません)。中間の三角形レコードも持たず、三角形番号からジオメトリを引き直して再変換します
    - **巨大三角形のしきい値**(既定4096画素=64x64相当)を超えた三角形は、1スレッドで塗らず「1スレッドグループ=1三角形」の専用パスへ回します。1スレッド1三角形のままだと画面全体を覆う三角形で1スレッドが数百万回ループし、描画が長時間止まるためです。しきい値は**2つの経路が一致していることを確かめる対照実験**に使います——極端に大きくすればすべて小三角形パス単独になり、下げれば巨大三角形パスへ回ります。Cherry Tree Test(360万三角形 / 3840x2160 / DX12 / Debug / RTX 4070 Ti)での実測では、既定4096と「全部を小三角形パスへ回す」設定で**描画結果が完全に一致(最大差0)**しました(SWRasterパスの時間は中央値3.6ms→16.2msに伸びます)
    - **フェーズ1の制約**: アルファカットアウト未対応(植栽・日除けは板になります)、近平面クリッピング未実装(壁に近づくと三角形が消えます)、法線マップは適用しません。巨大三角形リスト(容量4096)が溢れると画面左上がマゼンタになります
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
  - **積雲の「ボリュームとして描く」**(既定ON)を有効にすると、雲を雲底から雲頂までのスラブとしてレイマーチします。厚みと**レイマーチ段数**(1〜32、既定12)を指定できます。段数は雲パスのコストの主なつまみで、1段ごとにウェザーマップのfBm(4オクターブ)と3Dノイズ2枚を引くためコストはほぼこの数に比例します。Intel UHD Graphics 620 / 1280x720 での実測では12→6で雲パスが3.93ms→2.84msになりました(半分にならないのは、レイマーチの外に段数へ比例しない固定分——自己影5回と基底1回のfBm——があるためです)。減らすと雲の内部の階調が段状に粗くなります(輪郭ではなく芯の明暗に出ます)
    - オクターブ数と自己影の段数は実行時には変えられません。これらはfBmの値そのものを変えるため、動かすと背景の雲(ボリューム経路)と水面に映る雲・大気遠近の雲(平面経路)で形が食い違ってしまうからです
  - **EV100** は実在の写真露出値です。太陽/環境光/ポイント・スポットライトすべてに一様にかかるシーン全体の露出で、自動露出が有効なときはバッファの数値レンジを決める基準値として働きます。シーン全体の自発光の強度倍率もここです
  - ポイント/スポットライトの一覧・追加・複製・削除・種別の切り替え・パラメータ編集(強度はカンデラ/ルクス)ができます。ライトごとにスクリーンスペースシャドウを落とすかも切り替えられます
- **レンダリング → ジオメトリ** — **深度プリパス**(既定ON)のON/OFF。G-Bufferを描く前に不透明ジオメトリの深度だけを先に埋め、G-Buffer側の深度比較をGREATER_EQUAL(Reverse-Z)にして最前面の断片だけを通します。隠れる画素のピクセルシェーダー——6本のテクスチャサンプルと6枚のレンダーターゲットへの書き込み——が早期Zでまるごと省けます
  - **絵は変わりません。** 深度が等しい最前面の断片だけが通るため書かれる値は同じで、実測(Sponza / 1280x720 / DX11)で3Dビューポートがビット一致することを確認しています
  - ジオメトリを1周ぶん余計に描くのと引き換えなので、オーバードローが小さいシーンでは損になります。損益分岐は実測から**オーバードロー1.17倍**です(G-Bufferパスの内訳は「頂点処理+ラスタライズ+ドロー発行」が12%、残る88%がピクセルシェーダー側でした)
  - 実測(Intel UHD Graphics 620 / 1280x720 / DX11。熱ドリフトを除くため同フレームの`Tonemap`との比で比較しています):

    | シーン | プリパス無 | プリパス有 | 比 | 内訳から求めたオーバードロー |
    |---|---|---|---|---|
    | Sponza (26万三角形) | 6.44 | 4.28 | **0.671** | 2.05倍 |
    | MultiModelTest | 1.419 | 0.769 | **0.542** | 2.34倍 |
    | MaterialTest (球13個+空) | 0.129 | 0.128 | 0.99 | ほぼ1倍 |

    Sponzaの絶対値では`GBuffer 14.11ms`が`GBuffer 6.62ms + DepthPrepass 1.42ms`になりました。**空が大半を占めるMaterialTestでも悪化していません**
  - **「メッシュレット描画」が有効な間は自動で止まります。** プリパスは頂点シェーダー経路で深度を書くため、G-Buffer側がメッシュシェーダーで描くと変換の丸めが一致する保証が無く、深度が1ulpずれた面がGREATER_EQUALを通らずに消えるためです。同じ理由でプリパスとG-Bufferは頂点シェーダーを共有しています(写して2本にしません)
  - アルファカットアウト(glTFの`alphaMode=MASK`)のメッシュだけは、切り抜きを反映しないと深度に嘘が入る(G-Buffer側の`clip`で穴が開いたまま「深度は手前にある」状態になり背景が抜ける)ため、同じ判定の`clip`だけを行うピクセルシェーダーを通します。不透明マテリアルはピクセルシェーダーを持ちません(段ごと省きます)
- **レンダリング → メッシュレット** — メッシュを頂点64個・三角形124個までの塊(メッシュレット)に分け、増幅シェーダーが塊ごとにカリングしてからメッシュシェーダーで描く経路のON/OFFと、その中のカリングの設定です。**DX12かつメッシュシェーダー Tier 1・シェーダーモデル6.6に対応したGPUでのみ操作でき**、それ以外の環境では理由だけが表示されます
  - **メッシュレットを色分けして表示** — 塊ごとに違う色でアルベドを塗ります。分割のされ方を目で確かめるためのもので、法線・深度・モーションベクターは通常どおり書くため他のパスは破綻しません
  - **Hi-Zオクルージョンカリング**(既定ON) — メッシュレットのバウンディング球をHi-Zへ投影し(**どのフレームのHi-Zを読むかは下の「Hi-Zを深度プリパスから作る」で決まります**)、「視錐台の内側にあるが手前の何かに完全に隠れている」塊を落とします。視錐台カリングは「視界の外」しか落とせないため、街路のように視界内のほぼ全部が手前の建物に隠れる場面ではこちらしか効きません
    - **有効な間だけHi-Zを毎フレーム構築します。** 無効にするとHi-Zパスごと止まります(「デバッグ表示 → Hi-Z」を選んでいる間を除く)
    - **純粋な最適化で、有効/無効で最終画像が変わってはなりません。** 実測(RTX 4070 Ti / DX12 / Release / 1280x720 / PlateauTokyo23ku の街路)では、ON/OFFの画素差(0.088%)が**同一設定どうしの差(0.400%)を下回りました** ―― つまり絵は変わっていません
    - **オクルージョンの半径倍率**(既定1.0) — 判定に使う球を膨らませる量です(**前フレームのHi-Zで判定するときだけ効きます**。今フレームのHi-Zならずれる原因が無いので膨らませません)。上げるほど間引きが減り安全側になります。1フレームぶんのカメラ**移動**による視差ずれはこの倍率とは別に、移動距離を半径へ足すことで補正済みです。この倍率が埋めるのは、バウンディング球がメッシュレットの実体より緩いことと、カメラの**回転**による見え方の変化です
  - **カリングの間引き数を数える**(既定ON) — 増幅シェーダーが判定数と間引き数を数え、数フレーム遅れでCPUへ読み戻してパネルに表示し、`Perf`ログにも1秒ごとに残します。**保守的な判定が正しく働いていれば絵は1画素も変わらないため、効いているかどうかは数値でしか確かめられません。**オクルージョンは視錐台+コーンとは別のカウンタで出ます
    - 実測(同上): **俯瞰(1400mから真下)ではオクルージョンの間引きが0.0%、街路では81.0%。** 俯瞰の0.0%は「効いていない」ではなく、真下を見下ろす視点では建物どうしがほとんど重ならないためです。**片方の視点だけでは合否を決められません**
    - 同じ街路でGBufferパスが**2.16ms → 0.17ms**、GPU合計が**4.48ms → 2.12ms**(各10フレームの中央値、いずれもGPU待ち0.00ms)。Hi-Z構築そのものは0.07msです
  - **Hi-Zを深度プリパスから作る**(既定ON) — Hi-Zミップチェーンを深度プリパスの直後に、**そのフレームの深度から**作ります。**入れるとG-Bufferの判定から1フレーム遅れが消え**、投影に前フレームの行列を使う必要も、視差ぶんを保守的に膨らませる必要も無くなります(カメラが動いても遮蔽の判定がずれません)。切ると従来どおりG-Bufferの後で作り、次フレームに前フレームのものとして読みます。**深度プリパスが無効なフレームでは、この項目によらず常にそちらになります**(深度が埋まっていないため)
    - **深度プリパス自身は今フレームのHi-Zを使えません**(そのHi-Zをプリパスの出力から作るため)。プリパスは従来どおり前フレームのHi-Zで判定します — 保守側へ倒れるだけで絵は壊れず、判定を捨てるとプリパスが描くメッシュレットが街路の実測で11,321から103,868へ(約9倍に)戻ります
    - **間引き数からは切り替わったか分かりません。** カメラが止まっていれば前フレームのHi-Zと今フレームのHi-Zは同じ中身になり、新旧どちらでも同じ数が出ます。`Perf`ログの「Hi-Zの出どころ」の行で経路そのものを確認してください
    - **この項目のGPUコストはまだ測れていません。** 検証機のVRAM使用量が予算を超えており(9811.8 MB / 9442〜9677 MB)、GPUがページングするため総GPU時間のA/Bが取れませんでした
  - **モデル単位のGPUカリング**(既定ON) — 上の判定はメッシュレット単位のため、遮蔽されたモデルでも増幅シェーダーは起動します。こちらはコンピュートシェーダーがモデルのワールドAABBを視錐台とHi-Zで判定し、生き残ったモデルの`ExecuteIndirect`引数をGPU上に作ります。**DX12かつメッシュシェーダー対応環境でのみ動きます**
  - **カリング結果で間接描画する**(既定ON) — 生き残った候補だけを`ExecuteIndirect`で発行します。深度プリパスとG-Bufferの1モデル1ドロー経路が、CPUのループではなくこの引数で描かれます。**切っても判定と計数は動き、描画だけが従来のCPUループへ戻ります** ―― 「判定が正しいか」と「間接描画が速いか」を別々に確かめるためにトグルを分けてあります
    - 切り替わったことは、プロファイラの**ドローコール数**で分かります(PLATEAU 東京23区の街路で G-Buffer 145 → 1 / 深度プリパス 145 → 1)。`Perf`ログには区画ごとの発行数と、GPUの視錐台判定をCPUの判定と突き合わせた結果が出ます(食い違えば警告になります)
    - 実測(RTX 4070 Ti / DX12 / Release / 1280x720 / PLATEAU 東京23区 767モデル): 視錐台の判定は**CPUと1件も食い違わず**、街路では視錐台を通った145モデルのうち**58モデル(40%)が完全に隠れており、俯瞰では0**でした。**総GPU時間のON/OFF差は分解能以下です**(街路 6.43ms / 6.59ms、俯瞰 6.81ms / 6.70ms。差の方向が一致しません) ―― 削れる仕事は既にメッシュレット単位の判定で削れており、この段で消えるのは遮蔽されたモデルの増幅シェーダーを起動するぶんだけです
- **レンダリング → DDGI (拡散グローバルイルミネーション)** — DDGIのON/OFF・強度・**レイの取得**・1フレームに更新するプローブ数・**更新モード**。`.kscene`に`[GIVolume]`があるシーンでのみ動作します。
  - **レイの取得**(DXRが使える環境では既定が「レイトレーシング」) — プローブへ入れる放射輝度と距離をどう集めるかを選びます。**DX11とDXR非対応の環境では選択肢そのものを出さず、ラスタライズのまま動きます。**
    - **ラスタライズ** — プローブ1個につきシーンを6回描きます。1フレームの描画回数が「プローブ数 × 6面 × 不透明メッシュ数」になるため、メッシュの多いシーンでは**1フレームに更新するプローブ数が自動的に抑えられます**(抑えたことはログに出ます)。`BistroInteriorLit`(不透明59メッシュ)では16個/フレームの指定が8個へ制限されます
    - **レイトレーシング (DXR)** — 1スレッド1レイでプローブのキューブを直接埋めます。メッシュ数はBVHが吸収するので上記の制限が掛からず、**太陽の影をカスケードシャドウマップではなく影レイで求める**ため、カメラから遠いプローブにも影が落ちます(ラスタライズ側の既知の制約が消えます)。
      代わりにヒット面では**法線マップ・ベイク済みAO・bent normalを引けません**(レイトレーシング側の頂点属性に接線とライトマップUVが無いため)。その分だけ絵が違います
    - 実測(`BistroInteriorLit` / 1280x720 / Release / DX12 / RTX 4070 Ti。各経路1回ずつ起動し、その実行内の43サンプルの平均): `DDGIUpdate`パス1本あたりのGPU時間が**0.958ms → 0.464ms**、CPUフレーム時間が**1.19ms → 0.66ms**、GPUフレーム時間が**9.02ms → 8.77ms**。ラスタライズは予算の都合で8個/フレームに制限される一方、レイトレーシングは16個を焼いてなお総時間が短くなります(1プローブあたり約2.1倍速い)
    - **プローブ分類**(レイトレーシング選択時のみ) — 壁や什器の内部に埋まってしまったプローブを
      サンプリングから外します。埋まったプローブはほとんどの方向で面の裏側しか見えず、
      「そこには光が無い」という誤った情報を周りの面へ配ってしまいます。
      裏面ヒット率は「デバッグ表示 → DDGI - プローブ裏面率」で分布を確認できます
      (白いほど埋まっている)。**しきい値を変えても焼き直しは不要**です
      (アトラスには率そのものが入っており、しきい値は読み出し時に掛かります)
      - **既定のしきい値0.5の根拠** — 「全レイの半分より多くが裏面 = 外より内側にいる」という判定です。
        `Sponza`(1152プローブ)は分布がきれいな二山で、76%が0.05未満・21%が0.5超、その間はほぼ空なので
        谷のどこに置いても同じ集合になります。一方 `BistroInteriorLit`(480プローブ)は二山にならず
        連続的に減るだけで、0.5超は1.9%(9個)しかありません。
        0.5では**両シーンとも「わずかに明るくなり、暗くなる画素は0.1%未満」**で向きが一致します
      - RTXGIの既定値である0.25は採っていません。Bistroの連続分布を途中で切るため壁の領域が
        18〜22%暗くなり、しきい値0.75(3個だけ無効)にすると同じ領域の変化が+0.004%まで落ちることから、
        その暗化は「明らかに埋まったプローブ」ではなく中間の率を持つプローブを落とした結果と分かるためです
      - **この百分率は額面どおりには受け取らないでください。** `BistroInteriorLit`は画面がほぼ真っ暗
        (平均輝度1.19/255)なので「-18%」は平均0.5階調ほどで、`Sponza`側の変化はほとんどが±1階調です。
        個々の画素では41〜66階調動いており実体はありますが、百分率だけを根拠にはできません
    - 一様な白の環境(`FurnaceTest`)でのDDGIイラディアンスは、レイトレーシングでは基準のグローバルIBLと**完全に一致**します(アトラス全体で186・標準偏差0)。ラスタライズは平均183.65・標準偏差3.28・最小168で、アトラスの約60%のテクセルが186を下回ります
  - **更新モード**(既定は「常時更新」) — いつ焼くのをやめるかを選びます。どのモードでも時間分割(1フレームに指定した個数ずつ)であることは変わりません。**プローブ更新はIntel UHD Graphics 620 / 1280x720での実測でGPU 40〜47ms・CPU 30msを占めており、どちらもフレームの最大要素でした。** 止めている間はこれがゼロになります
    - **常時更新** — 常に焼き続けます。ライトや時刻が動き続けるシーンでも必ず追従します
    - **多重バウンスまで焼いて停止** — 太陽・時刻・影・ライト・IBL・自発光が変わらなくなったら、ヒステリシスを使わない上書きで4巡してから止めます。プローブのキャプチャは前巡のアトラスを読むので1巡につき1バウンス積み上がります
    - **一巡だけ焼いて停止** — 同じく上書きですが1巡で止めます。最も速く止まります
    - **【止めるモードの判定を巡回数からバウンス数へ変えました】** 以前は「残差が1%を切るヒステリシス由来の巡回数」(0.97なら152巡)を停止条件にしていましたが、これは**1巡が何フレームかを見ていない**ため、プローブが多いボリュームでは実質止まりませんでした ― Sponza(1152プローブ・4個/フレーム)では1巡288フレーム、152巡で**43,776フレーム ≒ 53分**かかり、実測でも180秒回して止まりませんでした。上書きなら目標値そのものが1巡で入るので、複数巡が要るのは多重バウンスの積み上げだけです。この変更でSponzaは**140秒で停止**するようになりました(FPS 20.5 / GPU 49.59ms / CPU 3.38ms、`DDGIUpdate*`が内訳から消え`DDGIResolve 1.83ms`だけが残る)
      - **ただし4巡と1巡の差はまだ確認できていません。** Sponzaの同一カメラで両者を撮り比べたところ3Dビューポートは**ビット一致**でした(「常時更新」を200秒回したものとも一致)。多重バウンスの寄与を分離できる計測方法をまだ持っていないため、4という値は保守的に置いたものです
    - どのモードでも、焼き上がりに影響する状態が変わると**自動で再開します**。判定には反射プローブの「変化を検出して焼き直す」と同じ署名を使っています
  - **1/2解像度で評価する**(既定はOFF) — 拡散間接光を内部レンダー解像度の1/2で求め、深度を見てアップサンプルします。DDGIのサンプリングは1画素あたり周囲8プローブ×2テクスチャ＝16サンプルを踏むため重く、実測(ProbeTest / 1280x720 / DX11)では`Lighting`パス23.9msのうち**10.2ms**を占めていました。有効にすると`Lighting`が21.8ms→12.6ms、新設の`DDGIResolve`パスが3.4msで、**正味5.8msの削減**になります
    - **雲の低解像度化(下記の「雲」)と違い、これは厳密ではありません。** 雲は視線方向だけの関数なので低解像度化しても数学的に等価でしたが、DDGIは面の位置と法線の関数なので、ジオメトリの輪郭をまたぐと手前の面の間接光が奥へ滲みます。深度の近いテクセルだけを採用するアップサンプルで抑えていますが近似であり、そのため**既定はOFF**です(品質プリセットの低/中が有効にします)
    - DDGIの更新を止めた状態でこのつまみだけを切り替えた比較では、差は平均0.111/255・最大8/255・8を超える画素は0.00%でした
    - **アップサンプルは1画素5サンプルです。** 深度を見たバイラテラルには周囲4テクセルそれぞれの「代表している深度」が要りますが、以前はそれを**全解像度の深度から4回引き直して**いました(合計8サンプル)。`DDGIResolve`パスが自分で使った深度を2枚目のレンダーターゲットへ書き出すようにしたので、合成側は低解像度テクスチャへの`GatherRed`**1回**で4テクセルぶんを取れます。値は同一(どちらもポイントサンプラーで同じUVを引いている)なので**絵は変わりません**
      - 実測(Sponza / 1280x720 / DX11。同フレームの`Tonemap`比)で`Lighting`が**5.757 → 4.920(−14.5%、約1.69ms)**。3Dビューポートはビット一致でした
      - あわせて4タップのループを展開し、重みの計算を4成分のベクタ演算にまとめてあります。**ここをループ + 添字アクセスで書いてはいけません** — ローカル配列でも`float4`の添字でもfxcのコンパイル時間が爆発し、実測でこのシェーダーのコンパイルが65秒から171秒へ延びました
  - 実測(ProbeTest / 1280x720 / DX11)では「一巡だけ焼いて停止」で **8.9fps → 16.2fps**(GPU 122.5ms → 65.1ms、CPU 35.3ms → 5.3ms)になりました
  - **拡散の間接光(壁の色が床へ回り込むような効果)を出せるのはDDGIだけです**(反射プローブは鏡面専任です。下記)。無効にすると、あるいはDDGIボリュームの外に出ると、拡散はスカイボックス由来のグローバルIBLへ戻ります(加算ではなく差し替えです)
- **レンダリング → 水面** — 水面(`.kscene`の`[Model]Water = true`で指定したインスタンス)の法線マップのスクロールを止める「水面アニメを止める」トグルと、波のスケール・速さ・強さのつまみがあります。対象インスタンスは「デバッグ表示」の「水面マスク」で白く表示され確認できます。水面には不透明ジオメトリの鏡像(平面反射)と空・雲が映ります
- **反射プローブ** — 反射プローブのON/OFFと、プローブの一覧・追加・削除・位置や影響範囲(形状(球/箱)・半径・箱の半径・箱の向き・ブレンド距離)の編集ができます。**反射プローブは鏡面(映り込み)専任です**。以前は拡散の間接光もプローブ側で計算できましたが、その経路は廃止し、拡散はDDGIへ一本化しました。プローブの中身はシーンのジオメトリやライトに依存するため、シーン読み込み時と位置を動かしたときに自動で焼き直されます(影響範囲だけを変えた場合は焼き直し不要です)。ライトや時刻を変えた後に焼き直したい場合は「焼き直す」ボタンを押してください
  - 「更新モード」で焼き直しの方式を切り替えられます — **焼き込み**(シーン読み込み時と「焼き直す」ボタンのみ。実行時コストはゼロですが、ライトや時刻を動かしても反射は焼いた時点のまま止まります)、**変化を検出して焼き直す**(太陽・時刻・ライトの変化を検出して自動で焼き直す)、**毎フレーム少しずつ**(1つのプローブを12フレームかけて更新します。前半6フレームで1面ずつ撮り、後半6フレームで1面ぶんずつぼかしてから次のプローブへ回ります。ぼかしの処理は実測で1フレームあたり1〜3ミリ秒に収まり、分割していなかった頃の「6フレームに1回だけ20ミリ秒前後」という山は出ません)
  - ただし**時刻を大きく動かして場面全体の明るさが2倍以上変わったときだけは、「焼き込み」でも自動で焼き直します**。プローブには焼いた時点の明るさの数値が入っているため、これを持ち越すと反射だけが桁違いの明るさになってしまうためです
  - 「視差補正」「プローブのブレンド」は視差補正・プローブ間ブレンドの有無を切り替えるもので、無効にしたときの見た目(壁際での反射位置のずれ、影響範囲の境界の継ぎ目)と見比べられます
  - 「距離キューブを使う」は視差補正の方式です。有効にするとキャプチャ時に一緒に焼いた距離を辿って実際の形状に反射を当てます(無効なら「部屋を直方体とみなす」従来の方式)。強く曲がった鏡面で反射像が二重に割れるのが軽減される一方、距離の解像度に由来する階段状のエッジが反射に乗るため**既定は無効**です
  - 「遮蔽判定(光漏れの抑制)」も同じ距離を使い、プローブから見えない位置(壁の向こう)のピクセルでそのプローブの寄与を落とします。ただしプローブが少ないうちは落ちた分をより明るい空由来のIBLが埋めるため、物体の真下が逆に明るくなることがあり**既定は無効**です
- **デバッグ表示** — Presentパスで表示する内容をドロップダウンで選択(最終結果 / アルベド / 法線 / マテリアル(R=金属度, G=粗さ, B=遮蔽マップ) / 自発光 / 深度 / シャドウマップ / RTシャドウ(太陽の可視率) / IBL(プリフィルタ済み鏡面・BRDF LUT・検証用の拡散イラディアンス) / ブルーム / ライトタイル(タイルごとのライト数のヒートマップ) / 反射プローブ(キャプチャ結果・影響範囲の色分け・プローブから見た距離) / モーションベクター(TAAが使う速度バッファ。静止で灰色、カメラを動かすと移動方向に応じて色が付く) / シーンカラー(生HDR・トーンマップなし)(トーンマップもガンマも通さないリニア値をそのまま表示。値を実測したいとき用) / DDGI(イラディアンスと距離モーメントのオクタヘドラルアトラス) / 水面マスク(水面のマテリアルIDを白黒表示) / SWラスタ(自前ラスタライザのフラット陰影・深度(生値)・法線。深度と法線はハードウェア側の「深度 (生値)」「法線」と同じ表示モードで出るので、そのまま並べて差分が取れる)等、各パス中間結果のデバッグ表示)。間接光のように値が小さいバッファを見るための輝度倍率も指定できます。中間バッファの精度構成(HDR / Legacy 8bit)を切り替えて画質を比較することもできます
- **ストリーミング** — モデルインスタンスの常駐状態(常駐 / 読み込み中 / 未読み込み)とLOD段を、XZ平面の俯瞰図に色分けして表示します。カメラの位置と向き、縮尺の目盛りも重なります。**ストリーミング本体とモデルLODはまだ実装されていないため、現状は全件が「常駐」・LOD段0のまま動きません**(表示だけを先に用意してあります)。距離に応じた読み込みと破棄を入れたときに、「破棄が早すぎる」「範囲内なのに読み込まれない」といった破綻を位置ごとに見るためのものです
- **システム** — 垂直同期、フレームレート制限(既定でON・60fps。30/60/120から選択可能)、性能のログ記録、品質プリセット、内部レンダー解像度、超解像(FSR1相当)、グラフィックスAPI、パネルの表示/非表示、レイアウトの初期化、現在のUIスケール
  - **性能をログに記録**(既定でON) — FPS・CPU/GPUフレーム時間・GPU待ち・その期間の最悪フレーム時間を**1秒ごとに1行**、続けて**GPU・CPUのパス別内訳をそれぞれ重い順に**ログファイルへ書き出します。プロファイラパネルの表示は実行中しか見えず後から比較できないため、最適化の前後を数値で突き合わせる用途に使います。出力は1秒に1回だけなのでフレーム時間への影響はありません。起動時のログには使用中のGPU名とメモリ量も残ります
  - **品質プリセット**(低 / 中 / 高。既定は高) — 重い設定をまとめて振ります。**押した時点で一括適用されるだけ**で、その後は「レンダリング」「ポストプロセス」パネルから個別に上書きして構いません(個別に変えてもComboの表示は追従しません)。**「高」はエンジンの既定値ではなく、そのシーンを読み込んだ直後の状態へ戻します** — `.kscene`はSSRやTAAを自分で指定できるため、静的な既定へ戻すとシーンが要求した反射が消えてしまうからです。シーンを切り替えると選択は「高」へ戻ります
    - **低** — DDGIのプローブ更新を2個/フレームへ・更新モードを「一巡だけ焼いて停止」へ・1/2解像度評価を有効化、SSAOのサンプル数を4へ、反射(SSR)・平面反射・ボリュメトリック積雲・巻雲・星・TAA・ブルーム・スクリーンスペースシャドウを無効化
    - **中** — DDGIを4個/フレームへ・更新モードを「多重バウンスまで焼いて停止」へ・1/2解像度評価を有効化、SSAOのサンプル数を8へ、反射(SSR)・TAA・ブルーム・スクリーンスペースシャドウを無効化(平面反射は1/4解像度、ボリュメトリック積雲はレイマーチ6段で残します)
    - どの段でも**シャドウと内部レンダー解像度は変えません**。Intel UHD Graphics 620での実測でシャドウは4カスケード合計が常に1ms未満で、落としても得が無かったためです。解像度は独立したつまみとして下の「解像度」で指定します
    - 実測(Intel UHD Graphics 620 / 1280x720 / DX11 / Release)では、DDGIと水面を持つ`MontSaintMichel`で **8.6fps → 23.3fps**(GPU 130.4ms → 43.3ms、CPU 26.4ms → 8.6ms)になりました
  - **内部レンダー解像度**(既定1280x720) — G-Buffer以降すべての中間バッファの解像度です。**ウィンドウサイズとは独立**していて、表示時はアスペクト比を保ったままウィンドウへ拡大縮小します(余る側にレターボックス/ピラーボックスが出ます)。1280x720 / 1600x900 / 1920x1080 / 2560x1440 / 3840x2160 から選べるほか、「現在のウィンドウサイズに合わせる」で等倍表示にもできます(押した時点で1回だけ適用され、その後のウィンドウリサイズには追従しません)。上げるほどVRAM使用量とフレーム時間が増え、変更時にTAAの履歴は一度破棄されます
  - **超解像(FSR1相当)**(既定OFF) — 低い内部解像度で描いた絵を、**EASU**(方向性エッジ再構成)と**RCAS**(シャープ化)で出力解像度へ拡大します。AMD FidelityFX Super Resolution 1.0 の移植で、DX11 / DX12 の両方で動きます。有効にすると上の解像度は**「出力解像度」**の意味になり、実際に描く内部レンダー解像度は品質モードの倍率で割った値が自動で設定されます(8の倍数へ切り捨て)
    - **品質モード** — Ultra Quality (1.3倍) / Quality (1.5倍) / Balanced (1.7倍) / Performance (2.0倍)。出力1280x720ならQualityで内部848x480になります
    - **シャープネス**(既定0.25) — RCASの強さです。0で無効。拡大後の出力解像度で効くため、**超解像が有効な間はトーンマップ側のシャープネス(ポストプロセスパネルのTAAシャープネス)が自動的に0になります** — 内部解像度で戻した高域をそのまま拡大すると、オーバーシュートの縁まで一緒に引き伸ばされて太い縁取りになるためです
    - **UIはぼけません。** ImGuiは拡大後のバックバッファへそのまま描かれます。またデバッグ表示(最終結果以外)を選んでいる間は超解像パスを止め、中間バッファを従来どおり等倍で表示します
    - 実測(Intel UHD Graphics 620 / Sponza / DX11 / Release。同一プロセス内でネイティブ1280x720を75秒測ってから内部848x480へ切り替えて75秒測ったもの。解像度に依存しないシャドウとDDGIは除いた合計):

      | | ネイティブ1280x720 | 内部848x480+超解像 | 比 |
      |---|---|---|---|
      | 解像度依存パスの合計 | 35.00ms | **25.86ms** | **0.739** |
      | うち超解像(EASU+RCAS) | — | 4.17ms | |

      画素数比は0.442ですが各パスは0.58〜0.61倍にしかなりません(1パスあたりに解像度へ比例しない固定分があるため)。超解像自体は、解像度を下げて浮いた13.31msのうち31%を返しています
    - 絵の精細さ(中央1160x640の|ラプラシアン|平均。大きいほど精細)は **バイリニア拡大1.478 → EASU+RCAS 1.983** で、単純な拡大より**34%多く高域が戻ります**。ネイティブ1280x720は3.906で、空間手法である以上ここには届きません(存在しないサンプルは作れないため)
    - **フレーム全体で速くなるとは限りません。** DDGIのプローブ更新のように解像度に依存しないパスが支配的なシーンでは、削減できるのは解像度依存分だけです
  - **グラフィックスAPI** — DX11 / DX12 を**再起動なしで切り替えられます**。切り替えるとエンジンを作り直すため、ウィンドウが一度閉じてシーンが読み直されます(数秒かかります)。シーンと内部レンダー解像度は引き継がれますが、**それ以外の設定は既定値へ戻ります**。レイトレーシング(RT反射 / RTシャドウ / RTAO)はDX12かつDXR Tier 1.1対応のGPUでしか選べないため、DX11へ切り替えるとそれらの選択肢は消えます
- **プロファイラ** — FPS、CPU/GPUフレーム時間をパスごとに表示。フラスタムカリングの「判定した数 / 間引いた数」も出します(モデル単位とメッシュ単位を別々に)。**効くシーンが逆です** — 実測(DX11 / Release / 1280x720)で、Emerald Square(1モデル / 約220メッシュ)はモデル単位が0%・メッシュ単位が1.5%、PLATEAU 東京23区(671モデル / 1メッシュずつ)はモデル単位が84.0%・メッシュ単位が0%、PLATEAU 丸の内 LOD2(6モデル / 3,418メッシュ)はモデル単位が4.8%・メッシュ単位が59.7%でした(いずれも各シーンの初期カメラで固定したときの値。カメラを振れば変わります)

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
| デモ画面の切り替え | 数字キー(1〜9、10番目は`0`) |
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
| 9 | 折れ線 — `DrawPolyline` と `DrawLine` の連結を、不透明・半透明で並べて比較 |
| 0 | ドット絵 — 32pxの元絵を1/2/3倍に拡大。`F` でフィルタ(Point/Linear/Anisotropic)、`C` でアドレスモード(Clamp/Wrap)、`V` で整数倍スナップの入り切り、`R` で推奨設定へ戻す |

デモ5で使うアトラスも、初回起動時に `DemoAtlas.bmp`(4x4区画の256x256)を生成して読み込みます。
デモ0で使うドット絵は、初回起動時に `DemoPixelArt.tga`(32x32)を生成して読み込みます
(ドット絵にBMP/PNGではなくTGAを使う理由は `docs/KurenaiEngine.html` 3.3節)。

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
- `ThirdParty/SourceModels/EmeraldSquare_v4_1/` — [NVIDIA Emerald Square](https://developer.nvidia.com/orca/nvidia-emerald-square)(ORCA)。
  商業街区4ブロックの屋外シーン。**このアセットだけは`Assets/Source/`を経由せず、展開先から直接
  パックします**(配布物がFBX+外部DDSで完結しており、中間形式へ変換する必要がないため)。
  `Tools/import_emerald_square.ps1`が取得から配布まで行います。
  **ライセンスはCC BY-NC-SA 3.0 Unported(非商用のみ)** で、他のアセットと条件が違う点に注意してください
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
- `Assets/Source/PenumbraTest/` — 球光源の半影の測定用。半径0.6mの球を**軸上の指定した高さ1個だけ**
  浮かべた形状で、高さ違い(4/6/7/8m)のモデルを作る。`Tools/generate_penumbra_test.py` で再生成できる。
  **1モデル1遮蔽物**なのは、複数個を並べると測れないため(光源を遠くに置くと真上から見て影が遮蔽物の
  真後ろに隠れ、近くに置くと軸外の影が床から落ちる)
- `Assets/Source/ProbeTest/` — 反射プローブの検証用シーン。中央の仕切り壁で「密閉の西室(暖色)」と
  「天井が開いた東室(寒色・太陽光)」に分かれたホールと、その間を貫く金属球列(`metallic=1.0`、
  粗さ0.05)。床は磨いた石(粗さ0.06)で、壁のエミッシブ帯の映り込みから視差補正の効きを読み取る。
  各室には半透明のガラス板(`alphaMode=BLEND`)も1枚ずつ立ててあり、半透明にはSSRが効かないため
  プローブの有無がそのまま映り込みの違いとして現れる。
  `Tools/generate_probe_test.py` で再生成できる
- `Assets/Source/CherryTree/` — 桜(ソメイヨシノ)の満開の木。**メッシュシェーダー(メッシュレット)
  描画の検証用**に、板ポリゴンの花房カードを十数万枚使う高密度モデルとして作ってある
  (1本あたり約100万三角形 / メッシュレット約5万個)。乱数シード違いの3個体と地面を
  `Tools/blender_cherry_tree.py` で再生成できる(Blender 2.82が必要):

  ```
  Tools\run_blender.ps1 -Script Tools\blender_cherry_tree.py ^
    -ScriptArgs @("--export","Assets/Source/CherryTree/CherryTree_s1.gltf","--seed","1")
  Tools\run_blender.ps1 -Script Tools\blender_cherry_tree.py ^
    -ScriptArgs @("--export-ground","Assets/Source/CherryTree/CherryGround.gltf")
  ```

  パックするときは**`--force`を付けること**。手続き生成したテクスチャを描き直しても、
  出力先に`.ktex`が残っていると変換がスキップされ、古いテクスチャのまま描かれます:

  ```
  Tools\KurenaiPacker\Build\Bin\x64\Release\KurenaiPacker.exe --force ^
    Assets\Source\CherryTree\CherryTree_s1.gltf -o Assets\Packed\CherryTree\CherryTree_s1.kmodel
  ```

  花房は`alphaMode=MASK`(アルファカットアウト)です。**`BLEND`にしてはいけません** —
  半透明フォワードパスへ回され、メッシュレット描画の対象外になります。
  また**エンジンは`CULL_BACK`固定で両面描画を持たない**ため、カードは巻き順と法線を
  反転した複製を必ず持ちます
- `Scenes/` — `.kscene`(シーンファイル)。`Assets/`の外にあり**Git管理対象**。
  このうち`BistroInteriorLit.kscene`は、Bistro内装の照明器具の実際の位置に合わせてポイントライトを15灯置き、
  部屋の形に合わせた反射プローブを3個置いたシーンです。反射プローブ・スクリーンスペースシャドウ・
  タイルライトカリングを、テスト用の合成シーンではなく実際のモデルの上で確認できます
  (`BistroInterior.kscene`はライトもプローブも持たない素の読み込み確認用として残してあります)。
  `ScreenSpaceShadowTest.kscene`(接触影の目視確認用)と`ManyLightsTest.kscene`(タイルライトカリング用に
  ポイントライトを格子状に64灯配置)の2つだけは手書きではなく`Tools/generate_shadow_test_scenes.py`で
  生成します(ジオメトリは`LightTest.kmodel`を流用するため、生成されるのは`.kscene`だけです)。
  半影の測定用の`PenumbraTest.kscene`・`PenumbraH{4,6,7,8}.kscene`も同じスクリプトが生成します
  (こちらは`PenumbraTest`のモデルを使うため、先に`Tools/generate_penumbra_test.py`を走らせてパックします)。

  **MegaLights用の2つ**も生成物です。どちらも`-dx12 -megalights 2`で起動しないと
  MegaLightsは走りません(既定はDX11かつMegaLights無効)。

  - `BistroExteriorNight.kscene` — Bistro屋外の夜景。`Tools/extract_bistro_lights.py`が
    `Exterior.kmodel`/`.kgeom`を直接読み、街灯・ストリングライトの電球・庇のスポット・
    壁付けランタン・スクーターのヘッドライトの**実際の器具位置**から灯を導出します。
    位置・色・光源半径はすべて実測値で、目分量の数値は入っていません。
    **灯は器具の重心ではなく、そこから真下へ下ろした位置に置かれます** — MegaLightsの
    影レイは`RAY_FLAG_FORCE_OPAQUE`なので、重心へ置くと器具自身のガラスと笠に遮られて
    1灯も光りません(絵が暗いだけで例外もログも出ないため、MegaLightsの不具合と誤診しやすい)。
    スクリプトは灯ごとに脱出率を測り、しきい値を超える位置まで下ろしてから採用します
  - `MegaLightsNoiseCheck.kscene` — ノイズ測定用(`docs/ImplementationDetail.md` 61.7f/61.7g が
    使っているシーン)。`BistroInteriorLit` から時刻0・GIVolume無し・露出2.0固定にしたもので、
    **測定を決定的にするために `-autoexposure 0` と組で使います**
  - `MegaLightsStage.kscene` — 日常の切り分け用の軽いステージ。
    `Tools/generate_megalights_stage.py`が2層の回廊・アーチ・手すりの縦桟・中庭の箱・
    粗さの帯を持つglTFを生成し、灯は`KHR_lights_punctual`として埋め込みます
    (`.kscene`側に`[Light]`は書きません)。従来の`LightScale`系が「床と壁と球4個」で
    **影を落とす相手をほとんど持たなかった**のに対し、遮蔽を濃くしてあります

  ```
  python Tools\generate_megalights_stage.py
  Tools\KurenaiPacker\Build\Bin\x64\Release\KurenaiPacker.exe ^
    Assets\Source\MegaLightsStage\MegaLightsStage.gltf ^
    -o Assets\Packed\MegaLightsStage\MegaLightsStage.kmodel
  python Tools\extract_bistro_lights.py
  ```
- `Assets/Packed/` — 上記をKurenaiPacker.exeで変換した`.kmodel`/`.kgeom`/`.ktex`と、検証済みの`.kscene`
- `Assets/Packed/Skybox/` — 背景表示・IBLの入力となるHDR空キューブマップ(DDS形式、R16G16B16A16_Float、既に圧縮済みのためパッカーを通さず直接ここへ出力する)。`Tools/generate_sky_cubemap.py`(要`pip install numpy`)で再生成できる。既定では空をGPUで手続き生成するため通常は使われず、Procedural Skyを無効にしたときのフォールバックと、`[Scene]Skybox`を明示するシーン向けのアセットとして残っている

`.kmodel`/`.ktex`は元ファイルのタイムスタンプを見て自動生成・自動更新されることはありません
(実行時のディスクキャッシュではなく、KurenaiPacker.exeが生成する配布可能なアセットのため)。
ソースモデルを更新した場合は、KurenaiPacker.exeを再実行してください(`--force`を付けない限り、
既存の`.ktex`はスキップして高速に再パックします)。
