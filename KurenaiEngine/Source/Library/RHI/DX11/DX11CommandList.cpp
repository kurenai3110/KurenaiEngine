#include "DX11CommandList.h"

#include <cstring>
#include <utility>

#include "Core/Logger.h"

#include "DX11Buffer.h"
#include "DX11ComputePipelineState.h"
#include "DX11PipelineState.h"
#include "DX11SamplerSet.h"
#include "DX11Shader.h"
#include "DX11SwapChain.h"
#include "DX11Texture.h"

namespace Kurenai::RHI
{
    DX11CommandList::DX11CommandList(Microsoft::WRL::ComPtr<ID3D11DeviceContext> context)
        : m_Context(std::move(context))
    {
    }

    void DX11CommandList::SetRenderTarget(IRHISwapChain* swapChain)
    {
        auto* dx11SwapChain = static_cast<DX11SwapChain*>(swapChain);
        m_CurrentRenderTargetViews[0] = dx11SwapChain->GetRenderTargetView();
        m_CurrentRenderTargetCount = 1;
        m_CurrentDepthStencilView = dx11SwapChain->GetDepthStencilView();
        m_Context->OMSetRenderTargets(1, m_CurrentRenderTargetViews, m_CurrentDepthStencilView);
    }

    void DX11CommandList::SetRenderTargets(
        IRHITexture* const* targets, uint32_t count, IRHITexture* depthTexture, uint32_t depthArraySlice)
    {
        count = count < kMaxRenderTargets ? count : kMaxRenderTargets;
        for (uint32_t i = 0; i < count; ++i)
        {
            m_CurrentRenderTargetViews[i] = static_cast<DX11Texture*>(targets[i])->GetRenderTargetView();
        }
        m_CurrentRenderTargetCount = count;

        m_CurrentDepthStencilView = nullptr;
        if (depthTexture)
        {
            auto* dx11Depth = static_cast<DX11Texture*>(depthTexture);
            m_CurrentDepthStencilView = dx11Depth->GetDepthStencilView(depthArraySlice);
            if (!m_CurrentDepthStencilView)
            {
                // 範囲外のスライス指定は深度なしで描かれて結果が静かに壊れるため、必ずログを残す
                Core::Logger::Error(
                    "DX11",
                    "SetRenderTargets: 深度配列スライス" + std::to_string(depthArraySlice) +
                        "が範囲外です(スライス数: " + std::to_string(dx11Depth->GetDepthSliceCount()) + ")");
            }
        }

        m_Context->OMSetRenderTargets(count, m_CurrentRenderTargetViews, m_CurrentDepthStencilView);
    }

    void DX11CommandList::ClearRenderTarget(const ClearColor& color)
    {
        const float clearColor[4] = { color.R, color.G, color.B, color.A };
        for (uint32_t i = 0; i < m_CurrentRenderTargetCount; ++i)
        {
            if (m_CurrentRenderTargetViews[i])
            {
                m_Context->ClearRenderTargetView(m_CurrentRenderTargetViews[i], clearColor);
            }
        }
    }

    void DX11CommandList::ClearDepth(float depth)
    {
        if (!m_CurrentDepthStencilView)
        {
            return;
        }

        m_Context->ClearDepthStencilView(m_CurrentDepthStencilView, D3D11_CLEAR_DEPTH, depth, 0);
    }

    void DX11CommandList::SetViewport(const Viewport& viewport)
    {
        D3D11_VIEWPORT dxViewport{};
        dxViewport.TopLeftX = viewport.TopLeftX;
        dxViewport.TopLeftY = viewport.TopLeftY;
        dxViewport.Width = viewport.Width;
        dxViewport.Height = viewport.Height;
        dxViewport.MinDepth = viewport.MinDepth;
        dxViewport.MaxDepth = viewport.MaxDepth;
        m_Context->RSSetViewports(1, &dxViewport);

        // ラスタライザはScissorEnable=TRUE(DX11Device::CreatePipelineState)。
        // D3D11のシザー矩形の既定は「矩形0本」なので、有効なまま一度も張らないと
        // 全ピクセルがクリップされて何も映らなくなる。ここで必ずビューポート全体を張ることで、
        // SetScissorRectを使わない呼び出し側から見た挙動は従来と変わらない。
        // (D3D12もコマンドリストのリセット直後は矩形0本という同じ危険があり、
        //  DX12CommandList::SetViewportが同じ方法で塞いでいる)
        m_CurrentViewport = viewport;
        m_HasViewport = true;
        ApplyScissorRect(MakeFullViewportScissorRect(viewport));
    }

    void DX11CommandList::SetScissorRect(const ScissorRect& rect)
    {
        if (!m_HasViewport)
        {
            Core::Logger::Error(
                "DX11",
                "SetScissorRect: SetViewportより先に呼ばれました。クランプ先のビューポートが"
                "決まらないため、この呼び出しを無視します");
            return;
        }
        ApplyScissorRect(ClampScissorRectToViewport(rect, m_CurrentViewport));
    }

    void DX11CommandList::ResetScissorRect()
    {
        if (!m_HasViewport)
        {
            Core::Logger::Error("DX11", "ResetScissorRect: SetViewportより先に呼ばれました。この呼び出しを無視します");
            return;
        }
        ApplyScissorRect(MakeFullViewportScissorRect(m_CurrentViewport));
    }

    void DX11CommandList::ApplyScissorRect(const ScissorRect& rect)
    {
        D3D11_RECT dxRect{};
        dxRect.left = rect.Left;
        dxRect.top = rect.Top;
        dxRect.right = rect.Right;
        dxRect.bottom = rect.Bottom;
        m_Context->RSSetScissorRects(1, &dxRect);
    }

    void DX11CommandList::SetPipelineState(IRHIPipelineState* pipelineState)
    {
        auto* dx11PipelineState = static_cast<DX11PipelineState*>(pipelineState);

        m_Context->IASetInputLayout(dx11PipelineState->GetInputLayout());
        m_Context->IASetPrimitiveTopology(dx11PipelineState->GetTopology());
        m_Context->VSSetShader(dx11PipelineState->GetVertexShader()->GetVertexShader(), nullptr, 0);
        // ピクセルシェーダーを持たないパイプライン(深度プリパス)ではnullptrを張って
        // ピクセルシェーダー段を無効化する。前のパスのものが残ると深度だけを書くつもりが
        // レンダーターゲットへ書き込んでしまう
        DX11Shader* const dx11PixelShader = dx11PipelineState->GetPixelShader();
        m_Context->PSSetShader(dx11PixelShader ? dx11PixelShader->GetPixelShader() : nullptr, nullptr, 0);
        m_Context->OMSetDepthStencilState(dx11PipelineState->GetDepthStencilState(), 0);
        m_Context->OMSetBlendState(dx11PipelineState->GetBlendState(), nullptr, 0xFFFFFFFF);
        m_Context->RSSetState(dx11PipelineState->GetRasterizerState());
    }

    void DX11CommandList::SetVertexBuffer(IRHIBuffer* buffer)
    {
        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);
        ID3D11Buffer* buffers[] = { dx11Buffer->GetBuffer() };
        const uint32_t strides[] = { dx11Buffer->GetStride() };
        const uint32_t offsets[] = { 0 };
        m_Context->IASetVertexBuffers(0, 1, buffers, strides, offsets);
    }

    void DX11CommandList::SetIndexBuffer(IRHIBuffer* buffer)
    {
        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);
        m_Context->IASetIndexBuffer(dx11Buffer->GetBuffer(), DXGI_FORMAT_R32_UINT, 0);
    }

    void DX11CommandList::SetConstantBuffer(uint32_t slot, IRHIBuffer* buffer)
    {
        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);
        ID3D11Buffer* buffers[] = { dx11Buffer->GetBuffer() };
        m_Context->VSSetConstantBuffers(slot, 1, buffers);
        m_Context->PSSetConstantBuffers(slot, 1, buffers);
    }

    void DX11CommandList::SetTexture(uint32_t slot, IRHITexture* texture)
    {
        auto* dx11Texture = static_cast<DX11Texture*>(texture);
        ID3D11ShaderResourceView* srvs[] = { dx11Texture->GetShaderResourceView() };
        m_Context->PSSetShaderResources(slot, 1, srvs);
        if (slot < kTextureSlotCount)
        {
            m_BoundPixelSrvs[slot] = srvs[0];
        }
    }

    void DX11CommandList::SetSamplerSet(IRHISamplerSet* samplerSet)
    {
        if (!samplerSet)
        {
            Core::Logger::Error("DX11", "SetSamplerSet: サンプラーセットがnullptrのためバインドをスキップします");
            return;
        }

        auto* dx11SamplerSet = static_cast<DX11SamplerSet*>(samplerSet);
        m_Context->PSSetSamplers(0, dx11SamplerSet->GetCount(), dx11SamplerSet->GetSamplers());
    }

    void DX11CommandList::SetShaderResourceBuffer(uint32_t slot, IRHIBuffer* buffer)
    {
        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);
        ID3D11ShaderResourceView* srvs[] = { dx11Buffer->GetShaderResourceView() };
        m_Context->PSSetShaderResources(slot, 1, srvs);
        if (slot < kTextureSlotCount)
        {
            m_BoundPixelSrvs[slot] = srvs[0];
        }
    }

    void DX11CommandList::SetVertexShaderResourceBuffer(uint32_t slot, IRHIBuffer* buffer)
    {
        if (buffer == nullptr)
        {
            Core::Logger::Error(
                "DX11", "SetVertexShaderResourceBuffer: バッファがnullptrのためバインドをスキップします");
            return;
        }

        // 頂点シェーダのSRVスロットはピクセルシェーダのそれとは完全に独立しているため、
        // m_BoundPixelSrvs(SRV/UAVの同時バインド回避用のシャドウ)には記録しない。
        // このバッファはコンピュートがUAVで書くことがなく、UAVとの衝突が起こり得ないため
        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);
        ID3D11ShaderResourceView* srvs[] = { dx11Buffer->GetShaderResourceView() };
        m_Context->VSSetShaderResources(slot, 1, srvs);
    }

    // 指定リソースをピクセルシェーダのSRVスロットから外す。
    // DX11は同一リソースをSRVとUAVに同時バインドできず、そのまま両方バインドすると
    // ドライバが片方を黙って外して警告を出す。UAVでの書き込み直前にSRV側を明示的に外すことで、
    // 「どちらが外れるか」がドライバ任せにならないようにする。
    // Dispatch後のUAV解除(m_BoundComputeUavSlotMask)と対になる処理
    void DX11CommandList::UnbindPixelSrvForResource(ID3D11Resource* resource)
    {
        if (resource == nullptr)
        {
            return;
        }

        ID3D11ShaderResourceView* nullSrv[] = { nullptr };
        for (uint32_t slot = 0; slot < kTextureSlotCount; ++slot)
        {
            if (!m_BoundPixelSrvs[slot])
            {
                continue;
            }

            Microsoft::WRL::ComPtr<ID3D11Resource> boundResource;
            m_BoundPixelSrvs[slot]->GetResource(&boundResource);
            if (boundResource.Get() == resource)
            {
                m_Context->PSSetShaderResources(slot, 1, nullSrv);
                m_BoundPixelSrvs[slot].Reset();
            }
        }
    }

    void DX11CommandList::UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t sizeInBytes)
    {
        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);

        // BufferUsage::StructuredImmutable(D3D11_USAGE_IMMUTABLE)はMapもUpdateSubresourceも
        // 受け付けない(作成時の初期データから変えられない)。そのまま進めるとD3D11デバッグレイヤーの
        // エラーになるため、ここで弾く
        if (dx11Buffer->IsImmutable())
        {
            Core::Logger::Error(
                "DX11", "UpdateBuffer: BufferUsage::StructuredImmutableのバッファは更新できません。更新をスキップします");
            return;
        }

        // BufferUsage::StructuredReadOnly(D3D11_USAGE_DYNAMIC)はUpdateSubresourceが使えないため
        // Map(WRITE_DISCARD)経由で書き込む。有効なライト数ぶんだけ書けばよく、残りは未定義のままで
        // 構わない(シェーダ側はLightCount.xまでしかループしないため読まれない)
        if (dx11Buffer->IsDynamic())
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT hr = m_Context->Map(dx11Buffer->GetBuffer(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (FAILED(hr))
            {
                Core::Logger::Error("DX11", "動的バッファのMapに失敗しました");
                return;
            }
            std::memcpy(mapped.pData, data, sizeInBytes);
            m_Context->Unmap(dx11Buffer->GetBuffer(), 0);
            return;
        }

        (void)sizeInBytes;
        m_Context->UpdateSubresource(dx11Buffer->GetBuffer(), 0, nullptr, data, 0, 0);
    }

    void DX11CommandList::Draw(uint32_t vertexCount, uint32_t startVertexLocation)
    {
        m_Context->Draw(vertexCount, startVertexLocation);
    }

    void DX11CommandList::DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation)
    {
        m_Context->DrawIndexed(indexCount, startIndexLocation, baseVertexLocation);
    }

    void DX11CommandList::DispatchMesh(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ)
    {
        (void)threadGroupCountX;
        (void)threadGroupCountY;
        (void)threadGroupCountZ;
        // 上位層はIRHIDevice::SupportsMeshShader()を見て従来の頂点シェーダー描画へ
        // 分岐する設計のため、ここへ来るのは分岐漏れ
        Core::Logger::Error(
            "DX11", "DispatchMesh: DX11はメッシュシェーダーに対応していません。SupportsMeshShader()で分岐してください");
    }

    void DX11CommandList::SetComputePipelineState(IRHIPipelineState* pipelineState)
    {
        auto* dx11ComputePipelineState = static_cast<DX11ComputePipelineState*>(pipelineState);
        m_Context->CSSetShader(dx11ComputePipelineState->GetComputeShader()->GetComputeShader(), nullptr, 0);
    }

    void DX11CommandList::SetComputeConstantBuffer(uint32_t slot, IRHIBuffer* buffer)
    {
        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);
        ID3D11Buffer* buffers[] = { dx11Buffer->GetBuffer() };
        m_Context->CSSetConstantBuffers(slot, 1, buffers);
    }

    void DX11CommandList::SetComputeTexture(uint32_t slot, IRHITexture* texture)
    {
        auto* dx11Texture = static_cast<DX11Texture*>(texture);
        ID3D11ShaderResourceView* srvs[] = { dx11Texture->GetShaderResourceView() };
        m_Context->CSSetShaderResources(slot, 1, srvs);
    }

    void DX11CommandList::SetComputeShaderResourceBuffer(uint32_t slot, IRHIBuffer* buffer)
    {
        if (!buffer)
        {
            Core::Logger::Error("DX11", "SetComputeShaderResourceBuffer: バッファがnullptrのためバインドをスキップします");
            return;
        }

        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);
        ID3D11ShaderResourceView* srvs[] = { dx11Buffer->GetShaderResourceView() };
        if (srvs[0] == nullptr)
        {
            Core::Logger::Error(
                "DX11",
                "SetComputeShaderResourceBuffer: SRVを持たないバッファです"
                "(BufferUsage::StructuredReadOnly / StructuredRW で作成してください)。バインドをスキップします");
            return;
        }
        m_Context->CSSetShaderResources(slot, 1, srvs);
    }

    void DX11CommandList::SetComputeSamplerSet(IRHISamplerSet* samplerSet)
    {
        if (!samplerSet)
        {
            Core::Logger::Error("DX11", "SetComputeSamplerSet: サンプラーセットがnullptrのためバインドをスキップします");
            return;
        }

        auto* dx11SamplerSet = static_cast<DX11SamplerSet*>(samplerSet);
        m_Context->CSSetSamplers(0, dx11SamplerSet->GetCount(), dx11SamplerSet->GetSamplers());
    }

    void DX11CommandList::SetComputeUnorderedAccessTexture(uint32_t slot, IRHITexture* texture, uint32_t mipLevel)
    {
        auto* dx11Texture = static_cast<DX11Texture*>(texture);
        ID3D11UnorderedAccessView* uavs[] = { dx11Texture->GetUnorderedAccessView(mipLevel) };
        m_Context->CSSetUnorderedAccessViews(slot, 1, uavs, nullptr);
        m_BoundComputeUavSlotMask |= (1u << slot);
    }

    void DX11CommandList::SetComputeUnorderedAccessTextureCubeFace(
        uint32_t slot, IRHITexture* texture, uint32_t face, uint32_t mipLevel, uint32_t cubeIndex)
    {
        if (!texture)
        {
            Core::Logger::Error("DX11", "SetComputeUnorderedAccessTextureCubeFace: テクスチャがnullptrのためバインドをスキップします");
            return;
        }

        auto* dx11Texture = static_cast<DX11Texture*>(texture);
        // 範囲外指定の場合はDX11Texture側がログを出してnullptrを返す。ここでバインドを打ち切らず
        // そのまま渡すと以降のディスパッチが古いUAVを掴んだままになるため、明示的に中断する
        ID3D11UnorderedAccessView* uav = dx11Texture->GetCubeUnorderedAccessView(face, mipLevel, cubeIndex);
        if (!uav)
        {
            return;
        }

        ID3D11UnorderedAccessView* uavs[] = { uav };
        m_Context->CSSetUnorderedAccessViews(slot, 1, uavs, nullptr);
        m_BoundComputeUavSlotMask |= (1u << slot);
    }

    void DX11CommandList::SetComputeUnorderedAccessBuffer(uint32_t slot, IRHIBuffer* buffer)
    {
        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);
        // 前フレームにピクセルシェーダがSRVで読んだままになっている場合は先に外す
        // (BufferUsage::StructuredRWのライトグリッドが毎フレームこの経路を通る)
        UnbindPixelSrvForResource(dx11Buffer->GetBuffer());

        ID3D11UnorderedAccessView* uavs[] = { dx11Buffer->GetUnorderedAccessView() };
        m_Context->CSSetUnorderedAccessViews(slot, 1, uavs, nullptr);
        m_BoundComputeUavSlotMask |= (1u << slot);
    }

    void DX11CommandList::SetComputeAccelerationStructure(uint32_t slot, IRHIAccelerationStructure* accelerationStructure)
    {
        // DX11にはレイトレーシングAPIが無いため、そもそもTLASを作れない(DX11Device::CreateTopLevelASは
        // 常にnullptrを返す)。上位層がSupportsRaytracing()での分岐を忘れた場合にだけここへ来る
        (void)slot;
        (void)accelerationStructure;
        Core::Logger::Error(
            "DX11", "SetComputeAccelerationStructure: DX11はレイトレーシングに対応していません。バインドをスキップします");
    }

    void DX11CommandList::Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ)
    {
        m_Context->Dispatch(threadGroupCountX, threadGroupCountY, threadGroupCountZ);

        // バインドしたUAVはこのDispatchでのみ有効とし、直後に明示的に解放する(コメントはヘッダ側参照)
        if (m_BoundComputeUavSlotMask != 0)
        {
            ID3D11UnorderedAccessView* nullUavs[kComputeUavSlotCount] = {};
            m_Context->CSSetUnorderedAccessViews(0, kComputeUavSlotCount, nullUavs, nullptr);
            m_BoundComputeUavSlotMask = 0;
        }
    }
}
