#pragma once

#include <memory>
#include <string>
#include <vector>

#include "RHI/IRHIBuffer.h"
#include "RHI/IRHITexture.h"
#include "RHI/TextureImage.h"

#include "RaytracingGeometry.h"

namespace Kurenai::Assets
{
    // 1モデル分のマテリアルテーブル(StructuredBuffer<GpuMaterial>)の1件。
    //
    // 【何のためにあるのか】従来、マテリアルはメッシュを描く直前に
    // cmd->SetTexture(t0..t3,t5,t6) と定数バッファ(ObjectConstants)で渡していた。
    // これは「1ドロー = 1マテリアル」を強制するため、メッシュが1,715個あるモデル
    // (PLATEAU LOD2の1タイル)は必ず1,715ドローになる。
    // テーブルへ載せてピクセルシェーダーが実行時の番号で引けるようにすると、
    // 1回のDispatchMeshでモデル全体を描いても材質を描き分けられる。
    //
    // 【テクスチャはbindless番号で持つ】RaytracingMaterial(RaytracingScene.h)と同じ方式。
    // IRHIDevice::RegisterBindlessが払い出した番号を入れ、シェーダーは
    // Shaders/3D/Bindless.hlsliのBindlessSampleでこれを引く。
    // bindless非対応環境ではkInvalidBindlessIndexが入り、消費側は
    // 従来のt0..t6経路へ落ちる(=このテーブル自体が作られない)。
    //
    // HLSL側の対応: Shaders/3D/GBufferCommon.hlsli の struct GpuMaterial。
    // **並びとサイズを必ず一致させること。**
    struct GpuMaterial
    {
        float BaseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float EmissiveFactor[3] = { 0.0f, 0.0f, 0.0f };
        float MetallicFactor = 0.0f;
        // 負値ならソースデータに係数が無かったことを表す(Assets::kInvalidMaterialFactor)。
        // 解釈は消費側の責任で、シェーダーは1.0(テクスチャの値をそのまま使う)として扱う
        float RoughnessFactor = 1.0f;
        // 0以下ならアルファカットアウト無効(Assets::Mesh::AlphaCutoffと同じ意味)
        float AlphaCutoff = 0.0f;
        float OcclusionStrength = 1.0f;
        float Translucency = 0.0f;
        // 以下はbindlessディスクリプタ番号。既定はRHI::kInvalidBindlessIndex(=テクスチャ無し)
        uint32_t BaseColorTextureIndex = RHI::kInvalidBindlessIndex;
        uint32_t NormalTextureIndex = RHI::kInvalidBindlessIndex;
        uint32_t MetallicRoughnessTextureIndex = RHI::kInvalidBindlessIndex;
        uint32_t EmissiveTextureIndex = RHI::kInvalidBindlessIndex;
        uint32_t OcclusionTextureIndex = RHI::kInvalidBindlessIndex;
        uint32_t BentNormalTextureIndex = RHI::kInvalidBindlessIndex;
        // kGpuMaterialFlag* の組み合わせ
        uint32_t Flags = 0;
        uint32_t Padding = 0;
    };
    static_assert(sizeof(GpuMaterial) == 80, "HLSL側のGpuMaterialと一致させるため80バイト固定");

    // GpuMaterial::Flagsのビット定義。
    // どちらも「そのマテリアルをどのパスで描くか」の判定に使う ――
    // 1ドローでモデル全体を描くようになると、パイプラインステートやドローの分割では
    // 材質ごとの出し分けができなくなるため、増幅シェーダーがこのビットで取捨する
    inline constexpr uint32_t kGpuMaterialFlagTransparent = 1u << 0;  // glTFのalphaMode=BLEND
    inline constexpr uint32_t kGpuMaterialFlagCutout = 1u << 1;       // glTFのalphaMode=MASK

    // GpuMeshlet::Flagsは材質フラグとメッシュレットLODの段を1つのuintへ詰めている。
    // 下位8bitが上のkGpuMaterialFlag*で、上位に段を置く。
    //
    // 【なぜ専用のフィールドを増やさないのか】GpuMeshletは64バイト固定で、HLSL側の
    // Meshletと1バイトも違ってはいけない。段のために5バイト目を足すと構造体が80バイトへ
    // 伸び、地形タイル1枚(25,904塊)で414KB、モデル全体では25%の増加になる。
    // 段は0〜3の2bitで足りるので、材質フラグの空きビットへ入れるほうが安い。
    //
    // 【材質の判定では必ずマスクすること】MeshletPassesMaterialFilterは
    // (flags & rejectMask)==0 で捨てるかを決める。段のビットを混ぜたまま渡しても
    // rejectMaskが下位ビットしか使っていないうちは偶然通るが、
    // マスクを1つ足した瞬間に「特定の段だけ描かれない」という形で壊れる
    inline constexpr uint32_t kGpuMaterialFlagMask = 0xFFu;
    // この塊自身が何段目か(0が原寸)
    inline constexpr uint32_t kGpuMeshletLODLevelShift = 8u;
    inline constexpr uint32_t kGpuMeshletLODLevelMask = 0x3u;

    // モデル1つ分のメッシュレット表(StructuredBuffer<Meshlet>)の1件。
    // ディスク形式のAssets::MeshletEntry(48バイト、ModelPackage.h)へ
    // 「どのメッシュの、どのマテリアルの塊か」を足したもの。
    //
    // 【なぜメッシュ単位ではなくモデル単位で持つのか】メッシュごとに別のバッファを
    // 持っていると、1回のDispatchMeshで扱えるのが1メッシュぶんに限られる。
    // モデル全体のメッシュレットを1本の表にまとめておけば、
    // 増幅シェーダーが「モデルの全メッシュレット」を1回のディスパッチで見渡せる。
    //
    // 【オフセットはモデル基準へ付け替える】MeshletEntry::VertexOffset/TriangleOffsetは
    // ディスク上ではメッシュ内相対だが、ここではモデル単位に連結した
    // MeshletVertexBuffer / MeshletTriangleBuffer の中でのオフセットに直してある。
    //
    // 【頂点バッファだけはメッシュ単位のまま】.kgeomのペイロードは
    // [頂点][インデックス][メッシュレット3本] をメッシュごとに連結した並びで、
    // 頂点ブロックが連続していない。モデル単位の頂点バッファを作るには集めて複製する
    // 必要があり、それは従来経路(DX11・BLAS・自前ラスタライザ)が使う
    // メッシュ単位の頂点バッファと二重にVRAMを食う。
    // 代わりに、メッシュレット1件ごとに「自分の頂点バッファのbindless番号」を持たせて
    // メッシュシェーダーが選ぶ(自前ラスタライザのSWRasterMeshInfoと同じ方式)。
    //
    // HLSL側の対応: Shaders/3D/GBufferMeshlet.hlsl の struct Meshlet。
    // **並びとサイズを必ず一致させること。**
    struct GpuMeshlet
    {
        uint32_t VertexOffset = 0;         // モデル単位のMeshletVertexBuffer内の要素オフセット
        uint32_t TriangleOffset = 0;       // モデル単位のMeshletTriangleBuffer内の要素オフセット
        uint32_t VertexCount = 0;
        uint32_t TriangleCount = 0;
        float BoundsCenter[3] = { 0.0f, 0.0f, 0.0f };
        float BoundsRadius = 0.0f;
        float ConeAxis[3] = { 0.0f, 0.0f, 0.0f };
        float ConeCutoff = 1.0f;
        // この塊が属するメッシュの頂点バッファのbindless番号
        uint32_t VertexBufferIndex = RHI::kInvalidBindlessIndex;
        // Model::MaterialTableBuffer 内の番号(= Mesh::MaterialIndex)
        uint32_t MaterialIndex = 0;
        // kGpuMaterialFlag* の写し。増幅シェーダーがパスごとの取捨に使う
        // (1ドローでモデル全体を描くと、ドローの分割で材質を出し分けられなくなるため)
        uint32_t Flags = 0;
        // このメッシュ内で何番目の塊か。**モデル内の通し番号ではない。**
        // メッシュレットの色分け表示(Meshlet.hlsliのMeshletDebugColor)にだけ使う値で、
        // レイトレーシング側(RaytracingScene.hlsliのRTFindMeshlet)がメッシュ内の番号を
        // 返すのに合わせてある。揃えないと同じ塊が別の色になり、
        // 「ラスタとRTが同じジオメトリを見ているか」の確認が成立しない
        uint32_t MeshletIndexInMesh = 0;
    };
    static_assert(sizeof(GpuMeshlet) == 64, "HLSL側のMeshletと一致させるため64バイト固定");

    struct Mesh
    {
        std::unique_ptr<RHI::IRHIBuffer> VertexBuffer;
        std::unique_ptr<RHI::IRHIBuffer> IndexBuffer;
        uint32_t IndexCount = 0;
        // VertexBufferに入っている頂点数。描画はインデックス経由なので不要だが、
        // BLASの構築(D3D12_RAYTRACING_GEOMETRY_DESC::Triangles.VertexCount)に必要
        uint32_t VertexCount = 0;
        // レイトレーシング用: Model::RaytracingAttributes / RaytracingIndices の中で
        // このメッシュのデータが始まる位置。RaytracingSceneがシーン全体の統合バッファを
        // 組み立てる際の連結元として使う。
        // デバイスがレイトレーシング非対応の場合は両配列とも空で、この値は意味を持たない
        uint32_t RaytracingAttributeOffset = 0;
        uint32_t RaytracingIndexOffset = 0;
        // 同じくModel::RaytracingMeshletTriangleOffsets内でこのメッシュのデータが始まる位置。
        // 要素数はMeshletCount(メッシュレットを持たない.kmodelでは0)
        uint32_t RaytracingMeshletOffset = 0;

        // --- メッシュレット(メッシュシェーダー用) ---------------------------------------
        //
        // 【GPUバッファはModel側にある】メッシュレットの表と2段の間接参照テーブルは
        // モデル単位で1本ずつ持つ(Model::MeshletBuffer ほか)。ここにあるのは
        // 「そのモデル単位の表の、どこからいくつがこのメッシュのぶんか」だけ。
        //
        // 【空になる条件】(1) デバイスがメッシュシェーダー非対応、(2) .kmodelが
        // --no-meshletsで焼かれている、のいずれか。
        // 描画側はMeshletCountではなくModel::MeshletBufferの有無で経路を選ぶこと ――
        // MeshletCountはアセットが持つメッシュレット数そのもので、メッシュシェーダー
        // 非対応の環境でも(レイトレーシング側が使うため)0にはならない
        //
        // Model::MeshletBuffer内でこのメッシュのメッシュレットが始まる位置
        uint32_t MeshletOffset = 0;
        // .kmodelが持つメッシュレット数。GPUバッファの有無とは独立。
        //
        // 【LOD0の個数であって全段の合計ではない】レイトレーシング(RaytracingMeshletOffsetが
        // 指す三角形オフセット表)と従来の頂点シェーダー経路は、.kgeomのインデックスブロック
        // ―― すなわちLOD0の三角形 ―― しか見ない。全段の合計を入れると三角形番号の対応が崩れる
        uint32_t MeshletCount = 0;
        // 全段を合わせたメッシュレット数。Model::MeshletBufferにはこの数だけ載っている。
        // 増幅シェーダーが段を選ぶには、選ばれうる段すべてを走査範囲に入れる必要がある
        uint32_t MeshletTotalCount = 0;
        // 焼かれている段の数(1〜kMaxMeshletLODCount)。1なら段の選択は何もしないのと同じ
        uint32_t MeshletLODCount = 0;

        // このメッシュのモデルローカル空間でのAABB。.kmodel v10のMeshEntryが持つ値をそのまま入れる。
        //
        // 【なぜモデル単位のAABBでは足りないか】1つのモデルが街区全体を覆うことがある
        // (Bistro Exteriorは132メッシュで1インスタンス、AABBは109x32x115m)。
        // カメラがそのAABBの内側に入ると、モデル単位の距離は全メッシュで0になり、
        // 「どのテクスチャも最大解像度が要る」としか言えなくなる。PLATEAUのLOD2タイル
        // (1.1km四方)で街路に降りたときも同じことが起きる。
        // テクスチャストリーミングが距離を測るにはメッシュ単位の広がりが要る。
        //
        // 【メッシュ単位のフラスタムカリングも同じ値を使う】モデル単位の判定を通ったあとに
        // もう一段間引くための材料でもある。判定に使うのはこれをWorldで変換した
        // ModelInstance::MeshWorldBoundsList のほうで、Modelは複数のインスタンスから
        // 共有されうるためワールド空間の値をここに持たせてはいけない
        float BoundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float BoundsMax[3] = { 0.0f, 0.0f, 0.0f };

        // このメッシュのUVが「モデルローカル1メートルあたり何UV単位に相当するか」。
        // W×Hのテクスチャを貼ったときのテクセル密度は UVPerLocalMeter * sqrt(W*H) [texels/m] になる。
        //
        // 【テクスチャの寸法を掛けずにUV空間の値で持つ理由】1つのメッシュはベースカラー・
        // 法線・メタリックラフネスと寸法の違うテクスチャを同時に参照しうる。UV密度は
        // メッシュの性質、テクセル密度はテクスチャとの組み合わせの性質なので、前者だけを持つ。
        //
        // テクスチャストリーミングが「距離いくつなら何段目のミップで足りるか」を
        // CPUで見積もるために使う(Sampler Feedbackを使わない理由はdocs/ImplementationDetail.md)。
        // ModelLoaderが読み込み時に三角形を最大64個サンプリングして10パーセンタイルを取る
        // (最も引き伸ばされている領域が常駐段を縛るため、低い側を採る)。
        // 求められなかった場合(UVが無い・縮退している)は0で、その場合は
        // 常駐ミップを削らない(見積もれないものを削ると静かにぼける)
        float UVPerLocalMeter = 0.0f;
        RHI::IRHITexture* BaseColorTexture = nullptr;
        RHI::IRHITexture* NormalTexture = nullptr;
        RHI::IRHITexture* MetallicRoughnessTexture = nullptr;
        RHI::IRHITexture* EmissiveTexture = nullptr;
        // ベイク済みアンビエントオクルージョン(遮蔽マップ)。glTFのocclusionTexture由来で、
        // 赤チャンネルを遮蔽率(1=遮蔽なし、0=完全遮蔽)として読む。未指定時は白1x1(=遮蔽なし)。
        // SSAO/SSILがスクリーンスペースの制約で拾えない、布の折り目や狭い隙間のような
        // 細部の遮蔽を補う。間接光(IBL・SSILの間接拡散光)にのみ効き、直接光と自発光には掛からない
        RHI::IRHITexture* OcclusionTexture = nullptr;
        // bent normal(正規化しない可視方向の平均、RGBA16F)。ライトマップUV空間で、
        // .rgb = bRaw、.a = 有効フラグ。未指定時は黒1x1(=.a が0 = データ無し)。
        //
        // 遮蔽マップが「どれだけ隠れているか」しか持たないのに対し、こちらは
        // 「どの方向が開いているか」を持つ。消費側が軸 normalize(bRaw)・aoB = length(bRaw)・
        // aoN = dot(N, bRaw) の3つへ分解し、スペキュラ遮蔽の方向依存(壁を向いた反射だけを
        // 暗くする)とディフューズの方向バイアスを扱う(34章)
        RHI::IRHITexture* BentNormalTexture = nullptr;
        float MetallicFactor = 0.0f;
        // ソースデータに値が無い場合はkInvalidMaterialFactor(負値)が入る。シェーダー側は
        // 負値を「係数の指定なし」とみなし1.0(テクスチャの値をそのまま使う)として扱う
        float RoughnessFactor = 0.0f;
        float EmissiveFactor[3] = { 0.0f, 0.0f, 0.0f };
        // 0以下ならアルファカットアウト無効(常に不透明)。glTFのalphaMode=MASKのマテリアルのみ
        // alphaCutoff(既定0.5)が設定される
        float AlphaCutoff = 0.0f;
        // 透過率(0=不透明、1=完全に透ける)。葉や花弁のように薄いものが、裏から当たった光を
        // 透かして表側が明るく見える量。0なら従来どおりの不透明な陰影になる。
        // KurenaiPackerの --translucent <マテリアル名>=<値> で設定する(45章)
        float Translucency = 0.0f;
        // glTFのalphaMode=BLENDのマテリアルのみtrue。GBufferパス(不透明)には描画されず、
        // 専用のTransparentパス(KurenaiEngine3D::Render参照)でカメラから遠い順にアルファブレンド
        // 合成される。AlphaCutoffとは排他(glTF仕様上alphaModeはOPAQUE/MASK/BLENDのいずれか1つ)
        bool IsTransparent = false;
        // glTFのpbrMetallicRoughness.baseColorFactor(RGBA、既定[1,1,1,1])。BaseColorTextureと
        // 乗算して使う。テクスチャを持たずbaseColorFactorのみで色/不透明度を表現するマテリアル
        // (ガラス等でよくあるパターン)は、これが無いとBaseColorTexture=nullptr時の白1x1
        // プレースホルダー(alpha=1)にフォールバックし、意図した色・アルファと異なる見た目になる。
        // 現状このフォールバック乗算はTransparentパス(Transparent.hlsl)のみが行い、GBufferパス
        // (不透明・MASK)は既存動作を変えないため引き続きテクスチャの色をそのまま使う
        float BaseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        // glTFのocclusionTexture.strength(既定1.0)。シェーダーはlerp(1, ao, strength)で適用する。
        // 既定値はModelPackage.hのkDefaultOcclusionStrengthと同じ1.0だが、このヘッダーは
        // ディスク形式の定義に依存させたくないため定数を参照せず直接書いている
        float OcclusionStrength = 1.0f;
        // Model::MaterialTableBuffer の中でこのメッシュが使うマテリアルの番号。
        //
        // 【現状はメッシュと1対1】.kmodel v9はマテリアルテーブルを持たず、マテリアルの
        // 係数とテクスチャ番号をMeshEntryが直接持っている(=メッシュ1つにマテリアル1つ)。
        // そのためModelLoaderはメッシュ1つにつき1件のGpuMaterialを作り、その番号を入れる。
        // .kmodelがマテリアルテーブルを持つようになれば、ここへその番号がそのまま入る
        // (消費側とシェーダーは書き換え不要)。
        //
        // テーブルを作らなかった場合(bindless非対応)は0のまま。そのときはテーブル自体が
        // 存在せず、描画は従来のt0..t6経路を通るのでこの値は読まれない
        uint32_t MaterialIndex = 0;
    };

    enum class LightType : uint32_t
    {
        Directional = 0,
        Point       = 1,
        Spot        = 2,
        // 3以降はエリアライト(球/チューブ/矩形)用に予約。今回は未実装
    };

    struct Light
    {
        LightType Type = LightType::Point;
        float Position[3]{ 0.0f, 0.0f, 0.0f };
        float Direction[3]{ 0.0f, -1.0f, 0.0f };  // 光が進む向き(正規化済み)。Spot/Directional で使用
        float Color[3]{ 1.0f, 1.0f, 1.0f };       // 線形色。最大成分が1になるよう正規化して保持する
        // 測光量。Point/Spot はカンデラ(cd = lm/sr)、Directional はルクス(lx = lm/m²)。
        // glTF は KHR_lights_punctual の値をそのまま格納する(物理単位として正確)。
        // FBX は物理単位を持たないため、DCC側のIntensity/100をカンデラ相当として近似する(ModelLoader参照)
        float Intensity = 1.0f;
        float Range = 10.0f;                      // 影響半径。Directional では未使用
        float SpotInnerConeAngle = 0.4f;          // ラジアン(軸からの半角)。Spot のみ
        float SpotOuterConeAngle = 0.6f;
        bool Enabled = true;
        // このライトがスクリーンスペースシャドウ(ScreenSpaceShadow.hlsli)を落とすか。
        // ピクセルあたりのシャドウレイ数には上限があるため、影を出したいライトへ予算を回せるよう
        // ライト単位で切れるようにしてある。
        //
        // 【永続化しない】このフラグは .kmodelcache の CachedLightRecord(POD)には含めない。
        // 含めるとレコードのレイアウトが変わり kPackageVersion の更新と全アセットの再パックが
        // 必要になるが、得られるのは「モデルファイルに埋め込まれたライトごとの既定値」だけで
        // 見合わない。モデル由来のライトは常にこの既定値(true)で読み込まれ、
        // .kscene の [Light] セクション(テキスト形式)と ImGui で上書きする運用とする
        bool CastShadow = true;
        std::string Name;                         // aiLight::mName 由来。ImGui 一覧の表示に使う
    };

    struct Model
    {
        std::vector<Mesh> Meshes;
        std::vector<std::unique_ptr<RHI::IRHITexture>> Textures;
        // Texturesと同じ並びの.ktexへの絶対パス。テクスチャストリーミングが
        // 常駐ミップを変えるときに「どのファイルから読み直すか」を知るために要る。
        // 読み込みに失敗してプレースホルダー(白/フラット法線)へ落ちたテクスチャは
        // そもそもTexturesへ入らないため、両者の要素数は常に一致する
        std::vector<std::wstring> TexturePaths;
        // TexturePathsと同じ並びの.ktexヘッダ情報(解像度・ミップ段数・部分読み出しの可否)。
        //
        // 【読み込みスレッドで取っておく理由】常駐ミップ制御の追跡表への登録
        // (TextureStreamingManager::AttachModel)はRenderスレッドで走る。そこでヘッダを
        // 読みに行くと、モデルが1つ常駐するたびにテクスチャの枚数だけファイルを開くことになり
        // (PLATEAUのLOD2タイルで数十枚)、街を流している間じゅう描画が止まる
        std::vector<RHI::PackedTextureInfo> TextureInfos;
        std::vector<Light> Lights;

        // --- マテリアルテーブル(bindless経路用) -------------------------------------------
        //
        // このモデルの全マテリアル(GpuMaterial)を並べたStructuredImmutableバッファ。
        // Mesh::MaterialIndexが添字で、ピクセルシェーダーが実行時の番号で引く。
        //
        // 【空になる条件】デバイスがbindless非対応。そのときは従来どおり
        // メッシュを描く直前にSetTexture(t0..t6)で差し替える経路だけが動く。
        // 描画側はこのポインタの有無で経路を選ぶこと
        std::unique_ptr<RHI::IRHIBuffer> MaterialTableBuffer;
        uint32_t MaterialCount = 0;
        // このモデルにアルファカットアウト(glTFのalphaMode=MASK)のマテリアルが1つでもあるか。
        // 深度プリパスとシャドウは「ピクセルシェーダーを持たない速い経路」を使いたいので、
        // カットアウトを含むモデルだけを2ドロー(不透明ぶんとカットアウトぶん)に分ける。
        // 含まないモデル(PLATEAU LOD2がそう)は1ドローのままで済む
        bool HasCutoutMaterial = false;

        // --- メッシュレット(メッシュシェーダー用) -----------------------------------------
        //
        // KurenaiPackerが焼いた分割情報(Assets::MeshletEntry)を、モデルの全メッシュぶん
        // 1本へ連結したもの(Assets::GpuMeshlet)と、その2段の間接参照テーブル。
        // 増幅シェーダーがバウンディング球・法線コーンでカリングし、
        // メッシュシェーダーが生き残った塊の頂点/三角形を組み立てる。
        //
        // 3本ともBufferUsage::StructuredImmutable。シーン読み込み時に一度書いたら
        // 変わらないため、ステージングリングを持たないこのUsageがそのまま当てはまる。
        //
        // 【なぜメッシュ単位ではないのか】GpuMeshletのコメント参照。
        // メッシュごとに分かれていると、1回のDispatchMeshで1メッシュしか描けない
        std::unique_ptr<RHI::IRHIBuffer> MeshletBuffer;
        std::unique_ptr<RHI::IRHIBuffer> MeshletVertexBuffer;
        std::unique_ptr<RHI::IRHIBuffer> MeshletTriangleBuffer;
        // MeshletBufferの要素数(モデルの全メッシュのメッシュレット数の総和)
        uint32_t TotalMeshletCount = 0;
        // このモデルが選べる最も粗い段。**全メッシュが持っている段のうち最小のもの**
        // (= min over メッシュ (MeshletLODCount - 1))。
        //
        // 【なぜモデル単位で1つに畳むのか】段の数はメッシュごとに違う ――
        // 潰せる辺を持たないメッシュはLOD0しか焼かれない(MeshletBuilder.cpp)。
        // メッシュごとに min(選んだ段, そのメッシュの最も粗い段) で読み替えると、
        // **1つのモデルの中で段が混ざる**。混ざると、簡略化で頂点が動いた側と
        // 動いていない側で辺が一致せず境目に穴が開く(材質の境目でメッシュが
        // 分かれているモデルは、その境目で実際に辺を共有している)。
        //
        // ここで全メッシュの共通部分まで落としておけば、増幅シェーダーが選んだ段は
        // 必ずどのメッシュにも存在し、モデル全体が同じ段で描かれる。
        // 段を1つしか持たないメッシュが1つでもあれば、そのモデルは常に原寸になる
        // ―― 保守的だが、穴が開かないことのほうを優先する
        uint32_t MeshletLODLevelCap = 0;
        // LOD0の三角形数の合計(= 各メッシュのIndexCountの総和 / 3)。
        //
        // メッシュレットLODのしきい値をモデルごとに決めるために使う。
        // 「原寸の三角形1つが画面上で1画素を切ったら段を落とす」を基準にすると、
        // 直径D画素の円の中にN個の三角形があるとき平均面積は (πD²/4)/N なので、
        // 1画素を切る直径は D = sqrt(4N/π)。三角形数はモデルによって3桁違う
        // (小道具の数千とPLATEAUの地形タイルの134万)ため、
        // 単一の画素数を全モデルへ当てはめると必ずどちらかが破綻する
        uint32_t TotalTriangleCount = 0;
        // このモデルの**すべての**メッシュがメッシュレットを持っているか。
        //
        // 【1モデル1ドローの前提条件】1回のDispatchMeshで描けるのはメッシュレットの表に
        // 載っているものだけ。1つでも塊を持たないメッシュがあると、そのメッシュだけが
        // 描かれずに消える。混在させて別途DrawIndexedで補うこともできるが、
        // 経路が2つ走ることで深度や丸めの食い違いを持ち込むより、
        // モデル単位で従来経路へ落とすほうが切り分けやすい
        bool AllMeshesHaveMeshlets = false;

        // レイトレーシングでヒット面の陰影を計算するための、このモデル全メッシュ分の
        // 頂点属性とインデックス(Mesh::RaytracingAttributeOffset / RaytracingIndexOffsetが
        // メッシュごとの開始位置を指す)。
        //
        // 描画用の頂点/インデックスバッファはGPU上にしか無く、シェーダーからは
        // 頂点バッファとして以外に読めない(SRVを持たない)ため、レイトレーシング側が
        // 参照できる形のコピーをModelLoaderが.kgeomの読み込み中に作る。
        //
        // 【寿命】RaytracingScene::Buildがシーン全体の統合バッファへ連結してGPUへ送った時点で
        // 用済みになるため、そこで解放される(数百万頂点のシーンでは100MB規模になるため、
        // 使い終わったCPU側コピーを抱え続けない)。
        // デバイスがレイトレーシング非対応の場合はそもそも構築されず空のまま
        std::vector<RaytracingVertexAttribute> RaytracingAttributes;
        std::vector<uint32_t> RaytracingIndices;
        // メッシュレットごとの「メッシュ内での開始三角形番号」(Mesh::RaytracingMeshletOffsetが
        // メッシュごとの開始位置を指す)。上の2つと同じくRaytracingScene::Buildが
        // シーン全体の統合バッファへ連結した時点で解放される。
        // 用途と、MeshletEntryをそのまま持たない理由はRaytracingScene::GetMeshletTriangleOffsetBuffer参照
        std::vector<uint32_t> RaytracingMeshletTriangleOffsets;

        float BoundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float BoundsMax[3] = { 0.0f, 0.0f, 0.0f };
    };
}
