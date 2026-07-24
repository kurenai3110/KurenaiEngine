#include "KurenaiEngine2D.h"

#include <Windows.h>

#include <cmath>

namespace Kurenai
{
    namespace
    {
        // 呼び出し元(exe)ではなくKurenaiEngine.dll自身のフォルダを返す。
        // Shaders/AssetsはDLLと同じフォルダに配置される運用のため、DLLがどこにコピー
        // されて使われても(各サンプルのBuildフォルダ配下など)データを正しく解決できる
        std::wstring GetModuleDirectory()
        {
            HMODULE module = nullptr;
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&GetModuleDirectory),
                &module);

            wchar_t path[MAX_PATH];
            GetModuleFileNameW(module, path, MAX_PATH);
            std::wstring pathStr(path);
            const size_t pos = pathStr.find_last_of(L"\\/");
            return pos == std::wstring::npos ? L"" : pathStr.substr(0, pos + 1);
        }

        struct Vertex2D
        {
            float Position[3];
            float UV[2];
        };

        // register(b0)のFrameConstantsとレイアウトを一致させる
        struct alignas(16) FrameConstants
        {
            DirectX::XMFLOAT4X4 ViewProj;
        };

        // register(b1)のObjectConstantsとレイアウトを一致させる
        struct alignas(16) ObjectConstants
        {
            DirectX::XMFLOAT4X4 World;
            DirectX::XMFLOAT4 Color;
        };
    }

    KurenaiEngine2D::KurenaiEngine2D(const std::wstring& title, uint32_t width, uint32_t height, GraphicsAPI api)
        : KurenaiEngineBase(title, width, height, api)
    {
        // ShadersはビルドでKurenaiEngine.dllと同じフォルダにコピーされる
        const std::wstring shaderPath = GetModuleDirectory() + L"Shaders\\Sprite2D.hlsl";

        m_VertexShader = m_Device->CreateShader({ RHI::ShaderStage::Vertex, shaderPath, "VSMain" });
        m_PixelShader = m_Device->CreateShader({ RHI::ShaderStage::Pixel, shaderPath, "PSMain" });

        RHI::PipelineStateDesc pipelineDesc;
        pipelineDesc.InputLayout = {
            { "POSITION", 0, RHI::Format::R32G32B32_Float, 0 },
            { "TEXCOORD", 0, RHI::Format::R32G32_Float, 12 },
        };
        pipelineDesc.VertexShader = m_VertexShader.get();
        pipelineDesc.PixelShader = m_PixelShader.get();
        pipelineDesc.RenderTargetFormats = { RHI::Format::R8G8B8A8_UNorm };
        pipelineDesc.HasDepthStencil = false; // 2Dは深度テスト不要(描画順で前後関係を決める)
        pipelineDesc.ReverseZ = false;
        pipelineDesc.BlendMode = RHI::BlendMode::AlphaBlend; // 半透明スプライトのため
        m_PipelineState = m_Device->CreatePipelineState(pipelineDesc);

        // DrawCircle用。頂点シェーダー・頂点レイアウトはスプライトと共用し、ピクセルシェーダーのみ
        // 円形マスク版(PSCircle)に差し替える
        m_CirclePixelShader = m_Device->CreateShader({ RHI::ShaderStage::Pixel, shaderPath, "PSCircle" });
        RHI::PipelineStateDesc circlePipelineDesc = pipelineDesc;
        circlePipelineDesc.PixelShader = m_CirclePixelShader.get();
        m_CirclePipelineState = m_Device->CreatePipelineState(circlePipelineDesc);

        // 原点中心の単位クアッド(-0.5〜0.5)。スプライトごとの位置/大きさ/回転はWorld行列側で表現する
        const Vertex2D quadVertices[] = {
            { { -0.5f, -0.5f, 0.0f }, { 0.0f, 1.0f } },
            { { -0.5f, 0.5f, 0.0f }, { 0.0f, 0.0f } },
            { { 0.5f, 0.5f, 0.0f }, { 1.0f, 0.0f } },
            { { 0.5f, -0.5f, 0.0f }, { 1.0f, 1.0f } },
        };
        const uint32_t quadIndices[] = { 0, 1, 2, 0, 2, 3 };

        RHI::BufferDesc vertexBufferDesc;
        vertexBufferDesc.Usage = RHI::BufferUsage::Vertex;
        vertexBufferDesc.SizeInBytes = sizeof(quadVertices);
        vertexBufferDesc.StrideInBytes = sizeof(Vertex2D);
        vertexBufferDesc.InitialData = quadVertices;
        m_QuadVertexBuffer = m_Device->CreateBuffer(vertexBufferDesc);

        RHI::BufferDesc indexBufferDesc;
        indexBufferDesc.Usage = RHI::BufferUsage::Index;
        indexBufferDesc.SizeInBytes = sizeof(quadIndices);
        indexBufferDesc.StrideInBytes = sizeof(uint32_t);
        indexBufferDesc.InitialData = quadIndices;
        m_QuadIndexBuffer = m_Device->CreateBuffer(indexBufferDesc);

        m_Sampler = m_Device->CreateDefaultSampler();

        RHI::BufferDesc frameConstantBufferDesc;
        frameConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        frameConstantBufferDesc.SizeInBytes = sizeof(FrameConstants);
        m_FrameConstantBuffer = m_Device->CreateBuffer(frameConstantBufferDesc);

        RHI::BufferDesc objectConstantBufferDesc;
        objectConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        objectConstantBufferDesc.SizeInBytes = sizeof(ObjectConstants);
        m_ObjectConstantBuffer = m_Device->CreateBuffer(objectConstantBufferDesc);

        m_WhiteTexture = CreateSolidColorTexture(255, 255, 255, 255); // DrawLineが使う
    }

    KurenaiEngine2D::~KurenaiEngine2D() = default;

    TextureHandle KurenaiEngine2D::LoadTexture(const std::wstring& filePath, bool sRGB)
    {
        auto texture = m_Device->CreateTextureFromFile(filePath, sRGB);
        RHI::IRHITexture* rawPtr = texture.get();
        m_Textures.push_back(std::move(texture));
        return TextureHandle(rawPtr);
    }

    TextureHandle KurenaiEngine2D::CreateSolidColorTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        auto texture = m_Device->CreateSolidColorTexture(r, g, b, a);
        RHI::IRHITexture* rawPtr = texture.get();
        m_Textures.push_back(std::move(texture));
        return TextureHandle(rawPtr);
    }

    void KurenaiEngine2D::BeginFrame(float clearR, float clearG, float clearB, float clearA)
    {
        const uint32_t width = GetWidth();
        const uint32_t height = GetHeight();
        if (width == 0 || height == 0)
        {
            return;
        }

        // 2Dで単純にウィンドウのピクセル座標をそのままワールド座標として使う(原点は画面左下、Y-up)。
        // 画面中央にカメラを置くことで、ワールド座標が0〜width/0〜heightの範囲を過不足なく映す
        m_Camera.SetOrthographic(static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f);
        m_Camera.SetPosition({ width * 0.5f, height * 0.5f, -1.0f });

        FrameConstants frameConstants{};
        const DirectX::XMMATRIX viewProj = m_Camera.GetViewMatrix() * m_Camera.GetProjectionMatrix();
        DirectX::XMStoreFloat4x4(&frameConstants.ViewProj, DirectX::XMMatrixTranspose(viewProj));

        RHI::IRHICommandList* commandList = GetCommandList();
        commandList->SetRenderTarget(m_SwapChain.get());
        commandList->ClearRenderTarget({ clearR, clearG, clearB, clearA });
        commandList->SetViewport({ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f });

        commandList->SetPipelineState(m_PipelineState.get());
        commandList->UpdateBuffer(m_FrameConstantBuffer.get(), &frameConstants, sizeof(frameConstants));
        commandList->SetConstantBuffer(0, m_FrameConstantBuffer.get());
        commandList->SetVertexBuffer(m_QuadVertexBuffer.get());
        commandList->SetIndexBuffer(m_QuadIndexBuffer.get());
        commandList->SetSampler(0, m_Sampler.get());
    }

    void KurenaiEngine2D::DrawSprite(
        float x, float y, float width, float height, float rotationRadians,
        TextureHandle texture, float r, float g, float b, float a)
    {
        if (!texture.IsValid())
        {
            return;
        }

        ObjectConstants objectConstants{};
        const DirectX::XMMATRIX world = DirectX::XMMatrixScaling(width, height, 1.0f) *
            DirectX::XMMatrixRotationZ(rotationRadians) *
            DirectX::XMMatrixTranslation(x, y, 0.0f);
        DirectX::XMStoreFloat4x4(&objectConstants.World, DirectX::XMMatrixTranspose(world));
        objectConstants.Color = { r, g, b, a };

        RHI::IRHICommandList* commandList = GetCommandList();
        commandList->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
        commandList->SetConstantBuffer(1, m_ObjectConstantBuffer.get());
        commandList->SetTexture(0, static_cast<RHI::IRHITexture*>(texture.m_Handle));
        commandList->DrawIndexed(6, 0, 0);
    }

    void KurenaiEngine2D::DrawCircle(float x, float y, float radius, float r, float g, float b, float a)
    {
        ObjectConstants objectConstants{};
        const DirectX::XMMATRIX world = DirectX::XMMatrixScaling(radius * 2.0f, radius * 2.0f, 1.0f) *
            DirectX::XMMatrixTranslation(x, y, 0.0f);
        DirectX::XMStoreFloat4x4(&objectConstants.World, DirectX::XMMatrixTranspose(world));
        objectConstants.Color = { r, g, b, a };

        RHI::IRHICommandList* commandList = GetCommandList();
        commandList->SetPipelineState(m_CirclePipelineState.get());
        commandList->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
        commandList->SetConstantBuffer(1, m_ObjectConstantBuffer.get());
        commandList->DrawIndexed(6, 0, 0);
        commandList->SetPipelineState(m_PipelineState.get()); // 以降のDrawSprite呼び出しのため戻す
    }

    void KurenaiEngine2D::DrawLine(float x1, float y1, float x2, float y2, float thickness, float r, float g, float b, float a)
    {
        const float dx = x2 - x1;
        const float dy = y2 - y1;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length <= 0.0f)
        {
            return;
        }

        const float angle = std::atan2(dy, dx);
        DrawSprite((x1 + x2) * 0.5f, (y1 + y2) * 0.5f, length, thickness, angle, m_WhiteTexture, r, g, b, a);
    }

    void KurenaiEngine2D::EndFrame(bool vsync)
    {
        m_SwapChain->Present(vsync);
    }
}
