#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
        float RoughnessFactor = 0.7f;
        // 0以下ならアルファカットアウト無効(常に不透明)。glTFのalphaMode=MASKのマテリアルのみ
        // alphaCutoff(既定0.5)が設定される
        float AlphaCutoff = 0.0f;
        float EmissiveFactor[3] = { 0.0f, 0.0f, 0.0f };

        // 解決済みのフルパス(存在確認まで済んでいるとは限らない)。空 = 指定なし。
        // sRGBの要否はスロットで決まる(BaseColor/Emissive=true、Normal/MetallicRoughness=false)ため
        // ここでは保持しない
        std::wstring BaseColorPath;
        std::wstring NormalPath;
        std::wstring MetallicRoughnessPath;
        std::wstring EmissivePath;
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

    // モデルファイルをassimpで解析する。失敗時はstd::runtime_errorを投げる
    SourceModel LoadSourceModel(const std::wstring& filePath);
}
