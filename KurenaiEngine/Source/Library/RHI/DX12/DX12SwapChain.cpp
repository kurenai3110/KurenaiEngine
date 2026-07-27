#include "DX12SwapChain.h"

#include <utility>

#include "DX12Device.h"
#include "DX12Util.h"

namespace Kurenai::RHI
{
    DX12SwapChain::DX12SwapChain(DX12Device* device, Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain, uint32_t width, uint32_t height)
        : m_Device(device)
        , m_SwapChain(std::move(swapChain))
        , m_Width(width)
        , m_Height(height)
    {
        for (uint32_t i = 0; i < kBufferCount; ++i)
        {
            m_BackBufferStates[i] = D3D12_RESOURCE_STATE_PRESENT;
        }

        CreateRenderTargetViews();
        CreateDepthStencilView();
    }

    void DX12SwapChain::CreateRenderTargetViews()
    {
        auto* rtvHeap = m_Device->GetRtvHeap();
        for (uint32_t i = 0; i < kBufferCount; ++i)
        {
            ThrowIfFailed(m_SwapChain->GetBuffer(i, IID_PPV_ARGS(&m_BackBuffers[i])), "バックバッファの取得に失敗しました");
            m_RtvIndices[i] = rtvHeap->Allocate();
            m_Device->GetDevice()->CreateRenderTargetView(m_BackBuffers[i].Get(), nullptr, rtvHeap->GetCpuHandle(m_RtvIndices[i]));
            m_BackBufferStates[i] = D3D12_RESOURCE_STATE_PRESENT;
        }
    }

    void DX12SwapChain::CreateDepthStencilView()
    {
        // オフスクリーンの深度テクスチャ(CreateDepthTexture)と同じD32_FLOATに揃える。
        // ステンシルはエンジン全体で使っておらず、PSOのDSVFormat(HasDepthStencil指定時はD32_FLOAT)と
        // 実際にバインドされるDSVのフォーマットが一致していないとD3D12の仕様違反になるため
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 1.0f;
        clearValue.DepthStencil.Stencil = 0;

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC resourceDesc{};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resourceDesc.Width = m_Width;
        resourceDesc.Height = m_Height;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_D32_FLOAT;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        ThrowIfFailed(
            m_Device->GetDevice()->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(&m_DepthStencilBuffer)),
            "深度バッファの作成に失敗しました");

        m_DsvIndex = m_Device->GetDsvHeap()->Allocate();
        m_DsvHandle = m_Device->GetDsvHeap()->GetCpuHandle(m_DsvIndex);
        m_Device->GetDevice()->CreateDepthStencilView(m_DepthStencilBuffer.Get(), nullptr, m_DsvHandle);
    }

    void DX12SwapChain::ReleaseSizeDependentResources()
    {
        for (uint32_t i = 0; i < kBufferCount; ++i)
        {
            m_Device->GetRtvHeap()->Free(m_RtvIndices[i]);
            m_BackBuffers[i].Reset();
        }
        m_Device->GetDsvHeap()->Free(m_DsvIndex);
        m_DepthStencilBuffer.Reset();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12SwapChain::GetCurrentRenderTargetView() const
    {
        const uint32_t index = m_SwapChain->GetCurrentBackBufferIndex();
        return m_Device->GetRtvHeap()->GetCpuHandle(m_RtvIndices[index]);
    }

    void DX12SwapChain::TransitionToRenderTarget(ID3D12GraphicsCommandList* commandList)
    {
        const uint32_t index = m_SwapChain->GetCurrentBackBufferIndex();
        if (m_BackBufferStates[index] != D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = m_BackBuffers[index].Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = m_BackBufferStates[index];
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            commandList->ResourceBarrier(1, &barrier);
            m_BackBufferStates[index] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
    }

    void DX12SwapChain::TransitionToPresent(ID3D12GraphicsCommandList* commandList)
    {
        const uint32_t index = m_SwapChain->GetCurrentBackBufferIndex();
        if (m_BackBufferStates[index] != D3D12_RESOURCE_STATE_PRESENT)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = m_BackBuffers[index].Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = m_BackBufferStates[index];
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            commandList->ResourceBarrier(1, &barrier);
            m_BackBufferStates[index] = D3D12_RESOURCE_STATE_PRESENT;
        }
    }

    void DX12SwapChain::Present(bool vsync)
    {
        TransitionToPresent(m_Device->GetCommandList());
        m_Device->ExecuteCommandList();
        // このフレームの完了を示すフェンス値をシグナルしてから、CPUはGPUの完了を待たずに
        // 次のフレームスロットの記録へ進む(CPU/GPUがオーバーラップして動作する)
        m_Device->SignalFrame();
        ThrowIfFailed(m_SwapChain->Present(vsync ? 1 : 0, 0), "Presentに失敗しました");
        m_Device->AdvanceToNextFrame();
    }

    void DX12SwapChain::Resize(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0 || (width == m_Width && height == m_Height))
        {
            return;
        }

        // GPU上での旧バックバッファ/深度バッファの使用が完了していることを保証してから解放する。
        // この時点でコマンドリストは(前フレームのPresent後)開いたまま次の記録待ちの状態になっているため、
        // WaitForGPU()ではなくコマンドリストに触れないWaitForGPUIdle()を使う
        m_Device->WaitForGPUIdle();

        ReleaseSizeDependentResources();

        DXGI_SWAP_CHAIN_DESC1 desc{};
        ThrowIfFailed(m_SwapChain->GetDesc1(&desc), "スワップチェイン情報の取得に失敗しました");
        ThrowIfFailed(m_SwapChain->ResizeBuffers(desc.BufferCount, width, height, desc.Format, desc.Flags), "スワップチェインのリサイズに失敗しました");

        m_Width = width;
        m_Height = height;

        for (uint32_t i = 0; i < kBufferCount; ++i)
        {
            m_BackBufferStates[i] = D3D12_RESOURCE_STATE_PRESENT;
        }

        CreateRenderTargetViews();
        CreateDepthStencilView();
    }
}
