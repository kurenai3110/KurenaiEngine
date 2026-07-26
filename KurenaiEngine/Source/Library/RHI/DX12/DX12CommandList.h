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
        void SetShaderResourceBuffer(uint32_t slot, IRHIBuffer* buffer) override;
        void UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t sizeInBytes) override;
        void Draw(uint32_t vertexCount, uint32_t startVertexLocation) override;
        void DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation) override;

        void SetComputePipelineState(IRHIPipelineState* pipelineState) override;
        void SetComputeConstantBuffer(uint32_t slot, IRHIBuffer* buffer) override;
        void SetComputeTexture(uint32_t slot, IRHITexture* texture) override;
        void SetComputeUnorderedAccessTexture(uint32_t slot, IRHITexture* texture, uint32_t mipLevel = 0) override;
        void SetComputeUnorderedAccessBuffer(uint32_t slot, IRHIBuffer* buffer) override;
        void Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) override;

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
        static constexpr uint32_t kTextureSlotCount = 9;
        uint32_t m_CurrentSrvTableBase = 0;

        // SetTexture()はCopyDescriptorsをその場では呼ばず、コピー元ハンドルをここに溜めておき、
        // Draw直前にFlushPendingSrvWrites()でまとめて1回のCopyDescriptorsに反映する。
        // メッシュごとにテクスチャの数だけCopyDescriptorsSimpleを呼んでいた際のドライバ呼び出し
        // オーバーヘッド(CPU側のディスクリプタコピーコスト)を削減するため
        D3D12_CPU_DESCRIPTOR_HANDLE m_PendingSrvHandles[kTextureSlotCount]{};
        uint32_t m_PendingSrvSlotMask = 0;
        void FlushPendingSrvWrites();

        // 直前にDraw/DrawIndexedへ実際に反映した(コピー済みの)テクスチャの組み合わせ。
        // 同じマテリアルを使う連続したメッシュではテクスチャの組み合わせが変わらないため、
        // 次の描画がこれと完全に一致する場合はSRVテーブルの新規割り当て・CopyDescriptors・
        // ルートテーブルの再バインドをまるごと省略し、既存のブロックをそのまま使い回す。
        // SetPipelineState()はSetGraphicsRootSignatureを呼び直すたびにルート引数を無効化するため、
        // その直後は必ずm_HasLastDrawをfalseにして使い回しを禁止する
        D3D12_CPU_DESCRIPTOR_HANDLE m_LastDrawSrvHandles[kTextureSlotCount]{};
        uint32_t m_LastDrawSlotMask = 0;
        bool m_HasLastDraw = false;

        // コンピュートシェーダー用SRV(t0〜)+UAV(u0〜)テーブル。グラフィックスのSRVテーブルと同様、
        // Set*の時点ではコピー元だけ溜めておき、Dispatch直前のFlushPendingComputeWrites()でまとめて
        // CopyDescriptors・ルートテーブルの再バインドを行う
        static constexpr uint32_t kComputeSrvSlotCount = 4;
        static constexpr uint32_t kComputeUavSlotCount = 4;
        D3D12_CPU_DESCRIPTOR_HANDLE m_PendingComputeSrvHandles[kComputeSrvSlotCount]{};
        uint32_t m_PendingComputeSrvSlotMask = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE m_PendingComputeUavHandles[kComputeUavSlotCount]{};
        uint32_t m_PendingComputeUavSlotMask = 0;
        // 今回のDispatchでUAVとしてバインドされているリソース。Dispatch直後にUAVバリアを発行し、
        // 後続のDispatch/描画がこのDispatchの書き込み完了を確実に見えるようにするため保持する
        ID3D12Resource* m_BoundComputeUavResources[kComputeUavSlotCount]{};
        void FlushPendingComputeWrites();
    };
}
