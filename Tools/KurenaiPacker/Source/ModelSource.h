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

        // 解決済みのフルパス(存在確認まで済んでいるとは限らない)。空 = 指定なし。
        // sRGBの要否はスロットで決まる(BaseColor=true、Normal/MetallicRoughness=false)ため
        // ここでは保持しない
        std::wstring BaseColorPath;
        std::wstring NormalPath;
        std::wstring MetallicRoughnessPath;
    };

    struct SourceModel
    {
        std::vector<SourceMesh> Meshes;
        float BoundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float BoundsMax[3] = { 0.0f, 0.0f, 0.0f };
    };

    // モデルファイルをassimpで解析する。失敗時はstd::runtime_errorを投げる
    SourceModel LoadSourceModel(const std::wstring& filePath);
}
