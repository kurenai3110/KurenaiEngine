#include "DX11CommandList.h"

#include <cstring>
#include <utility>

#include "Core/Logger.h"

#include "DX11Buffer.h"
#include "DX11ComputePipelineState.h"
#include "DX11PipelineState.h"
#include "DX11Sampler.h"
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

    void DX11CommandList::SetRenderTargets(IRHITexture* const* targets, uint32_t count, IRHITexture* depthTexture)
    {
        count = count < kMaxRenderTargets ? count : kMaxRenderTargets;
        for (uint32_t i = 0; i < count; ++i)
        {
            m_CurrentRenderTargetViews[i] = static_cast<DX11Texture*>(targets[i])->GetRenderTargetView();
        }
        m_CurrentRenderTargetCount = count;
        m_CurrentDepthStencilView = depthTexture ? static_cast<DX11Texture*>(depthTexture)->GetDepthStencilView() : nullptr;
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
    }

    void DX11CommandList::SetPipelineState(IRHIPipelineState* pipelineState)
    {
        auto* dx11PipelineState = static_cast<DX11PipelineState*>(pipelineState);

        m_Context->IASetInputLayout(dx11PipelineState->GetInputLayout());
        m_Context->IASetPrimitiveTopology(dx11PipelineState->GetTopology());
        m_Context->VSSetShader(dx11PipelineState->GetVertexShader()->GetVertexShader(), nullptr, 0);
        m_Context->PSSetShader(dx11PipelineState->GetPixelShader()->GetPixelShader(), nullptr, 0);
        m_Context->OMSetDepthStencilState(dx11PipelineState->GetDepthStencilState(), 0);
        m_Context->OMSetBlendState(dx11PipelineState->GetBlendState(), nullptr, 0xFFFFFFFF);
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
    }

    void DX11CommandList::SetSampler(uint32_t slot, IRHISampler* sampler)
    {
        auto* dx11Sampler = static_cast<DX11Sampler*>(sampler);
        ID3D11SamplerState* samplers[] = { dx11Sampler->GetSamplerState() };
        m_Context->PSSetSamplers(slot, 1, samplers);
    }

    void DX11CommandList::SetShaderResourceBuffer(uint32_t slot, IRHIBuffer* buffer)
    {
        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);
        ID3D11ShaderResourceView* srvs[] = { dx11Buffer->GetShaderResourceView() };
        m_Context->PSSetShaderResources(slot, 1, srvs);
    }

    void DX11CommandList::UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t sizeInBytes)
    {
        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);

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

    void DX11CommandList::SetComputeSampler(uint32_t slot, IRHISampler* sampler)
    {
        auto* dx11Sampler = static_cast<DX11Sampler*>(sampler);
        ID3D11SamplerState* samplers[] = { dx11Sampler->GetSamplerState() };
        m_Context->CSSetSamplers(slot, 1, samplers);
    }

    void DX11CommandList::SetComputeUnorderedAccessTexture(uint32_t slot, IRHITexture* texture, uint32_t mipLevel)
    {
        auto* dx11Texture = static_cast<DX11Texture*>(texture);
        ID3D11UnorderedAccessView* uavs[] = { dx11Texture->GetUnorderedAccessView(mipLevel) };
        m_Context->CSSetUnorderedAccessViews(slot, 1, uavs, nullptr);
        m_BoundComputeUavSlotMask |= (1u << slot);
    }

    void DX11CommandList::SetComputeUnorderedAccessTextureCubeFace(uint32_t slot, IRHITexture* texture, uint32_t face, uint32_t mipLevel)
    {
        auto* dx11Texture = static_cast<DX11Texture*>(texture);
        ID3D11UnorderedAccessView* uavs[] = { dx11Texture->GetCubeUnorderedAccessView(face, mipLevel) };
        m_Context->CSSetUnorderedAccessViews(slot, 1, uavs, nullptr);
        m_BoundComputeUavSlotMask |= (1u << slot);
    }

    void DX11CommandList::SetComputeUnorderedAccessBuffer(uint32_t slot, IRHIBuffer* buffer)
    {
        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);
        ID3D11UnorderedAccessView* uavs[] = { dx11Buffer->GetUnorderedAccessView() };
        m_Context->CSSetUnorderedAccessViews(slot, 1, uavs, nullptr);
        m_BoundComputeUavSlotMask |= (1u << slot);
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
