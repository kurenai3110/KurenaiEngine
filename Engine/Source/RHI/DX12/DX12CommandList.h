#pragma once

#include <cstdint>
#include <d3d12.h>

#include "RHI/IRHIBuffer.h"
#include "RHI/IRHICommandList.h"
#include "RHI/IRHIPipelineState.h"
#include "RHI/IRHISampler.h"
#include "RHI/IRHISwapChain.h"
#include "RHI/IRHITexture.h"

namespace Kurenai::RHI
{
    class DX12Device;

    class DX12CommandList : public IRHICommandList
    {
    public:
        explicit DX12CommandList(DX12Device* device);

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

        DX12Device* m_Device;
        D3D12_CPU_DESCRIPTOR_HANDLE m_CurrentRenderTargetViews[kMaxRenderTargets]{};
        uint32_t m_CurrentRenderTargetCount = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE m_CurrentDepthStencilView{};
        bool m_HasDepthStencilView = false;

        // 現在の描画で使うSRVテーブルの割り当て済みブロック先頭インデックス。SetTexture(0, ...)のたびに
        // 新しいブロックを割り当て直す(1フレームぶんまとめて記録してから1回だけ実行する設計のため、
        // 同じスロットを使い回すとGPU実行時にはそのフレーム最後の書き込みで上書きされてしまう)
        static constexpr uint32_t kTextureSlotCount = 7;
        uint32_t m_CurrentSrvTableBase = 0;
    };
}
