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
    };

    struct Model
    {
        std::vector<Mesh> Meshes;
        std::vector<std::unique_ptr<RHI::IRHITexture>> Textures;
        float BoundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float BoundsMax[3] = { 0.0f, 0.0f, 0.0f };
    };
}
