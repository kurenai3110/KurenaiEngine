#pragma once

#include <Windows.h>

#include <chrono>
#include <memory>

#include "Assets/Model.h"
#include "Camera.h"
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
        void CreateSceneResources();
        void LoadScene(size_t sceneIndex);
        void FrameCameraToModel();
        void UpdateSceneSwitch();
        void UpdateMouseLook();
        void UpdateMovement(float deltaTime);
        void Update(float deltaTime);
        void Render();

        std::unique_ptr<Window> m_Window;
        std::unique_ptr<RHI::IRHIDevice> m_Device;
        std::unique_ptr<RHI::IRHISwapChain> m_SwapChain;

        std::unique_ptr<RHI::IRHIShader> m_VertexShader;
        std::unique_ptr<RHI::IRHIShader> m_PixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_PipelineState;
        std::unique_ptr<RHI::IRHISampler> m_Sampler;
        std::unique_ptr<RHI::IRHIBuffer> m_FrameConstantBuffer;
        Assets::Model m_Model;
        size_t m_CurrentSceneIndex = 0;
        bool m_DigitKeyWasDown[9] = {};

        Camera m_Camera;
        std::chrono::steady_clock::time_point m_LastFrameTime;

        bool m_MouseCaptured = false;
        POINT m_MouseCaptureCenter{};
    };
}
