#pragma once

#include <memory>

#include "RHI/IRHIDevice.h"
#include "Window.h"

namespace Kurenai::Core
{
    class Application
    {
    public:
        Application();
        ~Application();

        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;

        void Run();

    private:
        void CreateTriangleResources();
        void Update();
        void Render();

        std::unique_ptr<Window> m_Window;
        std::unique_ptr<RHI::IRHIDevice> m_Device;
        std::unique_ptr<RHI::IRHISwapChain> m_SwapChain;

        std::unique_ptr<RHI::IRHIShader> m_VertexShader;
        std::unique_ptr<RHI::IRHIShader> m_PixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_PipelineState;
        std::unique_ptr<RHI::IRHIBuffer> m_VertexBuffer;
    };
}
