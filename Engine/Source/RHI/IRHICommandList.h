#pragma once

#include <cstdint>

#include "IRHIBuffer.h"
#include "IRHIPipelineState.h"
#include "IRHISwapChain.h"

namespace Kurenai::RHI
{
    struct Viewport
    {
        float TopLeftX = 0.0f;
        float TopLeftY = 0.0f;
        float Width = 0.0f;
        float Height = 0.0f;
        float MinDepth = 0.0f;
        float MaxDepth = 1.0f;
    };

    struct ClearColor
    {
        float R = 0.0f;
        float G = 0.0f;
        float B = 0.0f;
        float A = 1.0f;
    };

    class IRHICommandList
    {
    public:
        virtual ~IRHICommandList() = default;

        virtual void SetRenderTarget(IRHISwapChain* swapChain) = 0;
        virtual void ClearRenderTarget(const ClearColor& color) = 0;
        virtual void SetViewport(const Viewport& viewport) = 0;
        virtual void SetPipelineState(IRHIPipelineState* pipelineState) = 0;
        virtual void SetVertexBuffer(IRHIBuffer* buffer) = 0;
        virtual void Draw(uint32_t vertexCount, uint32_t startVertexLocation) = 0;
    };
}
