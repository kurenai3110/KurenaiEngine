#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Assets/ModelPackage.h"
#include "Assets/Vertex.h"

// assimpによるモデルファイル(glTF/FBX/OBJ等)の解析。GPUデバイスに一切依存しないため
// KurenaiEngine.dll(ランタイム)には持たず、オフラインのKurenaiPacker.exeだけが
// assimp/zlibにリンクする。接線の自前平均化・マテリアル単位のメッシュ結合など、
// 実測に基づく判断はここに集約する。
namespace KurenaiPacker
{
    // FBX等に埋め込まれたテクスチャ(aiScene::mTextures)を一時ファイルへ取り出して保持する。
    // デストラクタで一時ディレクトリごと消す。
    //
    // 【なぜ一時ファイルなのか】RHI::TextureImageにはメモリから読む経路が無く
    // (LoadFromFileとLoadFromPackedTextureの2つだけ)、PackageWriterの重複排除・.ktexの
    // ミラー・ワーカースレッドへの分配もすべて「解決済みのファイルパス」を前提に組まれている。
    // バイト列を持ち回る経路を新設するとRHI・PackageWriterの両方へ手が入るのに対し、
    // 一時ファイルへ落として既存の経路へ合流させれば変更が解決の1点で済む。
    //
    // 【なぜ番号を含む決定的な名前なのか】PackageWriterの重複排除キーは
    // 「解決済みフルパス|sRGB」なので、同じ埋め込みテクスチャを複数のマテリアルが参照したとき、
    // 取り出し先のパスが一致していなければ同じ画像を何度もBC7圧縮することになる。
    // aiSceneのテクスチャ配列番号を名前に含め、一度取り出したものは同じパスを返す
    class EmbeddedTextureStore
    {
    public:
        EmbeddedTextureStore() = default;
        ~EmbeddedTextureStore();

        EmbeddedTextureStore(const EmbeddedTextureStore&) = delete;
        EmbeddedTextureStore& operator=(const EmbeddedTextureStore&) = delete;

        // 取り出し先の一時ディレクトリ(末尾は区切り文字)。まだ何も取り出していなければ空
        const std::wstring& Directory() const { return m_Directory; }

        // 取り出し済みの枚数(同じテクスチャを何度参照しても1と数える)
        size_t ExtractedCount() const { return m_Extracted.size(); }

        // 圧縮済みブロブ(JPEG/PNG/TIFF等)をそのまま書き出す。formatHintはaiTexture::achFormatHint。
        // 取り出せなければ空文字列を返す(呼び出し側は従来どおりの「見つからない」扱いへ落ちる)
        std::wstring StoreCompressed(
            unsigned int textureIndex,
            const std::string& formatHint,
            const std::string& originalName,
            const void* data,
            size_t sizeInBytes);

        // 非圧縮のARGB8888(aiTexture::mHeight != 0)をPNGとして書き出す。
        // PNGにするのは、DDS/TGAで書くとTextureImage::LoadFromFileが「圧縮・ミップ済みの
        // 配布形式」とみなしてミップ生成もBC7圧縮も行わないため(他のテクスチャと扱いが揃わない)
        std::wstring StoreUncompressed(
            unsigned int textureIndex,
            const std::string& originalName,
            const void* bgraTexels,
            unsigned int width,
            unsigned int height);

    private:
        // 一時ディレクトリを必要になった時点で作る。失敗したら空を返す
        bool EnsureDirectory();

        std::wstring m_Directory;
        std::unordered_map<unsigned int, std::wstring> m_Extracted;
    };

    struct SourceMesh
    {
        std::vector<Kurenai::Assets::Vertex> Vertices;
        std::vector<uint32_t> Indices;
        float MetallicFactor = 0.0f;
        // ソースデータがラフネスを持たない場合は、もっともらしい既定値を勝手に埋めず
        // Kurenai::Assets::kInvalidMaterialFactor(負値)を設定する
        float RoughnessFactor = 0.0f;
        // 0以下ならアルファカットアウト無効(常に不透明)。glTFのalphaMode=MASKのマテリアルのみ
        // alphaCutoff(既定0.5)が設定される
        float AlphaCutoff = 0.0f;
        // 透過率(0=不透明、1=完全に透ける)。葉や花弁のように薄いものが、裏から当たった光を
        // 透かして表側が明るく見える量。glTFにこれを表す標準のプロパティが無く(既存の
        // 拡張はガラス向けで、Blender 2.82のエクスポータも書き出さない)、
        // KurenaiPackerの --translucent <マテリアル名>=<値> で外から与える
        float Translucency = 0.0f;
        // glTFのalphaMode=BLENDのマテリアルのみtrue。AlphaCutoffとは排他(alphaModeはOPAQUE/MASK/BLENDの
        // いずれか1つ)
        bool IsTransparent = false;
        float EmissiveFactor[3] = { 0.0f, 0.0f, 0.0f };
        // glTFのpbrMetallicRoughness.baseColorFactor(RGBA、既定[1,1,1,1])。BaseColorTextureが
        // 無いマテリアル(色/不透明度をbaseColorFactorのみで表現するガラス等)を正しく再現するため
        float BaseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        // glTFのocclusionTexture.strength。ソースが値を持たない場合はglTF仕様の既定値1.0
        float OcclusionStrength = Kurenai::Assets::kDefaultOcclusionStrength;

        // 解決済みのフルパス(存在確認まで済んでいるとは限らない)。空 = 指定なし。
        // sRGBの要否はスロットで決まる(BaseColor/Emissive=true、Normal/MetallicRoughness/
        // Occlusion=false)ためここでは保持しない
        std::wstring BaseColorPath;
        std::wstring NormalPath;
        std::wstring MetallicRoughnessPath;
        std::wstring EmissivePath;
        std::wstring OcclusionPath;
    };

    enum class SourceLightType : uint32_t
    {
        Directional = 0,
        Point       = 1,
        Spot        = 2,
    };

    // モデルファイル埋め込みのライト(glTFのKHR_lights_punctual拡張やFBXのライトノード由来)。
    // Assets::LightEntry(ModelPackage.h)のPOD部分と1対1対応する
    struct SourceLight
    {
        SourceLightType Type = SourceLightType::Point;
        float Position[3] = { 0.0f, 0.0f, 0.0f };
        float Direction[3] = { 0.0f, -1.0f, 0.0f };
        float Color[3] = { 1.0f, 1.0f, 1.0f };
        float Intensity = 1.0f;
        float Range = 10.0f;
        float SpotInnerConeAngle = 0.4f;
        float SpotOuterConeAngle = 0.6f;
        bool Enabled = true;
        std::string Name;
    };

    struct SourceModel
    {
        std::vector<SourceMesh> Meshes;
        std::vector<SourceLight> Lights;
        float BoundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float BoundsMax[3] = { 0.0f, 0.0f, 0.0f };

        // 埋め込みテクスチャの取り出し先。**SourceModelが生きている間は消えない**ことが重要で、
        // WriteModelPackageがワーカースレッドから実ファイルとして読むため、書き出しが終わるまで
        // 保持し続ける必要がある。埋め込みが1枚も無ければnullptr
        std::shared_ptr<EmbeddedTextureStore> EmbeddedTextures;
    };

    // 解析後に全マテリアルへ強制的に適用する係数の上書き。
    //
    // 生のOBJ(3Dスキャン配布物など)はPBRのマテリアル係数を表現できない
    // ―― WavefrontMTLのPBR拡張(Pm/Pr)をassimpはテクスチャ指定としてしか読まないため、
    // メタリック値をファイル側から与える手段が無い。検証用にそうしたモデルへ
    // 「リフレクタンス=1(baseColor=1かつmetallic=1、F0=1の完全反射)」のような
    // マテリアルを与えられるようにする。std::nulloptなら上書きしない
    struct MaterialOverride
    {
        std::optional<float> MetallicFactor;
        std::optional<float> RoughnessFactor;
        std::optional<std::array<float, 3>> BaseColor;
        // マテリアル名 → 透過率。名前が一致したマテリアルにだけ透過率を与える
        // (メタリック等の「全マテリアルへ一律」とは違い、木の中でも花弁だけを
        //  透けさせたい、という使い方になるため)
        std::map<std::string, float> Translucency;
        // マテリアル名 → アルファカットアウトのしきい値。
        // FBX/OBJにはglTFのalphaMode(OPAQUE/MASK/BLEND)に相当する情報が無いため、
        // 「BaseColorのアルファで抜く」前提で作られた葉や草のマテリアルであっても
        // 解析だけでは判別できず、不透明な板として描かれてしまう。外から名前で指定する
        std::map<std::string, float> AlphaCutoff;
        // aiTextureType_SPECULARに入っているテクスチャをmetallicRoughnessとして読むか。
        //
        // FBXのSpecularColorスロットへORM(R=遮蔽/G=ラフネス/B=メタリック)を格納する
        // 規約のアセットがある(NVIDIA Emerald Squareがこれで、README.txtに明記されている)。
        // assimpはFBXのSpecularColorをaiTextureType_SPECULARへ入れるため、既定の
        // DIFFUSE_ROUGHNESS/METALNESSしか見ない解決では1枚も拾えない。
        //
        // 【無条件に見てはいけない】SpecularColorが本来の「鏡面反射色」であるアセット
        // (Specular/Glossinessワークフロー)では、色マップがそのままメタリック/ラフネスとして
        // 読まれ、全面が金属になる。既存アセットを再パックしたときに静かに壊れるため、
        // 指定したときだけ有効にする
        bool SpecularAsOrm = false;
    };

    // モデルファイルをassimpで解析する。失敗時はstd::runtime_errorを投げる。
    // scale: 頂点位置・バウンズに乗算する係数(既定1.0)。OBJ等、ファイル自体に単位情報を
    // 持たない形式では、センチメートル単位で作成されたアセットを
    // そのまま読み込むと本来の100倍のスケールになってしまうことがあるため、呼び出し側
    // (KurenaiPacker.exeの--scaleオプション)が既知の単位変換係数を明示的に渡す
    // originOffset: 頂点位置とバウンズから引く座標(ソースの単位のまま、scaleを掛ける前)。
    //
    // 【なぜ「自動で中心へ寄せる」ではなく明示指定なのか】地理座標系のように、原点が
    // 遠く離れた絶対座標でモデルが作られていることがある(Project PLATEAUのFBXは
    // 平面直角座標系の絶対値で、原点から数十km離れている)。頂点がfloat32であるうえ、
    // .kmodelのAABBがそのまま巨大になり、シーンAABBの対角から自動決定される遠クリップ面
    // (farZ = max(100, 対角x4))が桁で狂う。
    //
    // このときタイルごとに「自分のAABBの中心」で寄せてしまうと、タイル同士の相対的な
    // 位置関係が壊れて街が崩れる。**複数のモデルで同じ値を引く**必要があるため、
    // 自動ではなく呼び出し側が明示する形にしている
    SourceModel LoadSourceModel(
        const std::wstring& filePath,
        float scale = 1.0f,
        const MaterialOverride& materialOverride = {},
        const std::optional<std::array<float, 3>>& originOffset = std::nullopt);

    // assimpが読んだ直後のシーン構造を標準出力へ印字する(パッケージは書き出さない)。
    // 失敗時はLoadSourceModelと同じくstd::runtime_errorを投げる。
    //
    // 外部から持ち込んだモデルは、単位系(FBXのUnitScaleFactor)・上方向軸・テクスチャが
    // どのスロットへ入るか・ノード数の規模が事前に分からない。これらは「assimpに読ませれば
    // 全部分かる」ものだが、LoadSourceModel + WriteModelPackage を通すとテクスチャ変換と
    // メッシュレット構築と数GBの書き出しまで走ってしまい、「スケールを知りたいだけ」の
    // 一度の確認に毎回それを払うことになる(147MBのFBXで実測して判断するには非現実的で、
    // 試行のたびに出力先も数GB埋まる)。読み込んだ直後で止める経路を分けておく。
    //
    // scale: 印字するバウンディングボックスへ乗算する係数。--scaleに与える値の妥当性を
    // 「既知の寸法(車両の全長、建物の高さ)と合うか」で判定するためのもの
    void InspectModel(const std::wstring& filePath, float scale = 1.0f);
}
