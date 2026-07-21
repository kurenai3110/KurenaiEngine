#include "Application.h"

namespace Kurenai::Core
{
    namespace
    {
        struct Vertex
        {
            float Position[3];
            float Color[4];
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
        });

        m_Device = RHI::CreateDX11Device();
        m_SwapChain = m_Device->CreateSwapChain(m_Window->GetHandle(), m_Window->GetWidth(), m_Window->GetHeight());

        CreateTriangleResources();
    }

    Application::~Application() = default;

    void Application::CreateTriangleResources()
    {
        RHI::ShaderDesc vsDesc;
        vsDesc.Stage = RHI::ShaderStage::Vertex;
        vsDesc.FilePath = L"Shaders/Triangle.hlsl";
        vsDesc.EntryPoint = "VSMain";
        m_VertexShader = m_Device->CreateShader(vsDesc);

        RHI::ShaderDesc psDesc;
        psDesc.Stage = RHI::ShaderStage::Pixel;
        psDesc.FilePath = L"Shaders/Triangle.hlsl";
        psDesc.EntryPoint = "PSMain";
        m_PixelShader = m_Device->CreateShader(psDesc);

        RHI::PipelineStateDesc pipelineDesc;
        pipelineDesc.InputLayout =
        {
            { "POSITION", 0, RHI::Format::R32G32B32_Float, 0 },
            { "COLOR", 0, RHI::Format::R32G32B32A32_Float, 12 },
        };
        pipelineDesc.VertexShader = m_VertexShader.get();
        pipelineDesc.PixelShader = m_PixelShader.get();
        pipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        m_PipelineState = m_Device->CreatePipelineState(pipelineDesc);

        const Vertex vertices[] =
        {
            { {  0.0f,  0.5f, 0.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
            { {  0.5f, -0.5f, 0.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
            { { -0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } },
        };

        RHI::BufferDesc vertexBufferDesc;
        vertexBufferDesc.Usage = RHI::BufferUsage::Vertex;
        vertexBufferDesc.SizeInBytes = sizeof(vertices);
        vertexBufferDesc.StrideInBytes = sizeof(Vertex);
        vertexBufferDesc.InitialData = vertices;
        m_VertexBuffer = m_Device->CreateBuffer(vertexBufferDesc);
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

            Update();
            Render();
        }
    }

    void Application::Update()
    {
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

        commandList->ClearRenderTarget({ 0.1f, 0.1f, 0.15f, 1.0f });

        commandList->SetPipelineState(m_PipelineState.get());
        commandList->SetVertexBuffer(m_VertexBuffer.get());
        commandList->Draw(3, 0);

        m_SwapChain->Present(true);
    }
}
