#pragma once

#include <memory>

#include "IRHIBuffer.h"
#include "IRHICommandList.h"
#include "IRHIPipelineState.h"
#include "IRHIShader.h"
#include "IRHISwapChain.h"
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
        virtual IRHICommandList* GetImmediateCommandList() = 0;
    };

    std::unique_ptr<IRHIDevice> CreateDX11Device();
}
