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
            DirectX::XMFLOAT4 LightDirection;
            DirectX::XMFLOAT4 LightColor;
        };
    }

    Application::Application()
    {
        m_Window = std::make_unique<Window>(L"Kurenai Engine", 1280, 720);
        m_Window->SetResizeCallback([this](uint32_t width, uint32_t height)
        {
            if (m_SwapChain)
            {
                m_SwapChain->Resize(width, height);
            }
            if (height > 0)
            {
                m_Camera.SetAspectRatio(static_cast<float>(width) / static_cast<float>(height));
            }
        });

        m_Device = RHI::CreateDX11Device();
        m_SwapChain = m_Device->CreateSwapChain(m_Window->GetHandle(), m_Window->GetWidth(), m_Window->GetHeight());
        m_Camera.SetAspectRatio(static_cast<float>(m_Window->GetWidth()) / static_cast<float>(m_Window->GetHeight()));

        CreateSceneResources();

        m_LastFrameTime = std::chrono::steady_clock::now();
    }

    Application::~Application() = default;

    void Application::CreateSceneResources()
    {
        // Build/Bin/<Platform>/<Configuration>/ からリポジトリルートまでの相対パス
        const std::wstring repoRoot = GetExecutableDirectory() + L"..\\..\\..\\..\\";

        RHI::ShaderDesc vsDesc;
        vsDesc.Stage = RHI::ShaderStage::Vertex;
        vsDesc.FilePath = repoRoot + L"Sandbox\\Shaders\\Model.hlsl";
        vsDesc.EntryPoint = "VSMain";
        m_VertexShader = m_Device->CreateShader(vsDesc);

        RHI::ShaderDesc psDesc;
        psDesc.Stage = RHI::ShaderStage::Pixel;
        psDesc.FilePath = repoRoot + L"Sandbox\\Shaders\\Model.hlsl";
        psDesc.EntryPoint = "PSMain";
        m_PixelShader = m_Device->CreateShader(psDesc);

        RHI::PipelineStateDesc pipelineDesc;
        pipelineDesc.InputLayout =
        {
            { "POSITION", 0, RHI::Format::R32G32B32_Float, 0 },
            { "NORMAL", 0, RHI::Format::R32G32B32_Float, 12 },
            { "TEXCOORD", 0, RHI::Format::R32G32_Float, 24 },
        };
        pipelineDesc.VertexShader = m_VertexShader.get();
        pipelineDesc.PixelShader = m_PixelShader.get();
        pipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        m_PipelineState = m_Device->CreatePipelineState(pipelineDesc);

        m_Sampler = m_Device->CreateDefaultSampler();

        RHI::BufferDesc constantBufferDesc;
        constantBufferDesc.Usage = RHI::BufferUsage::Constant;
        constantBufferDesc.SizeInBytes = sizeof(FrameConstants);
        m_FrameConstantBuffer = m_Device->CreateBuffer(constantBufferDesc);

        const std::wstring modelPath = repoRoot + L"Assets\\Sponza\\Sponza.gltf";
        m_Model = Assets::LoadModel(*m_Device, modelPath);

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

        commandList->SetRenderTarget(m_SwapChain.get());

        RHI::Viewport viewport;
        viewport.Width = static_cast<float>(m_Window->GetWidth());
        viewport.Height = static_cast<float>(m_Window->GetHeight());
        commandList->SetViewport(viewport);

        commandList->ClearRenderTarget({ 0.05f, 0.05f, 0.08f, 1.0f });
        commandList->ClearDepth(1.0f);

        FrameConstants constants;
        const DirectX::XMMATRIX viewProj = m_Camera.GetViewMatrix() * m_Camera.GetProjectionMatrix();
        DirectX::XMStoreFloat4x4(&constants.ViewProj, DirectX::XMMatrixTranspose(viewProj));
        constants.LightDirection = { 0.4f, -0.8f, 0.3f, 0.0f };
        constants.LightColor = { 1.0f, 0.96f, 0.9f, 0.0f };
        commandList->UpdateBuffer(m_FrameConstantBuffer.get(), &constants, sizeof(constants));

        commandList->SetPipelineState(m_PipelineState.get());
        commandList->SetConstantBuffer(0, m_FrameConstantBuffer.get());
        commandList->SetSampler(0, m_Sampler.get());

        for (const auto& mesh : m_Model.Meshes)
        {
            commandList->SetVertexBuffer(mesh.VertexBuffer.get());
            commandList->SetIndexBuffer(mesh.IndexBuffer.get());
            commandList->SetTexture(0, mesh.BaseColorTexture);
            commandList->DrawIndexed(mesh.IndexCount, 0, 0);
        }

        m_SwapChain->Present(true);
    }
}
