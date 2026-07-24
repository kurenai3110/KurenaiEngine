#include "DX12CommandList.h"

#include <cstring>

#include "DX12Buffer.h"
#include "DX12Device.h"
#include "DX12PipelineState.h"
#include "DX12Sampler.h"
#include "DX12SwapChain.h"
#include "DX12Texture.h"

namespace Kurenai::RHI
{
    DX12CommandList::DX12CommandList(DX12Device* device)
        : m_Device(device)
    {
    }

    void DX12CommandList::SetRenderTarget(IRHISwapChain* swapChain)
    {
        auto* dx12SwapChain = static_cast<DX12SwapChain*>(swapChain);
        auto* cmdList = m_Device->GetCommandList();

        dx12SwapChain->TransitionToRenderTarget(cmdList);
        m_CurrentRenderTargetViews[0] = dx12SwapChain->GetCurrentRenderTargetView();
        m_CurrentRenderTargetCount = 1;
        m_CurrentDepthStencilView = dx12SwapChain->GetDepthStencilView();
        m_HasDepthStencilView = true;

        cmdList->OMSetRenderTargets(1, &m_CurrentRenderTargetViews[0], FALSE, &m_CurrentDepthStencilView);
    }

    void DX12CommandList::SetRenderTargets(IRHITexture* const* targets, uint32_t count, IRHITexture* depthTexture)
    {
        count = count < kMaxRenderTargets ? count : kMaxRenderTargets;
        auto* cmdList = m_Device->GetCommandList();

        for (uint32_t i = 0; i < count; ++i)
        {
            auto* dx12Texture = static_cast<DX12Texture*>(targets[i]);
            dx12Texture->TransitionTo(cmdList, D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_CurrentRenderTargetViews[i] = dx12Texture->GetRtvCpuHandle();
        }
        m_CurrentRenderTargetCount = count;

        m_HasDepthStencilView = depthTexture != nullptr;
        if (depthTexture)
        {
            auto* dx12Depth = static_cast<DX12Texture*>(depthTexture);
            dx12Depth->TransitionTo(cmdList, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            m_CurrentDepthStencilView = dx12Depth->GetDsvCpuHandle();
        }

        cmdList->OMSetRenderTargets(
            count, count > 0 ? m_CurrentRenderTargetViews : nullptr, FALSE, m_HasDepthStencilView ? &m_CurrentDepthStencilView : nullptr);
    }

    void DX12CommandList::ClearRenderTarget(const ClearColor& color)
    {
        const float clearColor[4] = { color.R, color.G, color.B, color.A };
        auto* cmdList = m_Device->GetCommandList();
        for (uint32_t i = 0; i < m_CurrentRenderTargetCount; ++i)
        {
            cmdList->ClearRenderTargetView(m_CurrentRenderTargetViews[i], clearColor, 0, nullptr);
        }
    }

    void DX12CommandList::ClearDepth(float depth)
    {
        if (!m_HasDepthStencilView)
        {
            return;
        }

        m_Device->GetCommandList()->ClearDepthStencilView(m_CurrentDepthStencilView, D3D12_CLEAR_FLAG_DEPTH, depth, 0, 0, nullptr);
    }

    void DX12CommandList::SetViewport(const Viewport& viewport)
    {
        auto* cmdList = m_Device->GetCommandList();

        D3D12_VIEWPORT dxViewport{};
        dxViewport.TopLeftX = viewport.TopLeftX;
        dxViewport.TopLeftY = viewport.TopLeftY;
        dxViewport.Width = viewport.Width;
        dxViewport.Height = viewport.Height;
        dxViewport.MinDepth = viewport.MinDepth;
        dxViewport.MaxDepth = viewport.MaxDepth;
        cmdList->RSSetViewports(1, &dxViewport);

        // DX12はD3D11と異なりシザー矩形を必ず設定する必要があるため、ビューポート全体を覆う矩形を張る
        D3D12_RECT scissorRect{};
        scissorRect.left = static_cast<LONG>(viewport.TopLeftX);
        scissorRect.top = static_cast<LONG>(viewport.TopLeftY);
        scissorRect.right = static_cast<LONG>(viewport.TopLeftX + viewport.Width);
        scissorRect.bottom = static_cast<LONG>(viewport.TopLeftY + viewport.Height);
        cmdList->RSSetScissorRects(1, &scissorRect);
    }

    void DX12CommandList::SetPipelineState(IRHIPipelineState* pipelineState)
    {
        auto* dx12PipelineState = static_cast<DX12PipelineState*>(pipelineState);
        auto* cmdList = m_Device->GetCommandList();

        // SetGraphicsRootSignatureは以前バインドされていたルート引数を無効化するため、
        // このPSOで実際に使うb0/b1/テクスチャ/サンプラーは呼び出し側が直後にSetConstantBuffer/SetTexture/SetSamplerで設定し直す
        cmdList->SetGraphicsRootSignature(m_Device->GetRootSignature());
        cmdList->SetPipelineState(dx12PipelineState->GetPipelineState());
        cmdList->IASetPrimitiveTopology(dx12PipelineState->GetTopology());
        // SRVテーブル(ルートパラメータ2)は描画ごとにSetTexture(0, ...)が新しいブロックを割り当てて再バインドする
        cmdList->SetGraphicsRootDescriptorTable(3, m_Device->GetShaderVisibleSamplerHeap()->GetGpuHandle(0));
    }

    void DX12CommandList::SetVertexBuffer(IRHIBuffer* buffer)
    {
        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        const D3D12_VERTEX_BUFFER_VIEW vbv = dx12Buffer->GetVertexBufferView();
        m_Device->GetCommandList()->IASetVertexBuffers(0, 1, &vbv);
    }

    void DX12CommandList::SetIndexBuffer(IRHIBuffer* buffer)
    {
        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        const D3D12_INDEX_BUFFER_VIEW ibv = dx12Buffer->GetIndexBufferView();
        m_Device->GetCommandList()->IASetIndexBuffer(&ibv);
    }

    void DX12CommandList::SetConstantBuffer(uint32_t slot, IRHIBuffer* buffer)
    {
        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        m_Device->GetCommandList()->SetGraphicsRootConstantBufferView(slot, dx12Buffer->GetGPUVirtualAddress());
    }

    void DX12CommandList::SetTexture(uint32_t slot, IRHITexture* texture)
    {
        auto* dx12Texture = static_cast<DX12Texture*>(texture);
        auto* cmdList = m_Device->GetCommandList();
        dx12Texture->TransitionTo(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // slot 0は新しい描画の開始とみなし、t0〜t6ぶんの新しいブロックを割り当てて
        // ルートシグネチャのSRVテーブル(ルートパラメータ2)をそこへ向け直す。
        // これをしないと、1フレームぶんまとめて記録してから1回だけ実行する設計上、
        // GPUが実際にDrawを処理する時点ではそのフレーム最後のSetTexture呼び出しの内容に
        // 全ての描画が上書きされてしまう
        if (slot == 0)
        {
            // 通常はDraw/DrawIndexedで既に反映済みだが、Drawを挟まず連続でslot 0が
            // 呼ばれた場合に備えて念のためここでも反映しておく
            FlushPendingSrvWrites();
            m_CurrentSrvTableBase = m_Device->AllocateSrvTableBlock(kTextureSlotCount);
            cmdList->SetGraphicsRootDescriptorTable(2, m_Device->GetShaderVisibleSrvHeap()->GetGpuHandle(m_CurrentSrvTableBase));
        }

        // CopyDescriptorsSimpleはこの場では呼ばず、コピー元だけ溜めておく。メッシュごとに
        // テクスチャの数だけ個別に呼ぶとドライバ呼び出しのCPUオーバーヘッドが積み重なるため、
        // 実際のコピーはDraw直前にFlushPendingSrvWrites()でまとめて1回行う
        m_PendingSrvHandles[slot] = dx12Texture->GetSrvCpuHandle();
        m_PendingSrvSlotMask |= (1u << slot);
    }

    void DX12CommandList::FlushPendingSrvWrites()
    {
        if (m_PendingSrvSlotMask == 0)
        {
            return;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE destRanges[kTextureSlotCount];
        D3D12_CPU_DESCRIPTOR_HANDLE srcRanges[kTextureSlotCount];
        uint32_t rangeCount = 0;

        auto* heap = m_Device->GetShaderVisibleSrvHeap();
        for (uint32_t slot = 0; slot < kTextureSlotCount; ++slot)
        {
            if (m_PendingSrvSlotMask & (1u << slot))
            {
                destRanges[rangeCount] = heap->GetCpuHandle(m_CurrentSrvTableBase + slot);
                srcRanges[rangeCount] = m_PendingSrvHandles[slot];
                ++rangeCount;
            }
        }

        // 各レンジはすべてサイズ1(pRangeSizes=nullptrは各レンジサイズ1を意味する)なので、
        // この描画で設定された分をまとめて1回のCopyDescriptorsで反映する
        m_Device->GetDevice()->CopyDescriptors(
            rangeCount, destRanges, nullptr, rangeCount, srcRanges, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        m_PendingSrvSlotMask = 0;
    }

    void DX12CommandList::SetSampler(uint32_t slot, IRHISampler* sampler)
    {
        auto* dx12Sampler = static_cast<DX12Sampler*>(sampler);
        const D3D12_CPU_DESCRIPTOR_HANDLE dest = m_Device->GetShaderVisibleSamplerHeap()->GetCpuHandle(slot);
        m_Device->GetDevice()->CopyDescriptorsSimple(1, dest, dx12Sampler->GetCpuHandle(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    }

    void DX12CommandList::UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t sizeInBytes)
    {
        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        memcpy(dx12Buffer->AdvanceRingAndGetWritePtr(), data, sizeInBytes);
    }

    void DX12CommandList::Draw(uint32_t vertexCount, uint32_t startVertexLocation)
    {
        FlushPendingSrvWrites();
        m_Device->GetCommandList()->DrawInstanced(vertexCount, 1, startVertexLocation, 0);
    }

    void DX12CommandList::DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation)
    {
        FlushPendingSrvWrites();
        m_Device->GetCommandList()->DrawIndexedInstanced(indexCount, 1, startIndexLocation, baseVertexLocation, 0);
    }
}
