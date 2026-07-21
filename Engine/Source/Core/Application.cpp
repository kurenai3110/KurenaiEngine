#include "Application.h"

#include <algorithm>
#include <cmath>

#include "Assets/ModelLoader.h"

namespace Kurenai::Core
{
    namespace
    {
        std::wstring GetExecutableDirectory()
        {
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring pathStr(path);
            size_t pos = pathStr.find_last_of(L"\\/");
            return pos == std::wstring::npos ? L"" : pathStr.substr(0, pos + 1);
        }

        struct alignas(16) FrameConstants
        {
            DirectX::XMFLOAT4X4 ViewProj;
            DirectX::XMFLOAT4X4 InvViewProj;
            DirectX::XMFLOAT4 CameraPosition;
            DirectX::XMFLOAT4 LightDirection;
            DirectX::XMFLOAT4 LightColor;
        };

        struct alignas(16) MaterialConstants
        {
            float MetallicFactor;
            float RoughnessFactor;
            float Padding[2];
        };

        struct SceneEntry
        {
            const wchar_t* DisplayName;
            const wchar_t* RelativePath;
        };

        const SceneEntry kScenes[] =
        {
            { L"Sponza", L"Assets\\Sponza\\Sponza.gltf" },
            { L"Bistro - Exterior", L"Assets\\Bistro\\BistroExterior.fbx" },
            { L"Bistro - Interior", L"Assets\\Bistro\\BistroInterior.fbx" },
            { L"Bistro - Interior (Wine Cellar)", L"Assets\\Bistro\\BistroInterior_Wine.fbx" },
        };
        constexpr size_t kSceneCount = sizeof(kScenes) / sizeof(kScenes[0]);

        // レンダー解像度(renderWidth x renderHeight)のアスペクト比を保ったまま、
        // windowWidth x windowHeight の中央に収まるビューポート(レターボックス/ピラーボックス)を求める
        RHI::Viewport ComputeLetterboxViewport(uint32_t windowWidth, uint32_t windowHeight, uint32_t renderWidth, uint32_t renderHeight)
        {
            const float windowAspect = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
            const float renderAspect = static_cast<float>(renderWidth) / static_cast<float>(renderHeight);

            float viewportWidth;
            float viewportHeight;
            if (windowAspect > renderAspect)
            {
                // ウィンドウの方が横長 -> 高さいっぱいに合わせ、左右に余白(ピラーボックス)
                viewportHeight = static_cast<float>(windowHeight);
                viewportWidth = viewportHeight * renderAspect;
            }
            else
            {
                // ウィンドウの方が縦長 -> 幅いっぱいに合わせ、上下に余白(レターボックス)
                viewportWidth = static_cast<float>(windowWidth);
                viewportHeight = viewportWidth / renderAspect;
            }

            RHI::Viewport viewport;
            viewport.TopLeftX = (static_cast<float>(windowWidth) - viewportWidth) * 0.5f;
            viewport.TopLeftY = (static_cast<float>(windowHeight) - viewportHeight) * 0.5f;
            viewport.Width = viewportWidth;
            viewport.Height = viewportHeight;
            return viewport;
        }
    }

    Application::Application(uint32_t renderWidth, uint32_t renderHeight)
        : m_RenderWidth(renderWidth)
        , m_RenderHeight(renderHeight)
    {
        m_Window = std::make_unique<Window>(L"Kurenai Engine", 1280, 720);
        m_Window->SetResizeCallback([this](uint32_t width, uint32_t height)
        {
            // G-Bufferは指定した内部解像度のまま固定し、表示側でアスペクト比を保って拡大縮小するため
            // ウィンドウリサイズではスワップチェインのみ更新する
            if (m_SwapChain)
            {
                m_SwapChain->Resize(width, height);
            }
        });

        m_Device = RHI::CreateDX11Device();
        m_SwapChain = m_Device->CreateSwapChain(m_Window->GetHandle(), m_Window->GetWidth(), m_Window->GetHeight());
        m_Camera.SetAspectRatio(static_cast<float>(m_RenderWidth) / static_cast<float>(m_RenderHeight));

        CreateSceneResources();

        m_LastFrameTime = std::chrono::steady_clock::now();
    }

    Application::~Application() = default;

    void Application::CreateSceneResources()
    {
        // Build/Bin/<Platform>/<Configuration>/ からリポジトリルートまでの相対パス
        const std::wstring repoRoot = GetExecutableDirectory() + L"..\\..\\..\\..\\";
        const std::wstring shaderDirectory = repoRoot + L"Sandbox\\Shaders\\";

        const std::vector<RHI::InputElementDesc> modelInputLayout =
        {
            { "POSITION", 0, RHI::Format::R32G32B32_Float, 0 },
            { "NORMAL", 0, RHI::Format::R32G32B32_Float, 12 },
            { "TEXCOORD", 0, RHI::Format::R32G32_Float, 24 },
        };

        // ジオメトリパス(G-Buffer書き込み)
        RHI::ShaderDesc gbufferVsDesc;
        gbufferVsDesc.Stage = RHI::ShaderStage::Vertex;
        gbufferVsDesc.FilePath = shaderDirectory + L"GBuffer.hlsl";
        gbufferVsDesc.EntryPoint = "VSMain";
        m_GBufferVertexShader = m_Device->CreateShader(gbufferVsDesc);

        RHI::ShaderDesc gbufferPsDesc;
        gbufferPsDesc.Stage = RHI::ShaderStage::Pixel;
        gbufferPsDesc.FilePath = shaderDirectory + L"GBuffer.hlsl";
        gbufferPsDesc.EntryPoint = "PSMain";
        m_GBufferPixelShader = m_Device->CreateShader(gbufferPsDesc);

        RHI::PipelineStateDesc gbufferPipelineDesc;
        gbufferPipelineDesc.InputLayout = modelInputLayout;
        gbufferPipelineDesc.VertexShader = m_GBufferVertexShader.get();
        gbufferPipelineDesc.PixelShader = m_GBufferPixelShader.get();
        gbufferPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        m_GBufferPipelineState = m_Device->CreatePipelineState(gbufferPipelineDesc);

        // ライティングパス(頂点バッファなしのフルスクリーン三角形)
        RHI::ShaderDesc lightingVsDesc;
        lightingVsDesc.Stage = RHI::ShaderStage::Vertex;
        lightingVsDesc.FilePath = shaderDirectory + L"DeferredLighting.hlsl";
        lightingVsDesc.EntryPoint = "VSMain";
        m_LightingVertexShader = m_Device->CreateShader(lightingVsDesc);

        RHI::ShaderDesc lightingPsDesc;
        lightingPsDesc.Stage = RHI::ShaderStage::Pixel;
        lightingPsDesc.FilePath = shaderDirectory + L"DeferredLighting.hlsl";
        lightingPsDesc.EntryPoint = "PSMain";
        m_LightingPixelShader = m_Device->CreateShader(lightingPsDesc);

        RHI::PipelineStateDesc lightingPipelineDesc;
        lightingPipelineDesc.VertexShader = m_LightingVertexShader.get();
        lightingPipelineDesc.PixelShader = m_LightingPixelShader.get();
        lightingPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        m_LightingPipelineState = m_Device->CreatePipelineState(lightingPipelineDesc);

        // Presentパス(頂点バッファなしのフルスクリーン三角形。SceneColorをバックバッファへ拡大縮小表示)
        RHI::ShaderDesc presentVsDesc;
        presentVsDesc.Stage = RHI::ShaderStage::Vertex;
        presentVsDesc.FilePath = shaderDirectory + L"Present.hlsl";
        presentVsDesc.EntryPoint = "VSMain";
        m_PresentVertexShader = m_Device->CreateShader(presentVsDesc);

        RHI::ShaderDesc presentPsDesc;
        presentPsDesc.Stage = RHI::ShaderStage::Pixel;
        presentPsDesc.FilePath = shaderDirectory + L"Present.hlsl";
        presentPsDesc.EntryPoint = "PSMain";
        m_PresentPixelShader = m_Device->CreateShader(presentPsDesc);

        RHI::PipelineStateDesc presentPipelineDesc;
        presentPipelineDesc.VertexShader = m_PresentVertexShader.get();
        presentPipelineDesc.PixelShader = m_PresentPixelShader.get();
        presentPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        m_PresentPipelineState = m_Device->CreatePipelineState(presentPipelineDesc);

        m_Sampler = m_Device->CreateDefaultSampler();

        RHI::BufferDesc constantBufferDesc;
        constantBufferDesc.Usage = RHI::BufferUsage::Constant;
        constantBufferDesc.SizeInBytes = sizeof(FrameConstants);
        m_FrameConstantBuffer = m_Device->CreateBuffer(constantBufferDesc);

        RHI::BufferDesc materialConstantBufferDesc;
        materialConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        materialConstantBufferDesc.SizeInBytes = sizeof(MaterialConstants);
        m_MaterialConstantBuffer = m_Device->CreateBuffer(materialConstantBufferDesc);

        CreateRenderTargets(m_RenderWidth, m_RenderHeight);

        LoadScene(0);
    }

    void Application::CreateRenderTargets(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
        {
            return;
        }

        m_GBufferAlbedo = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_GBufferNormal = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_GBufferMaterial = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
        m_GBufferDepth = m_Device->CreateDepthTexture(width, height);
        m_SceneColor = m_Device->CreateRenderTexture(width, height, RHI::Format::R8G8B8A8_UNorm);
    }

    void Application::LoadScene(size_t sceneIndex)
    {
        if (sceneIndex >= kSceneCount)
        {
            return;
        }

        const std::wstring repoRoot = GetExecutableDirectory() + L"..\\..\\..\\..\\";
        const std::wstring modelPath = repoRoot + kScenes[sceneIndex].RelativePath;

        m_Model = Assets::LoadModel(*m_Device, modelPath);
        m_CurrentSceneIndex = sceneIndex;

        FrameCameraToModel();

        m_Window->SetTitle(std::wstring(L"Kurenai Engine - ") + kScenes[sceneIndex].DisplayName);
    }

    void Application::FrameCameraToModel()
    {
        const float centerX = (m_Model.BoundsMin[0] + m_Model.BoundsMax[0]) * 0.5f;
        const float centerZ = (m_Model.BoundsMin[2] + m_Model.BoundsMax[2]) * 0.5f;
        const float sizeY = m_Model.BoundsMax[1] - m_Model.BoundsMin[1];
        const float dx = m_Model.BoundsMax[0] - m_Model.BoundsMin[0];
        const float dz = m_Model.BoundsMax[2] - m_Model.BoundsMin[2];
        const float diagonal = std::sqrt(dx * dx + sizeY * sizeY + dz * dz);
        const float eyeHeight = m_Model.BoundsMin[1] + sizeY * 0.15f;

        // ホールの長辺方向の端寄りから中心を見る位置を初期視点にする(中央の装飾物や壁に埋まらないように)
        float posX;
        float posZ;
        float yaw;
        if (dx >= dz)
        {
            posX = m_Model.BoundsMin[0] + dx * 0.2f;
            posZ = centerZ;
            yaw = DirectX::XM_PIDIV2;
        }
        else
        {
            posX = centerX;
            posZ = m_Model.BoundsMin[2] + dz * 0.2f;
            yaw = 0.0f;
        }

        m_Camera.SetPosition({ posX, eyeHeight, posZ });
        m_Camera.SetYawPitch(yaw, 0.0f);
        m_Camera.SetLens(DirectX::XM_PIDIV4, std::max(0.01f, diagonal * 0.0005f), std::max(100.0f, diagonal * 4.0f));
    }

    void Application::UpdateSceneSwitch()
    {
        const size_t count = kSceneCount < 9 ? kSceneCount : 9;
        for (size_t i = 0; i < count; ++i)
        {
            const bool isDown = (GetAsyncKeyState('1' + static_cast<int>(i)) & 0x8000) != 0;
            if (isDown && !m_DigitKeyWasDown[i] && i != m_CurrentSceneIndex)
            {
                LoadScene(i);
            }
            m_DigitKeyWasDown[i] = isDown;
        }
    }

    void Application::Run()
    {
        while (!m_Window->ShouldClose())
        {
            m_Window->PumpMessages();
            if (m_Window->ShouldClose())
            {
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            const float deltaTime = std::chrono::duration<float>(now - m_LastFrameTime).count();
            m_LastFrameTime = now;

            Update(deltaTime);
            Render();
        }
    }

    void Application::UpdateMouseLook()
    {
        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000)
        {
            if (!m_MouseCaptured)
            {
                m_MouseCaptured = true;
                ShowCursor(FALSE);

                RECT clientRect;
                GetClientRect(m_Window->GetHandle(), &clientRect);
                POINT center{ (clientRect.right - clientRect.left) / 2, (clientRect.bottom - clientRect.top) / 2 };
                ClientToScreen(m_Window->GetHandle(), &center);
                m_MouseCaptureCenter = center;
                SetCursorPos(center.x, center.y);
            }
            else
            {
                POINT currentPos;
                GetCursorPos(&currentPos);
                const float deltaX = static_cast<float>(currentPos.x - m_MouseCaptureCenter.x);
                const float deltaY = static_cast<float>(currentPos.y - m_MouseCaptureCenter.y);

                const float mouseSensitivity = 0.0025f;
                m_Camera.Rotate(deltaX * mouseSensitivity, -deltaY * mouseSensitivity);

                SetCursorPos(m_MouseCaptureCenter.x, m_MouseCaptureCenter.y);
            }
        }
        else if (m_MouseCaptured)
        {
            m_MouseCaptured = false;
            ShowCursor(TRUE);
        }
    }

    void Application::UpdateMovement(float deltaTime)
    {
        const float moveSpeed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 20.0f : 5.0f;
        const float moveAmount = moveSpeed * deltaTime;

        const DirectX::XMFLOAT3 forward = m_Camera.GetForward();
        const DirectX::XMFLOAT3 right = m_Camera.GetRight();

        DirectX::XMFLOAT3 move{ 0.0f, 0.0f, 0.0f };
        auto add = [&move](const DirectX::XMFLOAT3& v, float sign)
        {
            move.x += v.x * sign;
            move.y += v.y * sign;
            move.z += v.z * sign;
        };

        if (GetAsyncKeyState('W') & 0x8000) add(forward, 1.0f);
        if (GetAsyncKeyState('S') & 0x8000) add(forward, -1.0f);
        if (GetAsyncKeyState('D') & 0x8000) add(right, 1.0f);
        if (GetAsyncKeyState('A') & 0x8000) add(right, -1.0f);
        if (GetAsyncKeyState('E') & 0x8000) move.y += 1.0f;
        if (GetAsyncKeyState('Q') & 0x8000) move.y -= 1.0f;

        DirectX::XMVECTOR moveVec = DirectX::XMLoadFloat3(&move);
        if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(moveVec)) > 0.0001f)
        {
            moveVec = DirectX::XMVectorScale(DirectX::XMVector3Normalize(moveVec), moveAmount);
            DirectX::XMFLOAT3 delta;
            DirectX::XMStoreFloat3(&delta, moveVec);
            m_Camera.Move(delta);
        }
    }

    void Application::Update(float deltaTime)
    {
        UpdateSceneSwitch();
        UpdateMouseLook();
        UpdateMovement(deltaTime);
    }

    void Application::Render()
    {
        if (m_Window->GetWidth() == 0 || m_Window->GetHeight() == 0)
        {
            return;
        }

        auto* commandList = m_Device->GetImmediateCommandList();

        FrameConstants constants;
        const DirectX::XMMATRIX viewProj = m_Camera.GetViewMatrix() * m_Camera.GetProjectionMatrix();
        DirectX::XMStoreFloat4x4(&constants.ViewProj, DirectX::XMMatrixTranspose(viewProj));
        DirectX::XMVECTOR determinant;
        const DirectX::XMMATRIX invViewProj = DirectX::XMMatrixInverse(&determinant, viewProj);
        DirectX::XMStoreFloat4x4(&constants.InvViewProj, DirectX::XMMatrixTranspose(invViewProj));
        const DirectX::XMFLOAT3 cameraPosition = m_Camera.GetPosition();
        constants.CameraPosition = { cameraPosition.x, cameraPosition.y, cameraPosition.z, 0.0f };
        constants.LightDirection = { 0.4f, -0.8f, 0.3f, 0.0f };
        constants.LightColor = { 3.0f, 2.9f, 2.7f, 0.0f };
        commandList->UpdateBuffer(m_FrameConstantBuffer.get(), &constants, sizeof(constants));

        // --- ジオメトリパス: G-Bufferへ書き込む(常に指定した内部解像度) ---
        RHI::Viewport gbufferViewport;
        gbufferViewport.Width = static_cast<float>(m_RenderWidth);
        gbufferViewport.Height = static_cast<float>(m_RenderHeight);
        commandList->SetViewport(gbufferViewport);

        RHI::IRHITexture* gbufferTargets[] = { m_GBufferAlbedo.get(), m_GBufferNormal.get(), m_GBufferMaterial.get() };
        commandList->SetRenderTargets(gbufferTargets, 3, m_GBufferDepth.get());
        commandList->ClearRenderTarget({ 0.0f, 0.0f, 0.0f, 0.0f });
        commandList->ClearDepth(1.0f);

        commandList->SetPipelineState(m_GBufferPipelineState.get());
        commandList->SetConstantBuffer(0, m_FrameConstantBuffer.get());
        commandList->SetSampler(0, m_Sampler.get());

        for (const auto& mesh : m_Model.Meshes)
        {
            MaterialConstants materialConstants{};
            materialConstants.MetallicFactor = mesh.MetallicFactor;
            materialConstants.RoughnessFactor = mesh.RoughnessFactor;
            commandList->UpdateBuffer(m_MaterialConstantBuffer.get(), &materialConstants, sizeof(materialConstants));
            commandList->SetConstantBuffer(1, m_MaterialConstantBuffer.get());

            commandList->SetVertexBuffer(mesh.VertexBuffer.get());
            commandList->SetIndexBuffer(mesh.IndexBuffer.get());
            commandList->SetTexture(0, mesh.BaseColorTexture);
            commandList->SetTexture(1, mesh.NormalTexture);
            commandList->SetTexture(2, mesh.MetallicRoughnessTexture);
            commandList->DrawIndexed(mesh.IndexCount, 0, 0);
        }

        // --- ライティングパス: G-Bufferを読み、SceneColorへ出力(常に指定した内部解像度) ---
        RHI::IRHITexture* sceneColorTarget[] = { m_SceneColor.get() };
        commandList->SetRenderTargets(sceneColorTarget, 1, nullptr);
        commandList->SetViewport(gbufferViewport);
        // 深度テストに失敗した(=何も描かれていない)ピクセル用の背景色。discardされた箇所に前フレームのデータが
        // 残らないよう、フルスクリーン三角形を描く前に明示的にクリアしておく
        commandList->ClearRenderTarget({ 0.05f, 0.05f, 0.08f, 1.0f });

        commandList->SetPipelineState(m_LightingPipelineState.get());
        commandList->SetConstantBuffer(0, m_FrameConstantBuffer.get());
        commandList->SetSampler(0, m_Sampler.get());
        commandList->SetTexture(0, m_GBufferAlbedo.get());
        commandList->SetTexture(1, m_GBufferNormal.get());
        commandList->SetTexture(2, m_GBufferMaterial.get());
        commandList->SetTexture(3, m_GBufferDepth.get());
        commandList->Draw(3, 0);

        // --- Presentパス: SceneColorを、アスペクト比を保ってバックバッファへ出力 ---
        commandList->SetRenderTarget(m_SwapChain.get());
        commandList->ClearRenderTarget({ 0.05f, 0.05f, 0.08f, 1.0f });
        commandList->ClearDepth(1.0f);

        // レターボックス/ピラーボックスの余白もクリア色のまま残るよう、絞ったビューポートで描画する
        const RHI::Viewport letterboxViewport = ComputeLetterboxViewport(
            m_Window->GetWidth(), m_Window->GetHeight(), m_RenderWidth, m_RenderHeight);
        commandList->SetViewport(letterboxViewport);

        commandList->SetPipelineState(m_PresentPipelineState.get());
        commandList->SetSampler(0, m_Sampler.get());
        commandList->SetTexture(0, m_SceneColor.get());
        commandList->Draw(3, 0);

        m_SwapChain->Present(true);
    }
}
