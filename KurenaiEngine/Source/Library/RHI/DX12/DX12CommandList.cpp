#include "DX12CommandList.h"

#include <algorithm>
#include <cstring>

#include <d3dx12.h>

#include "DX12Buffer.h"
#include "DX12ComputePipelineState.h"
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

        // SetGraphicsRootSignatureでルートパラメータ2(SRVテーブル)も無効化されたため、
        // 「直前の描画と同じテクスチャならテーブルを使い回す」キャッシュは無効にする
        m_HasLastDraw = false;
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

        // slot 0は新しい描画の開始とみなす。通常はDraw/DrawIndexedで前回ぶんは既に反映済みだが、
        // Drawを挟まず連続でslot 0が呼ばれた場合に備えて念のためここでも反映しておく
        if (slot == 0)
        {
            FlushPendingSrvWrites();
        }

        // SRVテーブルの割り当て・CopyDescriptorsはこの場では行わず、コピー元だけ溜めておく。
        // 実際の割り当て・コピーはDraw直前のFlushPendingSrvWrites()でまとめて行う(そこで
        // 直前の描画と同じ組み合わせかどうかも判定し、同じなら丸ごと省略する)
        m_PendingSrvHandles[slot] = dx12Texture->GetSrvCpuHandle();
        m_PendingSrvSlotMask |= (1u << slot);
    }

    void DX12CommandList::FlushPendingSrvWrites()
    {
        if (m_PendingSrvSlotMask == 0)
        {
            return;
        }

        // 直前にDrawへ反映した組み合わせと完全に一致するか(同じマテリアルを使う連続した
        // メッシュではよくある)を調べる。一致するならルートパラメータ2は既に正しいブロックを
        // 指したままなので、新規割り当て・CopyDescriptors・ルートテーブルの再バインドを
        // まるごと省略できる
        bool sameAsLastDraw = m_HasLastDraw && m_PendingSrvSlotMask == m_LastDrawSlotMask;
        if (sameAsLastDraw)
        {
            for (uint32_t slot = 0; slot < kTextureSlotCount; ++slot)
            {
                if ((m_PendingSrvSlotMask & (1u << slot)) && m_PendingSrvHandles[slot].ptr != m_LastDrawSrvHandles[slot].ptr)
                {
                    sameAsLastDraw = false;
                    break;
                }
            }
        }

        if (!sameAsLastDraw)
        {
            auto* cmdList = m_Device->GetCommandList();
            m_CurrentSrvTableBase = m_Device->AllocateSrvTableBlock(kTextureSlotCount);
            cmdList->SetGraphicsRootDescriptorTable(2, m_Device->GetShaderVisibleSrvHeap()->GetGpuHandle(m_CurrentSrvTableBase));

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

            std::copy(std::begin(m_PendingSrvHandles), std::end(m_PendingSrvHandles), std::begin(m_LastDrawSrvHandles));
            m_LastDrawSlotMask = m_PendingSrvSlotMask;
            m_HasLastDraw = true;
        }

        m_PendingSrvSlotMask = 0;
    }

    void DX12CommandList::SetSampler(uint32_t slot, IRHISampler* sampler)
    {
        auto* dx12Sampler = static_cast<DX12Sampler*>(sampler);
        const D3D12_CPU_DESCRIPTOR_HANDLE dest = m_Device->GetShaderVisibleSamplerHeap()->GetCpuHandle(slot);
        m_Device->GetDevice()->CopyDescriptorsSimple(1, dest, dx12Sampler->GetCpuHandle(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    }

    void DX12CommandList::SetShaderResourceBuffer(uint32_t slot, IRHIBuffer* buffer)
    {
        // BufferUsage::StructuredReadOnlyのSRVはSetTextureのテクスチャSRVと同じディスクリプタテーブル
        // (t0〜t6)を共有するため、バインド経路もSetTextureと完全に同じ(コピー元だけ溜めておき、
        // Draw直前のFlushPendingSrvWritesでまとめて反映する)にする
        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        auto* cmdList = m_Device->GetCommandList();
        dx12Buffer->TransitionTo(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        if (slot == 0)
        {
            FlushPendingSrvWrites();
        }

        m_PendingSrvHandles[slot] = dx12Buffer->GetSrvCpuHandle();
        m_PendingSrvSlotMask |= (1u << slot);
    }

    void DX12CommandList::UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t sizeInBytes)
    {
        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);

        // BufferUsage::StructuredReadOnlyはUPLOADヒープに直接マップされていない(ピクセルごとに
        // 読まれるためDEFAULTヒープに本体を置いている)。ステージングリングへ書き込んでから
        // コマンドリスト上でCopyBufferRegionによりDEFAULTヒープ本体へコピーする
        if (dx12Buffer->IsStructuredReadOnly())
        {
            memcpy(dx12Buffer->AdvanceUploadRingAndGetWritePtr(), data, sizeInBytes);

            auto* cmdList = m_Device->GetCommandList();
            dx12Buffer->TransitionTo(cmdList, D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList->CopyBufferRegion(
                dx12Buffer->GetResource(), 0, dx12Buffer->GetUploadResource(), dx12Buffer->GetUploadRingOffset(), sizeInBytes);
            dx12Buffer->TransitionTo(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            return;
        }

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

    void DX12CommandList::SetComputePipelineState(IRHIPipelineState* pipelineState)
    {
        auto* dx12ComputePipelineState = static_cast<DX12ComputePipelineState*>(pipelineState);
        auto* cmdList = m_Device->GetCommandList();

        // SetComputeRootSignatureは以前バインドされていたルート引数を無効化するため、
        // このPSOで実際に使うb0/b1/SRV・UAVは呼び出し側が直後にSetComputeConstantBuffer/
        // SetComputeTexture/SetComputeUnorderedAccessTexture(Buffer)で設定し直す
        cmdList->SetComputeRootSignature(m_Device->GetComputeRootSignature());
        cmdList->SetPipelineState(dx12ComputePipelineState->GetPipelineState());
        // サンプラーテーブル(ルートパラメータ3)はグラフィックス同様s0固定で常に同じものを使うため、
        // ここで一度だけバインドしておく
        cmdList->SetComputeRootDescriptorTable(3, m_Device->GetShaderVisibleSamplerHeap()->GetGpuHandle(0));
    }

    void DX12CommandList::SetComputeConstantBuffer(uint32_t slot, IRHIBuffer* buffer)
    {
        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        m_Device->GetCommandList()->SetComputeRootConstantBufferView(slot, dx12Buffer->GetGPUVirtualAddress());
    }

    void DX12CommandList::SetComputeSampler(uint32_t slot, IRHISampler* sampler)
    {
        // コンピュート用ルートシグネチャもグラフィックスと同じs0固定の共有サンプラーヒープ
        // (SetComputePipelineStateが毎回ルートパラメータ3をこのヒープの先頭にバインドする)を使うため、
        // 実装はSetSamplerと同一(書き込み先ヒープが同じであれば呼び出し元のステージは問わない)
        SetSampler(slot, sampler);
    }

    void DX12CommandList::SetComputeTexture(uint32_t slot, IRHITexture* texture)
    {
        auto* dx12Texture = static_cast<DX12Texture*>(texture);
        dx12Texture->TransitionTo(m_Device->GetCommandList(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        m_PendingComputeSrvHandles[slot] = dx12Texture->GetSrvCpuHandle();
        m_PendingComputeSrvSlotMask |= (1u << slot);
    }

    void DX12CommandList::SetComputeUnorderedAccessTexture(uint32_t slot, IRHITexture* texture, uint32_t mipLevel)
    {
        auto* dx12Texture = static_cast<DX12Texture*>(texture);
        dx12Texture->TransitionTo(m_Device->GetCommandList(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        m_PendingComputeUavHandles[slot] = dx12Texture->GetUavCpuHandle(mipLevel);
        m_PendingComputeUavSlotMask |= (1u << slot);
        m_BoundComputeUavResources[slot] = dx12Texture->GetResource();
    }

    void DX12CommandList::SetComputeUnorderedAccessTextureCubeFace(uint32_t slot, IRHITexture* texture, uint32_t face, uint32_t mipLevel)
    {
        auto* dx12Texture = static_cast<DX12Texture*>(texture);
        dx12Texture->TransitionTo(m_Device->GetCommandList(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        m_PendingComputeUavHandles[slot] = dx12Texture->GetCubeUavCpuHandle(face, mipLevel);
        m_PendingComputeUavSlotMask |= (1u << slot);
        m_BoundComputeUavResources[slot] = dx12Texture->GetResource();
    }

    void DX12CommandList::SetComputeUnorderedAccessBuffer(uint32_t slot, IRHIBuffer* buffer)
    {
        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);

        m_PendingComputeUavHandles[slot] = dx12Buffer->GetUavCpuHandle();
        m_PendingComputeUavSlotMask |= (1u << slot);
        m_BoundComputeUavResources[slot] = dx12Buffer->GetResource();
    }

    void DX12CommandList::FlushPendingComputeWrites()
    {
        if (m_PendingComputeSrvSlotMask == 0 && m_PendingComputeUavSlotMask == 0)
        {
            return;
        }

        const uint32_t tableBase = m_Device->AllocateComputeTableBlock(kComputeSrvSlotCount + kComputeUavSlotCount);
        auto* heap = m_Device->GetShaderVisibleSrvHeap();
        m_Device->GetCommandList()->SetComputeRootDescriptorTable(2, heap->GetGpuHandle(tableBase));

        D3D12_CPU_DESCRIPTOR_HANDLE destRanges[kComputeSrvSlotCount + kComputeUavSlotCount];
        D3D12_CPU_DESCRIPTOR_HANDLE srcRanges[kComputeSrvSlotCount + kComputeUavSlotCount];
        uint32_t rangeCount = 0;

        for (uint32_t slot = 0; slot < kComputeSrvSlotCount; ++slot)
        {
            if (m_PendingComputeSrvSlotMask & (1u << slot))
            {
                destRanges[rangeCount] = heap->GetCpuHandle(tableBase + slot);
                srcRanges[rangeCount] = m_PendingComputeSrvHandles[slot];
                ++rangeCount;
            }
        }
        for (uint32_t slot = 0; slot < kComputeUavSlotCount; ++slot)
        {
            if (m_PendingComputeUavSlotMask & (1u << slot))
            {
                destRanges[rangeCount] = heap->GetCpuHandle(tableBase + kComputeSrvSlotCount + slot);
                srcRanges[rangeCount] = m_PendingComputeUavHandles[slot];
                ++rangeCount;
            }
        }

        m_Device->GetDevice()->CopyDescriptors(
            rangeCount, destRanges, nullptr, rangeCount, srcRanges, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        m_PendingComputeSrvSlotMask = 0;
        m_PendingComputeUavSlotMask = 0;
    }

    void DX12CommandList::Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ)
    {
        FlushPendingComputeWrites();
        m_Device->GetCommandList()->Dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ);

        // このDispatchでUAVとして書き込んだリソースは、直後に別のDispatchやSRVとして読む場合に
        // 書き込み完了を保証する必要があるため、UAVバリアを発行しておく
        D3D12_RESOURCE_BARRIER barriers[kComputeUavSlotCount];
        uint32_t barrierCount = 0;
        for (uint32_t slot = 0; slot < kComputeUavSlotCount; ++slot)
        {
            if (m_BoundComputeUavResources[slot])
            {
                barriers[barrierCount] = CD3DX12_RESOURCE_BARRIER::UAV(m_BoundComputeUavResources[slot]);
                ++barrierCount;
                m_BoundComputeUavResources[slot] = nullptr;
            }
        }
        if (barrierCount > 0)
        {
            m_Device->GetCommandList()->ResourceBarrier(barrierCount, barriers);
        }
    }
}
