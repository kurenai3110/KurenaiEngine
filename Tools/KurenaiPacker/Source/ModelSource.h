#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Assets/ModelPackage.h"
#include "Assets/Vertex.h"

// assimpによるモデルファイル(glTF/FBX/OBJ等)の解析。GPUデバイスに一切依存しないため
// KurenaiEngine.dll(ランタイム)には持たず、オフラインのKurenaiPacker.exeだけが
// assimp/zlibにリンクする。ロジックはj:\Claude\KurenaiEngine\KurenaiEngine\Source\Library\
// Assets\ModelLoader.cppに元々あったassimp解析部分をそのまま移設したもので、
// 実測に基づく判断(接線の自前平均化、マテリアル単位のメッシュ結合など)を変更していない。
namespace KurenaiPacker
{
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
    };

    // モデルファイルをassimpで解析する。失敗時はstd::runtime_errorを投げる。
    // scale: 頂点位置・バウンズに乗算する係数(既定1.0)。OBJ等、ファイル自体に単位情報を
    // 持たない形式では、センチメートル単位で作成されたアセットを
    // そのまま読み込むと本来の100倍のスケールになってしまうことがあるため、呼び出し側
    // (KurenaiPacker.exeの--scaleオプション)が既知の単位変換係数を明示的に渡す
    SourceModel LoadSourceModel(
        const std::wstring& filePath,
        float scale = 1.0f,
        const MaterialOverride& materialOverride = {});
}
