#pragma once

#include <memory>
#include <string>

#include "IRHIBuffer.h"
#include "IRHICommandList.h"
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
        virtual std::unique_ptr<IRHITexture> CreateDepthTexture(uint32_t width, uint32_t height) = 0;
        virtual std::unique_ptr<IRHISampler> CreateDefaultSampler() = 0;
        virtual IRHICommandList* GetImmediateCommandList() = 0;
    };

    std::unique_ptr<IRHIDevice> CreateDX11Device();
}
