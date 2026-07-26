#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "RHI/IRHIDevice.h"

namespace Kurenai::RHI
{
    class DX11CommandList;

    class DX11Device : public IRHIDevice
    {
    public:
        DX11Device();
        ~DX11Device() override;

        void Initialize();

        std::unique_ptr<IRHISwapChain> CreateSwapChain(void* windowHandle, uint32_t width, uint32_t height) override;
        std::unique_ptr<IRHIBuffer> CreateBuffer(const BufferDesc& desc) override;
        std::unique_ptr<IRHIShader> CreateShader(const ShaderDesc& desc) override;
        std::unique_ptr<IRHIPipelineState> CreatePipelineState(const PipelineStateDesc& desc) override;
        std::unique_ptr<IRHIPipelineState> CreateComputePipelineState(const ComputePipelineStateDesc& desc) override;
        std::unique_ptr<IRHITexture> CreateTextureFromFile(const std::wstring& filePath, bool sRGB) override;
        std::unique_ptr<IRHITexture> CreateTextureFromImage(const TextureImage& image) override;
        std::unique_ptr<IRHITexture> CreateSolidColorTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a) override;
        std::unique_ptr<IRHITexture> CreateTextureFromMemory(uint32_t width, uint32_t height, const void* pixelsRGBA8) override;
        std::unique_ptr<IRHITexture> CreateRenderTexture(uint32_t width, uint32_t height, Format format) override;
        std::unique_ptr<IRHITexture> CreateUAVTexture(uint32_t width, uint32_t height, Format format) override;
        std::unique_ptr<IRHITexture> CreateHiZTexture(uint32_t width, uint32_t height, uint32_t mipLevels) override;
        std::unique_ptr<IRHITexture> CreateDepthTexture(uint32_t width, uint32_t height, float clearDepth = 1.0f) override;
        std::unique_ptr<IRHISampler> CreateDefaultSampler(const SamplerDesc& desc) override;
        IRHICommandList* GetImmediateCommandList() override;

        std::unique_ptr<IRHIImGuiBackend> CreateImGuiBackend(void* windowHandle) override;
        std::unique_ptr<IRHIGPUProfiler> CreateGPUProfiler() override;
        // DX11はDX12のようなフレームパイプライン化(フェンスによる多重バッファリング)を行っていないが、
        // 代わりにDX11SwapChain::Present()がブロッキング呼び出しの実測時間をここへ報告する
        // (vsync有効時、GPUの描画完了待ち+次のvblankまでの待ちがこの呼び出しに現れるため)
        float GetLastFrameGPUWaitTimeMs() const override { return m_LastFrameGPUWaitTimeMs; }

        // DX11SwapChain::Present()から、実測したPresent呼び出し時間を報告してもらうためのAPI
        // (IRHIDeviceの公開インタフェースではなく、DX11実装内部でのみ使う)
        void SetLastFrameGPUWaitTimeMs(float ms) { m_LastFrameGPUWaitTimeMs = ms; }

        // DX11はDX12のような多重バッファリングを行わず、ID3D11DeviceContextがリソースの
        // 使用状況を暗黙に追跡してくれるため、DX12ほど厳密なフェンス待ちは不要だが、
        // Flushで発行済みコマンドをGPUへ送り切ってから完了を待つことで、LoadScene等が
        // 直前まで参照されていたリソースを破棄する前に安全マージンを確保する
        void WaitForGPUIdle() override;

    private:
        Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;
        Microsoft::WRL::ComPtr<IDXGIFactory2> m_Factory;
        std::unique_ptr<DX11CommandList> m_ImmediateCommandList;
        float m_LastFrameGPUWaitTimeMs = 0.0f;
    };
}
