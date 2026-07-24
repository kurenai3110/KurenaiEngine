#pragma once

#include <cstdint>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "RHI/IRHISwapChain.h"

namespace Kurenai::RHI
{
    class DX12Device;

    class DX12SwapChain : public IRHISwapChain
    {
    public:
        DX12SwapChain(DX12Device* device, Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain, uint32_t width, uint32_t height);

        void Present(bool vsync) override;
        void Resize(uint32_t width, uint32_t height) override;

        D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRenderTargetView() const;
        D3D12_CPU_DESCRIPTOR_HANDLE GetDepthStencilView() const { return m_DsvHandle; }

        // Present用バックバッファのバリア遷移。SetRenderTarget(swapChain)時にRENDER_TARGETへ、
        // Present()内でPRESENTへ戻す
        void TransitionToRenderTarget(ID3D12GraphicsCommandList* commandList);

    private:
        static constexpr uint32_t kBufferCount = 2;

        void CreateRenderTargetViews();
        void CreateDepthStencilView();
        void ReleaseSizeDependentResources();
        void TransitionToPresent(ID3D12GraphicsCommandList* commandList);

        DX12Device* m_Device;
        Microsoft::WRL::ComPtr<IDXGISwapChain3> m_SwapChain;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_BackBuffers[kBufferCount];
        D3D12_RESOURCE_STATES m_BackBufferStates[kBufferCount]{};
        uint32_t m_RtvIndices[kBufferCount]{};
        Microsoft::WRL::ComPtr<ID3D12Resource> m_DepthStencilBuffer;
        uint32_t m_DsvIndex = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE m_DsvHandle{};
        uint32_t m_Width;
        uint32_t m_Height;
    };
}
