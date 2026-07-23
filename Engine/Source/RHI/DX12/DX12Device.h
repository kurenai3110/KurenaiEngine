#pragma once

#include <cstdint>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <memory>
#include <wrl/client.h>

#include "DX12DescriptorHeap.h"
#include "RHI/IRHIDevice.h"

namespace DirectX
{
    struct TexMetadata;
    class ScratchImage;
}

namespace Kurenai::RHI
{
    class DX12CommandList;

    class DX12Device : public IRHIDevice
    {
    public:
        DX12Device();
        ~DX12Device() override;

        void Initialize();

        std::unique_ptr<IRHISwapChain> CreateSwapChain(void* windowHandle, uint32_t width, uint32_t height) override;
        std::unique_ptr<IRHIBuffer> CreateBuffer(const BufferDesc& desc) override;
        std::unique_ptr<IRHIShader> CreateShader(const ShaderDesc& desc) override;
        std::unique_ptr<IRHIPipelineState> CreatePipelineState(const PipelineStateDesc& desc) override;
        std::unique_ptr<IRHITexture> CreateTextureFromFile(const std::wstring& filePath, bool sRGB) override;
        std::unique_ptr<IRHITexture> CreateSolidColorTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a) override;
        std::unique_ptr<IRHITexture> CreateRenderTexture(uint32_t width, uint32_t height, Format format) override;
        std::unique_ptr<IRHITexture> CreateDepthTexture(uint32_t width, uint32_t height, float clearDepth = 1.0f) override;
        std::unique_ptr<IRHISampler> CreateDefaultSampler() override;
        IRHICommandList* GetImmediateCommandList() override;

        std::unique_ptr<IRHIImGuiBackend> CreateImGuiBackend(void* windowHandle) override;
        std::unique_ptr<IRHIGPUProfiler> CreateGPUProfiler() override;

        // DX12実装内部(DX12SwapChain/DX12Texture/DX12Sampler/DX12CommandList)から利用するアクセサ
        ID3D12Device* GetDevice() const { return m_Device.Get(); }
        ID3D12CommandQueue* GetCommandQueue() const { return m_CommandQueue.Get(); }
        ID3D12GraphicsCommandList* GetCommandList() const { return m_CommandList.Get(); }
        ID3D12RootSignature* GetRootSignature() const { return m_RootSignature.Get(); }
        DX12DescriptorHeap* GetRtvHeap() const { return m_RtvHeap.get(); }
        DX12DescriptorHeap* GetDsvHeap() const { return m_DsvHeap.get(); }
        DX12DescriptorHeap* GetSrvCpuHeap() const { return m_SrvCpuHeap.get(); }
        DX12DescriptorHeap* GetSamplerCpuHeap() const { return m_SamplerCpuHeap.get(); }
        DX12DescriptorHeap* GetShaderVisibleSrvHeap() const { return m_ShaderVisibleSrvHeap.get(); }
        DX12DescriptorHeap* GetShaderVisibleSamplerHeap() const { return m_ShaderVisibleSamplerHeap.get(); }

        // 1フレーム分のコマンドをすべて記録してから1回だけExecuteCommandListsする設計のため、
        // CopyDescriptorsSimpleによるディスクリプタ書き込みはGPU実行前にすべて完了してしまう。
        // そのため同じヒープスロットを毎回使い回すと、GPUが実際にDrawを処理する時点では
        // そのフレーム最後のSetTexture呼び出しの内容にすべて上書きされてしまう。
        // これを避けるため、t0〜t6の連続したkTextureSlotCount個のブロックを描画のたびに新規に払い出す
        uint32_t AllocateSrvTableBlock(uint32_t count);

        // コマンドリストを閉じてキューへ実行投入する
        void ExecuteCommandList();
        // フェンスでGPUの完了を待ち、次フレーム/次操作用にコマンドリストを開き直す
        void WaitForGPU();
        // フェンスでGPUの完了のみを待つ(コマンドリストの状態には触れない)。
        // 1フレーム分の記録を溜めて1回だけ実行する設計上、フレームの合間では
        // コマンドリストは常に開いた(記録可能な)状態になっているため、
        // ExecuteCommandList()を経ていない箇所(スワップチェインのリサイズ等)から
        // GPU完了を待つ場合はこちらを使い、開いたままのコマンドリストへ誤ってReset()しないようにする
        void WaitForGPUIdle();
        // ExecuteCommandList()とWaitForGPU()をまとめて行う、一度限りの同期処理(テクスチャアップロード等)用の便宜メソッド
        void SubmitAndWaitIdle();

    private:
        void CreateRootSignature();
        void ResetCommandList();
        Microsoft::WRL::ComPtr<ID3D12Resource> CreateUploadBuffer(uint64_t sizeInBytes);
        std::unique_ptr<IRHITexture> CreateTextureFromImage(const DirectX::TexMetadata& metadata, const DirectX::ScratchImage& image);

        Microsoft::WRL::ComPtr<ID3D12Device> m_Device;
        Microsoft::WRL::ComPtr<IDXGIFactory2> m_Factory;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_CommandQueue;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_CommandAllocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_CommandList;
        Microsoft::WRL::ComPtr<ID3D12Fence> m_Fence;
        uint64_t m_FenceValue = 0;
        HANDLE m_FenceEvent = nullptr;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_RootSignature;

        std::unique_ptr<DX12DescriptorHeap> m_RtvHeap;
        std::unique_ptr<DX12DescriptorHeap> m_DsvHeap;
        std::unique_ptr<DX12DescriptorHeap> m_SrvCpuHeap;
        std::unique_ptr<DX12DescriptorHeap> m_SamplerCpuHeap;
        std::unique_ptr<DX12DescriptorHeap> m_ShaderVisibleSrvHeap;
        std::unique_ptr<DX12DescriptorHeap> m_ShaderVisibleSamplerHeap;

        std::unique_ptr<DX12CommandList> m_ImmediateCommandList;

        uint32_t m_NextSrvTableIndex = 0;
    };
}
