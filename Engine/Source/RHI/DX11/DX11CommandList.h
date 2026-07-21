#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "RHI/IRHICommandList.h"

namespace Kurenai::RHI
{
    class DX11CommandList : public IRHICommandList
    {
    public:
        explicit DX11CommandList(Microsoft::WRL::ComPtr<ID3D11DeviceContext> context);

        void SetRenderTarget(IRHISwapChain* swapChain) override;
        void ClearRenderTarget(const ClearColor& color) override;
        void SetViewport(const Viewport& viewport) override;
        void SetPipelineState(IRHIPipelineState* pipelineState) override;
        void SetVertexBuffer(IRHIBuffer* buffer) override;
        void Draw(uint32_t vertexCount, uint32_t startVertexLocation) override;

    private:
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;
        ID3D11RenderTargetView* m_CurrentRenderTargetView = nullptr;
    };
}
