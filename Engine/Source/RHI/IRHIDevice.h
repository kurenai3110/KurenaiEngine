#pragma once

#include <memory>
#include <string>

#include "IRHIBuffer.h"
#include "IRHICommandList.h"
#include "IRHIGPUProfiler.h"
#include "IRHIImGuiBackend.h"
#include "IRHIPipelineState.h"
#include "IRHISampler.h"
#include "IRHIShader.h"
#include "IRHISwapChain.h"
#include "IRHITexture.h"
#include "RHIDesc.h"

namespace Kurenai::RHI
{
    class IRHIDevice
    {
    public:
        virtual ~IRHIDevice() = default;

        virtual std::unique_ptr<IRHISwapChain> CreateSwapChain(void* windowHandle, uint32_t width, uint32_t height) = 0;
        virtual std::unique_ptr<IRHIBuffer> CreateBuffer(const BufferDesc& desc) = 0;
        virtual std::unique_ptr<IRHIShader> CreateShader(const ShaderDesc& desc) = 0;
        virtual std::unique_ptr<IRHIPipelineState> CreatePipelineState(const PipelineStateDesc& desc) = 0;
        virtual std::unique_ptr<IRHITexture> CreateTextureFromFile(const std::wstring& filePath, bool sRGB) = 0;
        virtual std::unique_ptr<IRHITexture> CreateSolidColorTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a) = 0;
        virtual std::unique_ptr<IRHITexture> CreateRenderTexture(uint32_t width, uint32_t height, Format format) = 0;
        // clearDepth: このテクスチャの最適クリア値(DX12のD3D12_CLEAR_VALUE用)。実際のクリア値は
        // IRHICommandList::ClearDepthで毎回明示的に指定するが、DX12は生成時に宣言した値と
        // 一致しないと高速クリアパスが使えないため、Reverse-Zで0.0fクリアするテクスチャはここも合わせる
        virtual std::unique_ptr<IRHITexture> CreateDepthTexture(uint32_t width, uint32_t height, float clearDepth = 1.0f) = 0;
        virtual std::unique_ptr<IRHISampler> CreateDefaultSampler() = 0;
        virtual IRHICommandList* GetImmediateCommandList() = 0;

        // ImGui連携。ImGuiはバックエンド(DX11/DX12)ごとに専用の実装が必要なため、
        // このRHI抽象化層でも他のAPIと同様にバックエンド実装側(DX11Deviceなど)に委譲する
        virtual std::unique_ptr<IRHIImGuiBackend> CreateImGuiBackend(void* windowHandle) = 0;

        // GPUタイムスタンプクエリによる区間計測。DX11/DX12でクエリの仕組みが異なるため
        // バックエンド実装側(DX11Deviceなど)に委譲する
        virtual std::unique_ptr<IRHIGPUProfiler> CreateGPUProfiler() = 0;

        // 直前のPresent呼び出しでCPUがGPUの完了を待つのに費やした時間(ms)。
        // フレームパイプライン化(多重バッファリング)を行わないDX11では常に0を返す。
        // これは実際のCPU負荷ではなくGPU側の処理時間を反映した待ち時間なので、
        // CPU時間の表示からはこの値を差し引いて実質的なCPU負荷のみを示す
        virtual float GetLastFrameGPUWaitTimeMs() const = 0;
    };

    std::unique_ptr<IRHIDevice> CreateDX11Device();
    std::unique_ptr<IRHIDevice> CreateDX12Device();
}
