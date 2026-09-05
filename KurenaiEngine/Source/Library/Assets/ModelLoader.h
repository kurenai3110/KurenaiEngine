#pragma once

#include <memory>
#include <string>
#include <vector>

#include "KurenaiTypes.h"
#include "Model.h"
#include "RHI/IRHIDevice.h"

namespace Kurenai::Assets
{
    // モデルに依存しない1x1の定数テクスチャ(白 / フラット法線 / 黒 / マゼンタ)を、
    // 複数のモデルで共有するための入れ物。
    //
    // 【なぜ共有するのか】これらはテクスチャスロットを持たないマテリアルのフォールバックで、
    // 従来はモデルごとに作っていた。[Model]が数個のシーンでは無視できるが、多数の.kmodelを
    // 並べるシーン(PLATEAUの東京23区LOD1は671モデル)では2000個超の個別リソースになり、
    // ディスクリプタと、GPUの完了を待つUploadSubmitAndWaitの回数がモデル数に比例して増える。
    // 中身はどのモデルでも同一なので共有して構わない。
    //
    // 【所有権】シーンが持つ(Assets::Scene::SharedTextures)。個々のモデルより長生きし、
    // シーンの破棄と同時に解放される。nullptrを渡した場合は従来どおりモデルが自前で持つ
    // (単体でLoadModelを呼ぶ経路の後方互換のため)
    struct SharedTexturePool
    {
        std::vector<std::unique_ptr<RHI::IRHITexture>> Owned;
        RHI::IRHITexture* White = nullptr;
        RHI::IRHITexture* FlatNormal = nullptr;
        RHI::IRHITexture* Black = nullptr;
        RHI::IRHITexture* Magenta = nullptr;
    };

    KURENAI_LIB_API Model LoadModel(
        RHI::IRHIDevice& device,
        const std::wstring& filePath,
        SharedTexturePool* sharedTextures = nullptr);
}
