#include "DX11SwapChain.h"

#include <utility>

#include "DX11Util.h"

namespace Kurenai::RHI
{
    DX11SwapChain::DX11SwapChain(
        Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain,
        Microsoft::WRL::ComPtr<ID3D11Device> device,
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context,
        uint32_t width,
        uint32_t height)
        : m_SwapChain(std::move(swapChain))
        , m_Device(std::move(device))
        , m_Context(std::move(context))
        , m_Width(width)
        , m_Height(height)
    {
        CreateRenderTargetView();
    }

    void DX11SwapChain::CreateRenderTargetView()
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        ThrowIfFailed(m_SwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), "バックバッファの取得に失敗しました");
        ThrowIfFailed(m_Device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_RenderTargetView), "レンダーターゲットビューの作成に失敗しました");
    }

    void DX11SwapChain::Present(bool vsync)
    {
        m_SwapChain->Present(vsync ? 1 : 0, 0);
    }

    void DX11SwapChain::Resize(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0 || (width == m_Width && height == m_Height))
        {
            return;
        }

        m_RenderTargetView.Reset();
        m_Context->OMSetRenderTargets(0, nullptr, nullptr);

        DXGI_SWAP_CHAIN_DESC1 desc{};
        ThrowIfFailed(m_SwapChain->GetDesc1(&desc), "スワップチェイン情報の取得に失敗しました");
        ThrowIfFailed(m_SwapChain->ResizeBuffers(desc.BufferCount, width, height, desc.Format, desc.Flags), "スワップチェインのリサイズに失敗しました");

        m_Width = width;
        m_Height = height;
        CreateRenderTargetView();
    }
}
