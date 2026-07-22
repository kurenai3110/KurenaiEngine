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
        explicit Application(uint32_t renderWidth = 1280, uint32_t renderHeight = 720);
        ~Application();

        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;

        void Run();

    private:
        void CreateSceneResources();
        void CreateRenderTargets(uint32_t width, uint32_t height);
        void LoadScene(size_t sceneIndex);
        void FrameCameraToModel();
        void UpdateSceneSwitch();
        void UpdateMouseLook();
        void UpdateMovement(float deltaTime);
        void Update(float deltaTime);
        void Render();
        DirectX::XMMATRIX ComputeLightViewProj(const DirectX::XMFLOAT3& lightDirection) const;

        std::unique_ptr<Window> m_Window;
        std::unique_ptr<RHI::IRHIDevice> m_Device;
        std::unique_ptr<RHI::IRHISwapChain> m_SwapChain;

        // G-Bufferの内部解像度。ウィンドウサイズとは独立しており、表示時はアスペクト比を保って拡大縮小する
        uint32_t m_RenderWidth;
        uint32_t m_RenderHeight;

        // ジオメトリパス(G-Buffer書き込み)
        std::unique_ptr<RHI::IRHIShader> m_GBufferVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_GBufferPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_GBufferPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_GBufferAlbedo;
        std::unique_ptr<RHI::IRHITexture> m_GBufferNormal;
        std::unique_ptr<RHI::IRHITexture> m_GBufferMaterial;
        std::unique_ptr<RHI::IRHITexture> m_GBufferDepth;

        // ライティングパス(G-Bufferを読みSceneColorへ出力。G-Bufferと同じレンダー解像度)
        std::unique_ptr<RHI::IRHIShader> m_LightingVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_LightingPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_LightingPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_SceneColor;

        // Presentパス(SceneColorをアスペクト比を保ってバックバッファへ拡大縮小表示)
        std::unique_ptr<RHI::IRHIShader> m_PresentVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_PresentPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_PresentPipelineState;

        // シャドウパス(平行光のライト視点から深度のみを描画する)
        static constexpr uint32_t kShadowMapSize = 2048;
        std::unique_ptr<RHI::IRHIShader> m_ShadowVertexShader;
        std::unique_ptr<RHI::IRHIShader> m_ShadowPixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_ShadowPipelineState;
        std::unique_ptr<RHI::IRHITexture> m_ShadowMap;

        // 背景(深度が書き込まれなかったピクセル)に表示する空のキューブマップ
        std::unique_ptr<RHI::IRHITexture> m_SkyboxTexture;

        std::unique_ptr<RHI::IRHISampler> m_Sampler;
        std::unique_ptr<RHI::IRHIBuffer> m_FrameConstantBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_MaterialConstantBuffer;
        Assets::Model m_Model;
        size_t m_CurrentSceneIndex = 0;
        bool m_DigitKeyWasDown[9] = {};

        Camera m_Camera;
        std::chrono::steady_clock::time_point m_LastFrameTime;

        bool m_MouseCaptured = false;
        POINT m_MouseCaptureCenter{};
    };
}
