#include "DX11CommandList.h"

#include <utility>

#include "DX11Buffer.h"
#include "DX11PipelineState.h"
#include "DX11Shader.h"
#include "DX11SwapChain.h"

namespace Kurenai::RHI
{
    DX11CommandList::DX11CommandList(Microsoft::WRL::ComPtr<ID3D11DeviceContext> context)
        : m_Context(std::move(context))
    {
    }

    void DX11CommandList::SetRenderTarget(IRHISwapChain* swapChain)
    {
        auto* dx11SwapChain = static_cast<DX11SwapChain*>(swapChain);
        m_CurrentRenderTargetView = dx11SwapChain->GetRenderTargetView();
        m_Context->OMSetRenderTargets(1, &m_CurrentRenderTargetView, nullptr);
    }

    void DX11CommandList::ClearRenderTarget(const ClearColor& color)
    {
        if (!m_CurrentRenderTargetView)
        {
            return;
        }

        const float clearColor[4] = { color.R, color.G, color.B, color.A };
        m_Context->ClearRenderTargetView(m_CurrentRenderTargetView, clearColor);
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
    }

    void DX11CommandList::SetVertexBuffer(IRHIBuffer* buffer)
    {
        auto* dx11Buffer = static_cast<DX11Buffer*>(buffer);
        ID3D11Buffer* buffers[] = { dx11Buffer->GetBuffer() };
        const uint32_t strides[] = { dx11Buffer->GetStride() };
        const uint32_t offsets[] = { 0 };
        m_Context->IASetVertexBuffers(0, 1, buffers, strides, offsets);
    }

    void DX11CommandList::Draw(uint32_t vertexCount, uint32_t startVertexLocation)
    {
        m_Context->Draw(vertexCount, startVertexLocation);
    }
}
