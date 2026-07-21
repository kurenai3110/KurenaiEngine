#pragma once

#include <memory>
#include <vector>

#include "RHI/IRHIBuffer.h"
#include "RHI/IRHITexture.h"

namespace Kurenai::Assets
{
    struct Mesh
    {
        std::unique_ptr<RHI::IRHIBuffer> VertexBuffer;
        std::unique_ptr<RHI::IRHIBuffer> IndexBuffer;
        uint32_t IndexCount = 0;
        RHI::IRHITexture* BaseColorTexture = nullptr;
        RHI::IRHITexture* NormalTexture = nullptr;
        RHI::IRHITexture* MetallicRoughnessTexture = nullptr;
        float MetallicFactor = 0.0f;
        float RoughnessFactor = 0.7f;
    };

    struct Model
    {
        std::vector<Mesh> Meshes;
        std::vector<std::unique_ptr<RHI::IRHITexture>> Textures;
        float BoundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float BoundsMax[3] = { 0.0f, 0.0f, 0.0f };
    };
}
