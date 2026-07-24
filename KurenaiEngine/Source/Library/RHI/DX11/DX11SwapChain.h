#pragma once

#include <cstdint>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "RHI/IRHISwapChain.h"

namespace Kurenai::RHI
{
    class DX11SwapChain : public IRHISwapChain
    {
    public:
        DX11SwapChain(
            Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain,
            Microsoft::WRL::ComPtr<ID3D11Device> device,
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context,
            uint32_t width,
            uint32_t height);

        void Present(bool vsync) override;
        void Resize(uint32_t width, uint32_t height) override;

        ID3D11RenderTargetView* GetRenderTargetView() const { return m_RenderTargetView.Get(); }
        ID3D11DepthStencilView* GetDepthStencilView() const { return m_DepthStencilView.Get(); }
        uint32_t GetWidth() const { return m_Width; }
        uint32_t GetHeight() const { return m_Height; }

    private:
        void CreateRenderTargetView();
        void CreateDepthStencilView();

        Microsoft::WRL::ComPtr<IDXGISwapChain1> m_SwapChain;
        Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_RenderTargetView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_DepthStencilBuffer;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_DepthStencilView;
        uint32_t m_Width;
        uint32_t m_Height;
    };
}
