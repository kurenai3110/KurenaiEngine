#include "DX12CommandList.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <d3dx12.h>

#include "Core/Logger.h"

#include "DX12AccelerationStructure.h"
#include "DX12Buffer.h"
#include "DX12ComputePipelineState.h"
#include "DX12Device.h"
#include "DX12PipelineState.h"
#include "DX12SamplerSet.h"
#include "DX12SwapChain.h"
#include "DX12Texture.h"

namespace Kurenai::RHI
{
    DX12CommandList::DX12CommandList(DX12Device* device)
        : m_Device(device)
        , m_CurrentSamplerSetBase(device->GetFallbackSamplerSetBase())
        , m_CurrentComputeSamplerSetBase(device->GetFallbackSamplerSetBase())
    {
        // 全スロットをnullディスクリプタで初期化しておく。これにより払い出したブロックの
        // どの位置も未初期化のまま残らず、一度もバインドしていないスロットはDX11と同じく0を返す
        const D3D12_CPU_DESCRIPTOR_HANDLE nullSrv = device->GetNullSrvCpuHandle();
        const D3D12_CPU_DESCRIPTOR_HANDLE nullUav = device->GetNullUavCpuHandle();
        std::fill(std::begin(m_PendingSrvHandles), std::end(m_PendingSrvHandles), nullSrv);
        std::fill(std::begin(m_PendingComputeSrvHandles), std::end(m_PendingComputeSrvHandles), nullSrv);
        std::fill(std::begin(m_PendingComputeUavHandles), std::end(m_PendingComputeUavHandles), nullUav);
    }

    void DX12CommandList::UnbindSrvSlotsBoundTo(IRHITexture* texture)
    {
        auto* dx12Texture = static_cast<DX12Texture*>(texture);
        if (!dx12Texture->HasSrv())
        {
            return;
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = dx12Texture->GetSrvCpuHandle();
        const D3D12_CPU_DESCRIPTOR_HANDLE nullSrv = m_Device->GetNullSrvCpuHandle();
        for (uint32_t slot = 0; slot < kTextureSlotCount; ++slot)
        {
            if (m_PendingSrvHandles[slot].ptr == srvHandle.ptr)
            {
                m_PendingSrvHandles[slot] = nullSrv;
            }
        }
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

    void DX12CommandList::SetRenderTargets(
        IRHITexture* const* targets, uint32_t count, IRHITexture* depthTexture, uint32_t depthArraySlice)
    {
        count = count < kMaxRenderTargets ? count : kMaxRenderTargets;
        auto* cmdList = m_Device->GetCommandList();

        uint32_t boundCount = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            auto* dx12Texture = static_cast<DX12Texture*>(targets[i]);

            // RTVを持たないテクスチャは無効なディスクリプタハンドルになりデバイス削除に至るため除外する
            // (DX11は同じ状況でnullptrをバインドして静かに描画をやめる)
            if (!dx12Texture->HasRtv())
            {
                Core::Logger::Error(
                    "DX12", "SetRenderTargets: RTVを持たないテクスチャがレンダーターゲット" + std::to_string(i) + "に指定されました。除外します");
                continue;
            }

            // SRVとして張られたままのスロットがあれば解除する(D3D11ドライバの自動解除と同じ挙動)
            UnbindSrvSlotsBoundTo(dx12Texture);

            dx12Texture->TransitionTo(cmdList, D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_CurrentRenderTargetViews[boundCount] = dx12Texture->GetRtvCpuHandle();
            ++boundCount;
        }
        m_CurrentRenderTargetCount = boundCount;

        m_HasDepthStencilView = false;
        if (depthTexture)
        {
            auto* dx12Depth = static_cast<DX12Texture*>(depthTexture);

            // 範囲外のスライスはGetDsvCpuHandleがベクタ外アクセスになるため、事前に弾いてログを残す
            const uint32_t sliceCount = dx12Depth->GetDepthSliceCount();
            uint32_t slice = depthArraySlice;
            if (sliceCount > 0 && slice >= sliceCount)
            {
                Core::Logger::Error(
                    "DX12",
                    "SetRenderTargets: 深度配列スライス" + std::to_string(slice) + "が範囲外です(スライス数: " +
                        std::to_string(sliceCount) + ")。スライス0へフォールバックします");
                slice = 0;
            }

            if (dx12Depth->HasDsv())
            {
                UnbindSrvSlotsBoundTo(dx12Depth);
                dx12Depth->TransitionTo(cmdList, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                m_CurrentDepthStencilView = dx12Depth->GetDsvCpuHandle(slice);
                m_HasDepthStencilView = true;
            }
            else
            {
                Core::Logger::Error("DX12", "SetRenderTargets: DSVを持たないテクスチャが深度ターゲットに指定されました。深度なしで描画します");
            }
        }

        cmdList->OMSetRenderTargets(
            boundCount,
            boundCount > 0 ? m_CurrentRenderTargetViews : nullptr,
            FALSE,
            m_HasDepthStencilView ? &m_CurrentDepthStencilView : nullptr);
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

        // DX12はD3D11と異なりシザー矩形を必ず設定する必要があるため、ビューポート全体を覆う矩形を張る。
        // DX11はラスタライザステートがScissorEnable=FALSEでそもそもクリップしないので、
        // ここでビューポートより内側に丸めると「DX12だけ端が1px欠ける」という差になる。
        // レターボックス表示ではTopLeftX/Widthが非整数になるため、左上はfloor・右下はceilで
        // 必ずビューポート全体を含むように切り上げる
        D3D12_RECT scissorRect{};
        scissorRect.left = static_cast<LONG>(std::floor(viewport.TopLeftX));
        scissorRect.top = static_cast<LONG>(std::floor(viewport.TopLeftY));
        scissorRect.right = static_cast<LONG>(std::ceil(viewport.TopLeftX + viewport.Width));
        scissorRect.bottom = static_cast<LONG>(std::ceil(viewport.TopLeftY + viewport.Height));
        cmdList->RSSetScissorRects(1, &scissorRect);
    }

    void DX12CommandList::SetPipelineState(IRHIPipelineState* pipelineState)
    {
        auto* dx12PipelineState = static_cast<DX12PipelineState*>(pipelineState);
        auto* cmdList = m_Device->GetCommandList();

        // SetGraphicsRootSignatureは以前バインドされていたルート引数をすべて無効化する。
        // DX11のイミディエイトコンテキストはパイプラインステートを切り替えても定数バッファ・SRV・
        // サンプラーのバインドを保持するため、ここでシャドウコピーから全ルート引数を張り直して
        // 挙動を揃える(そうしないと呼び出し側にDX12だけの「SetPipelineStateより後に呼ぶ」制約が残る)
        cmdList->SetGraphicsRootSignature(m_Device->GetRootSignature());
        cmdList->SetPipelineState(dx12PipelineState->GetPipelineState());
        cmdList->IASetPrimitiveTopology(dx12PipelineState->GetTopology());

        // 定数バッファ(ルートパラメータ0/1)。一度もバインドされていないスロットはアドレスが0なので飛ばす
        for (uint32_t slot = 0; slot < kConstantBufferSlotCount; ++slot)
        {
            if (m_CurrentRootCbv[slot] != 0)
            {
                cmdList->SetGraphicsRootConstantBufferView(slot, m_CurrentRootCbv[slot]);
            }
        }

        // サンプラーテーブル(ルートパラメータ3)を直近のセットへ張り直す
        cmdList->SetGraphicsRootDescriptorTable(3, m_Device->GetShaderVisibleSamplerHeap()->GetGpuHandle(m_CurrentSamplerSetBase));

        // SRVテーブル(ルートパラメータ2)は次のDrawでFlushPendingSrvWrites()が新しいブロックを
        // 払い出して張り直す。ここでは「直前の描画と同じテクスチャならテーブルを使い回す」
        // キャッシュを無効にしておけばよい(ルート引数が無効化されたので使い回せない)
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
        // slotはそのままルートパラメータ番号として使うため、範囲外だとSRVテーブル(2)や
        // サンプラーテーブル(3)のルート引数をGPU仮想アドレスで上書きしてしまう
        if (slot >= kConstantBufferSlotCount)
        {
            Core::Logger::Error(
                "DX12",
                "SetConstantBuffer: スロット" + std::to_string(slot) + "は範囲外です(有効なのはb0〜b" +
                    std::to_string(kConstantBufferSlotCount - 1) + ")。バインドをスキップします");
            return;
        }

        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        // GPU仮想アドレスはリングの現在スロットを指すため、UpdateBufferより後に呼ぶ必要がある
        // (IRHICommandList.hのSetConstantBufferのコメント参照)。
        // SetPipelineStateでルート引数が無効化されたときに張り直せるようキャッシュしておく
        const D3D12_GPU_VIRTUAL_ADDRESS address = dx12Buffer->GetGPUVirtualAddress();
        m_CurrentRootCbv[slot] = address;
        m_Device->GetCommandList()->SetGraphicsRootConstantBufferView(slot, address);
    }

    void DX12CommandList::SetTexture(uint32_t slot, IRHITexture* texture)
    {
        if (slot >= kTextureSlotCount)
        {
            Core::Logger::Error(
                "DX12",
                "SetTexture: スロット" + std::to_string(slot) + "は範囲外です(有効なのはt0〜t" +
                    std::to_string(kTextureSlotCount - 1) + ")。バインドをスキップします");
            return;
        }

        auto* dx12Texture = static_cast<DX12Texture*>(texture);
        auto* cmdList = m_Device->GetCommandList();
        dx12Texture->TransitionTo(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // SRVテーブルの割り当て・CopyDescriptorsはこの場では行わず、コピー元だけ記録しておく。
        // 実際の割り当て・コピーはDraw直前のFlushPendingSrvWrites()でまとめて行う(そこで
        // 直前の描画と同じ組み合わせかどうかも判定し、同じなら丸ごと省略する)。
        // 記録した内容は上書きするまで残るため、DX11のPSSetShaderResourcesと同じく
        // Drawやパイプラインステート切り替えをまたいでバインドが維持される
        m_PendingSrvHandles[slot] = dx12Texture->GetSrvCpuHandle();
    }

    void DX12CommandList::FlushPendingSrvWrites()
    {
        // 直前にDrawへ反映した組み合わせと完全に一致するか(同じマテリアルを使う連続した
        // メッシュではよくある)を調べる。一致するならルートパラメータ2は既に正しいブロックを
        // 指したままなので、新規割り当て・CopyDescriptors・ルートテーブルの再バインドを
        // まるごと省略できる
        bool sameAsLastDraw = m_HasLastDraw;
        if (sameAsLastDraw)
        {
            for (uint32_t slot = 0; slot < kTextureSlotCount; ++slot)
            {
                if (m_PendingSrvHandles[slot].ptr != m_LastDrawSrvHandles[slot].ptr)
                {
                    sameAsLastDraw = false;
                    break;
                }
            }
        }

        if (sameAsLastDraw)
        {
            return;
        }

        auto* cmdList = m_Device->GetCommandList();
        m_CurrentSrvTableBase = m_Device->AllocateSrvTableBlock(kTextureSlotCount);
        cmdList->SetGraphicsRootDescriptorTable(2, m_Device->GetShaderVisibleSrvHeap()->GetGpuHandle(m_CurrentSrvTableBase));

        D3D12_CPU_DESCRIPTOR_HANDLE destRanges[kTextureSlotCount];
        D3D12_CPU_DESCRIPTOR_HANDLE srcRanges[kTextureSlotCount];

        auto* heap = m_Device->GetShaderVisibleSrvHeap();
        for (uint32_t slot = 0; slot < kTextureSlotCount; ++slot)
        {
            destRanges[slot] = heap->GetCpuHandle(m_CurrentSrvTableBase + slot);
            srcRanges[slot] = m_PendingSrvHandles[slot];
        }

        // 未バインドのスロットもnullディスクリプタが入っているため常に全スロットをコピーする。
        // **「今回セットされたスロットだけ」をコピーしてはいけない** ―― 払い出したブロックの残りが
        // リングの前世代のディスクリプタ(未初期化ゴミ)のまま残る。
        // 各レンジはすべてサイズ1(pRangeSizes=nullptrは各レンジサイズ1を意味する)
        m_Device->GetDevice()->CopyDescriptors(
            kTextureSlotCount, destRanges, nullptr, kTextureSlotCount, srcRanges, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        std::copy(std::begin(m_PendingSrvHandles), std::end(m_PendingSrvHandles), std::begin(m_LastDrawSrvHandles));
        m_HasLastDraw = true;
    }

    void DX12CommandList::SetSamplerSet(IRHISamplerSet* samplerSet)
    {
        if (!samplerSet)
        {
            Core::Logger::Error("DX12", "SetSamplerSet: サンプラーセットがnullptrのためバインドをスキップします");
            return;
        }

        // ヒープの中身は書き換えず、テーブルの先頭位置だけを切り替える。
        // これによりフレーム中に複数のパスが別々のセットを使っても互いに干渉しない(IRHISamplerSet.h参照)
        auto* dx12SamplerSet = static_cast<DX12SamplerSet*>(samplerSet);
        const D3D12_GPU_DESCRIPTOR_HANDLE base = dx12SamplerSet->GetBaseGpuHandle();
        m_CurrentSamplerSetBase = dx12SamplerSet->GetBaseDescriptorIndex();
        m_Device->GetCommandList()->SetGraphicsRootDescriptorTable(3, base);
    }

    void DX12CommandList::SetShaderResourceBuffer(uint32_t slot, IRHIBuffer* buffer)
    {
        if (slot >= kTextureSlotCount)
        {
            Core::Logger::Error(
                "DX12",
                "SetShaderResourceBuffer: スロット" + std::to_string(slot) + "は範囲外です(有効なのはt0〜t" +
                    std::to_string(kTextureSlotCount - 1) + ")。バインドをスキップします");
            return;
        }

        // BufferUsage::StructuredReadOnlyのSRVはSetTextureのテクスチャSRVと同じディスクリプタテーブル
        // (t0〜t11)を共有するため、バインド経路もSetTextureと完全に同じ(コピー元だけ記録しておき、
        // Draw直前のFlushPendingSrvWritesでまとめて反映する)にする
        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        auto* cmdList = m_Device->GetCommandList();
        dx12Buffer->TransitionTo(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        m_PendingSrvHandles[slot] = dx12Buffer->GetSrvCpuHandle();
    }

    void DX12CommandList::UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t sizeInBytes)
    {
        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);

        // BufferUsage::StructuredImmutableは作成時の初期データから書き換えない前提のUsageで、
        // CPU書き込み経路(マップ済みポインタ・ステージングリング)を一切持たない。
        // そのまま下の経路へ進むとnullptrへ書き込んでしまうため、ここで弾く
        if (dx12Buffer->IsStructuredImmutable())
        {
            Core::Logger::Error(
                "DX12", "UpdateBuffer: BufferUsage::StructuredImmutableのバッファは更新できません。更新をスキップします");
            return;
        }

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

        // SetComputeRootSignatureは以前バインドされていたルート引数をすべて無効化するため、
        // グラフィックス側と同じくシャドウコピーから定数バッファ・サンプラーテーブルを張り直す
        cmdList->SetComputeRootSignature(m_Device->GetComputeRootSignature());
        cmdList->SetPipelineState(dx12ComputePipelineState->GetPipelineState());

        for (uint32_t slot = 0; slot < kConstantBufferSlotCount; ++slot)
        {
            if (m_CurrentComputeRootCbv[slot] != 0)
            {
                cmdList->SetComputeRootConstantBufferView(slot, m_CurrentComputeRootCbv[slot]);
            }
        }

        cmdList->SetComputeRootDescriptorTable(3, m_Device->GetShaderVisibleSamplerHeap()->GetGpuHandle(m_CurrentComputeSamplerSetBase));
    }

    void DX12CommandList::SetComputeConstantBuffer(uint32_t slot, IRHIBuffer* buffer)
    {
        if (slot >= kConstantBufferSlotCount)
        {
            Core::Logger::Error(
                "DX12",
                "SetComputeConstantBuffer: スロット" + std::to_string(slot) + "は範囲外です(有効なのはb0〜b" +
                    std::to_string(kConstantBufferSlotCount - 1) + ")。バインドをスキップします");
            return;
        }

        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        const D3D12_GPU_VIRTUAL_ADDRESS address = dx12Buffer->GetGPUVirtualAddress();
        m_CurrentComputeRootCbv[slot] = address;
        m_Device->GetCommandList()->SetComputeRootConstantBufferView(slot, address);
    }

    void DX12CommandList::SetComputeSamplerSet(IRHISamplerSet* samplerSet)
    {
        if (!samplerSet)
        {
            Core::Logger::Error("DX12", "SetComputeSamplerSet: サンプラーセットがnullptrのためバインドをスキップします");
            return;
        }

        // コンピュート用ルートシグネチャもグラフィックスと同じサンプラーヒープを共有する。
        // 違いはルート引数を設定する先(グラフィックス用/コンピュート用)だけ
        auto* dx12SamplerSet = static_cast<DX12SamplerSet*>(samplerSet);
        const D3D12_GPU_DESCRIPTOR_HANDLE base = dx12SamplerSet->GetBaseGpuHandle();
        m_CurrentComputeSamplerSetBase = dx12SamplerSet->GetBaseDescriptorIndex();
        m_Device->GetCommandList()->SetComputeRootDescriptorTable(3, base);
    }

    void DX12CommandList::SetComputeTexture(uint32_t slot, IRHITexture* texture)
    {
        if (slot >= kComputeSrvSlotCount)
        {
            Core::Logger::Error(
                "DX12",
                "SetComputeTexture: スロット" + std::to_string(slot) + "は範囲外です(有効なのはt0〜t" +
                    std::to_string(kComputeSrvSlotCount - 1) + ")。バインドをスキップします");
            return;
        }

        auto* dx12Texture = static_cast<DX12Texture*>(texture);
        dx12Texture->TransitionTo(m_Device->GetCommandList(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        m_PendingComputeSrvHandles[slot] = dx12Texture->GetSrvCpuHandle();
    }

    void DX12CommandList::SetComputeShaderResourceBuffer(uint32_t slot, IRHIBuffer* buffer)
    {
        if (slot >= kComputeSrvSlotCount)
        {
            Core::Logger::Error(
                "DX12",
                "SetComputeShaderResourceBuffer: スロット" + std::to_string(slot) + "は範囲外です(有効なのはt0〜t" +
                    std::to_string(kComputeSrvSlotCount - 1) + ")。バインドをスキップします");
            return;
        }

        // 構造化バッファのSRVはSetComputeTextureのテクスチャSRVと同じディスクリプタテーブルを共有するため、
        // バインド経路もSetComputeTextureと完全に同じにする(コピー元だけ記録しておき、
        // Dispatch直前のFlushPendingComputeWritesでまとめて反映する)
        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        dx12Buffer->TransitionTo(m_Device->GetCommandList(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        m_PendingComputeSrvHandles[slot] = dx12Buffer->GetSrvCpuHandle();
    }

    void DX12CommandList::SetComputeUnorderedAccessTexture(uint32_t slot, IRHITexture* texture, uint32_t mipLevel)
    {
        if (slot >= kComputeUavSlotCount)
        {
            Core::Logger::Error(
                "DX12",
                "SetComputeUnorderedAccessTexture: スロット" + std::to_string(slot) + "は範囲外です(有効なのはu0〜u" +
                    std::to_string(kComputeUavSlotCount - 1) + ")。バインドをスキップします");
            return;
        }

        auto* dx12Texture = static_cast<DX12Texture*>(texture);
        dx12Texture->TransitionTo(m_Device->GetCommandList(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        m_PendingComputeUavHandles[slot] = dx12Texture->GetUavCpuHandle(mipLevel);
        m_BoundComputeUavResources[slot] = dx12Texture->GetResource();
    }

    void DX12CommandList::SetComputeUnorderedAccessTextureCubeFace(
        uint32_t slot, IRHITexture* texture, uint32_t face, uint32_t mipLevel, uint32_t cubeIndex)
    {
        if (slot >= kComputeUavSlotCount)
        {
            Core::Logger::Error(
                "DX12",
                "SetComputeUnorderedAccessTextureCubeFace: スロット" + std::to_string(slot) + "は範囲外です(有効なのはu0〜u" +
                    std::to_string(kComputeUavSlotCount - 1) + ")。バインドをスキップします");
            return;
        }
        if (!texture)
        {
            Core::Logger::Error("DX12", "SetComputeUnorderedAccessTextureCubeFace: テクスチャがnullptrのためバインドをスキップします");
            return;
        }

        auto* dx12Texture = static_cast<DX12Texture*>(texture);

        dx12Texture->TransitionTo(m_Device->GetCommandList(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // 面・ミップ・キューブ番号が範囲外の場合はDX12Texture側がログを出してnullディスクリプタを返す
        m_PendingComputeUavHandles[slot] = dx12Texture->GetCubeUavCpuHandle(face, mipLevel, cubeIndex);
        m_BoundComputeUavResources[slot] = dx12Texture->GetResource();
    }

    void DX12CommandList::SetComputeUnorderedAccessBuffer(uint32_t slot, IRHIBuffer* buffer)
    {
        if (slot >= kComputeUavSlotCount)
        {
            Core::Logger::Error(
                "DX12",
                "SetComputeUnorderedAccessBuffer: スロット" + std::to_string(slot) + "は範囲外です(有効なのはu0〜u" +
                    std::to_string(kComputeUavSlotCount - 1) + ")。バインドをスキップします");
            return;
        }

        auto* dx12Buffer = static_cast<DX12Buffer*>(buffer);
        // テクスチャ版と同じくUNORDERED_ACCESSへ遷移させる(直前にピクセルシェーダから
        // 読まれていた場合、遷移しないままUAVとして書くと結果が未定義になる)
        dx12Buffer->TransitionTo(m_Device->GetCommandList(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        m_PendingComputeUavHandles[slot] = dx12Buffer->GetUavCpuHandle();
        m_BoundComputeUavResources[slot] = dx12Buffer->GetResource();
    }

    void DX12CommandList::SetComputeAccelerationStructure(uint32_t slot, IRHIAccelerationStructure* accelerationStructure)
    {
        if (slot >= kComputeSrvSlotCount)
        {
            Core::Logger::Error(
                "DX12",
                "SetComputeAccelerationStructure: スロット" + std::to_string(slot) + "は範囲外です(有効なのはt0〜t" +
                    std::to_string(kComputeSrvSlotCount - 1) + ")。バインドをスキップします");
            return;
        }
        if (!accelerationStructure)
        {
            Core::Logger::Error("DX12", "SetComputeAccelerationStructure: TLASがnullptrです。バインドをスキップします");
            return;
        }

        auto* dx12As = static_cast<DX12AccelerationStructure*>(accelerationStructure);
        if (!dx12As->IsTopLevel())
        {
            // SRVを持つのはTLASだけ。BLASを渡すと未初期化のディスクリプタを指してしまう
            Core::Logger::Error(
                "DX12", "SetComputeAccelerationStructure: BLASはシェーダーへバインドできません(TLASを渡してください)。バインドをスキップします");
            return;
        }

        // ASバッファはD3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE状態のまま遷移しないため、
        // 他のSRVバインドと違ってTransitionToに相当する処理は不要。
        // コピー元の記録だけ行い、実際の反映はDispatch直前のFlushPendingComputeWritesでまとめて行う
        m_PendingComputeSrvHandles[slot] = dx12As->GetSrvCpuHandle();
    }

    void DX12CommandList::FlushPendingComputeWrites()
    {
        constexpr uint32_t kComputeTableSlotCount = kComputeSrvSlotCount + kComputeUavSlotCount;

        const uint32_t tableBase = m_Device->AllocateComputeTableBlock(kComputeTableSlotCount);
        auto* heap = m_Device->GetShaderVisibleSrvHeap();
        m_Device->GetCommandList()->SetComputeRootDescriptorTable(2, heap->GetGpuHandle(tableBase));

        D3D12_CPU_DESCRIPTOR_HANDLE destRanges[kComputeTableSlotCount];
        D3D12_CPU_DESCRIPTOR_HANDLE srcRanges[kComputeTableSlotCount];

        for (uint32_t slot = 0; slot < kComputeSrvSlotCount; ++slot)
        {
            destRanges[slot] = heap->GetCpuHandle(tableBase + slot);
            srcRanges[slot] = m_PendingComputeSrvHandles[slot];
        }
        for (uint32_t slot = 0; slot < kComputeUavSlotCount; ++slot)
        {
            destRanges[kComputeSrvSlotCount + slot] = heap->GetCpuHandle(tableBase + kComputeSrvSlotCount + slot);
            srcRanges[kComputeSrvSlotCount + slot] = m_PendingComputeUavHandles[slot];
        }

        // Dispatchのたびに新しいテーブルブロックを払い出す方式のため、シャドウ配列の全スロットを
        // 毎回コピーする。**「マスクが立っているスロットだけ」をコピーしてはいけない** ――
        // 2回目以降のDispatchで前回のバインドが引き継がれず未初期化のディスクリプタを参照する
        // (IBL畳み込みパスでスカイボックスを1回だけバインドし、面ごとにUAVだけ差し替えて
        // 6回Dispatchすると、DX12だけ2面目以降が真っ黒になる)。
        // コンストラクタで全スロットをnullディスクリプタに初期化してあるため、
        // マスクを持たずに全スロットコピーするだけでDX11と同じバインド寿命になる
        m_Device->GetDevice()->CopyDescriptors(
            kComputeTableSlotCount, destRanges, nullptr, kComputeTableSlotCount, srcRanges, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void DX12CommandList::Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ)
    {
        FlushPendingComputeWrites();
        m_Device->GetCommandList()->Dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ);

        // このDispatchでUAVとして書き込んだリソースは、直後に別のDispatchやSRVとして読む場合に
        // 書き込み完了を保証する必要があるため、UAVバリアを発行しておく。
        // あわせてUAVスロットのシャドウをnullディスクリプタへ戻す。DX11がDispatch直後に
        // CSSetUnorderedAccessViewsでnullを張って全スロットを解除するのに合わせた挙動で、
        // これにより破棄済みリソース(リサイズで作り直されるHi-Zテクスチャなど)の
        // ディスクリプタがシャドウに残り続けることも防げる。
        // なおSRVスロットはDX11も解除しないためここでは触らない
        const D3D12_CPU_DESCRIPTOR_HANDLE nullUav = m_Device->GetNullUavCpuHandle();
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
            m_PendingComputeUavHandles[slot] = nullUav;
        }
        if (barrierCount > 0)
        {
            m_Device->GetCommandList()->ResourceBarrier(barrierCount, barriers);
        }
    }
}
