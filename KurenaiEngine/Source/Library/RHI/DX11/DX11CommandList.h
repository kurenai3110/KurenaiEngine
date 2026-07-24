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
        void SetRenderTargets(IRHITexture* const* targets, uint32_t count, IRHITexture* depthTexture) override;
        void ClearRenderTarget(const ClearColor& color) override;
        void ClearDepth(float depth) override;
        void SetViewport(const Viewport& viewport) override;
        void SetPipelineState(IRHIPipelineState* pipelineState) override;
        void SetVertexBuffer(IRHIBuffer* buffer) override;
        void SetIndexBuffer(IRHIBuffer* buffer) override;
        void SetConstantBuffer(uint32_t slot, IRHIBuffer* buffer) override;
        void SetTexture(uint32_t slot, IRHITexture* texture) override;
        void SetSampler(uint32_t slot, IRHISampler* sampler) override;
        void UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t sizeInBytes) override;
        void Draw(uint32_t vertexCount, uint32_t startVertexLocation) override;
        void DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation) override;

    private:
        static constexpr uint32_t kMaxRenderTargets = 8;

        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;
        ID3D11RenderTargetView* m_CurrentRenderTargetViews[kMaxRenderTargets] = {};
        uint32_t m_CurrentRenderTargetCount = 0;
        ID3D11DepthStencilView* m_CurrentDepthStencilView = nullptr;
    };
}
