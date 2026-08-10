#include "KurenaiEngine2D.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "Core/Logger.h"
#include "Core/StringUtil.h"

namespace Kurenai
{
    namespace
    {
        using Core::GetModuleDirectory;

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
            // xy=UVオフセット, zw=UVスケール。DrawText以外は恒等変換(0, 0, 1, 1)で使う
            DirectX::XMFLOAT4 UVOffsetScale = { 0.0f, 0.0f, 1.0f, 1.0f };
            // DrawRoundedRect専用。xy=半幅・半高さ(ピクセル), z=角丸半径(ピクセル), w=枠線太さ(ピクセル)。
            // それ以外の描画では未使用のため既定値のままでよい
            DirectX::XMFLOAT4 ShapeParams = { 0.0f, 0.0f, 0.0f, 0.0f };
            // DrawRoundedRect専用。枠線の色
            DirectX::XMFLOAT4 BorderColor = { 0.0f, 0.0f, 0.0f, 0.0f };
        };

        // --- DrawPolylineの上限 ---
        //
        // メモリは「1本あたりの最大頂点数 × 8バイト × (本数上限 × kFrameCount + 1)」で効く。
        // 下記の値では 9210 × 8B ≒ 72KiB、DX12のステージングは 72KiB × 65 ≒ 4.7MiB。
        // 用途に応じてこの2つの定数だけを触れば調整できる
        constexpr uint32_t kMaxPolylinePoints = 1024;
        // 全接合がベベルになる最悪ケース: セグメントごとに2三角形 + 内部頂点ごとに1三角形
        constexpr uint32_t kMaxPolylineVertices = (kMaxPolylinePoints - 1) * 6 + (kMaxPolylinePoints - 2) * 3;
        // 1フレームに描ける本数。DX12のステージングリング(BufferDesc::MaxUpdatesPerFrame)と同じ値にする
        constexpr uint32_t kMaxPolylinesPerFrame = 32;
        // マイター長がこの倍率(×半太さ)を超える鋭角ではベベルへ切り替える(SVGのstroke-miterlimit既定と同じ)
        constexpr float kPolylineMiterLimit = 4.0f;

        struct Float2
        {
            float X = 0.0f;
            float Y = 0.0f;
        };

        Float2 operator+(const Float2& a, const Float2& b) { return { a.X + b.X, a.Y + b.Y }; }
        Float2 operator-(const Float2& a, const Float2& b) { return { a.X - b.X, a.Y - b.Y }; }
        Float2 operator*(const Float2& a, float s) { return { a.X * s, a.Y * s }; }
        float Dot(const Float2& a, const Float2& b) { return a.X * b.X + a.Y * b.Y; }
        // 2Dの外積(符号付き面積の2倍)。正なら反時計回り(ワールドはY-up)
        float Cross(const Float2& a, const Float2& b) { return a.X * b.Y - a.Y * b.X; }
        float Length(const Float2& v) { return std::sqrt(v.X * v.X + v.Y * v.Y); }
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
        // ただしスワップチェインへ描くためDSV自体はバインドされる。DX12はPSOが申告した
        // DSVフォーマットと実際のDSVが一致している必要があるため、フォーマットの申告だけは行う
        pipelineDesc.DepthTargetAttached = true;
        pipelineDesc.ReverseZ = false;
        pipelineDesc.BlendMode = RHI::BlendMode::AlphaBlend; // 半透明スプライトのため
        m_PipelineState = m_Device->CreatePipelineState(pipelineDesc);

        // DrawCircle用。頂点シェーダー・頂点レイアウトはスプライトと共用し、ピクセルシェーダーのみ
        // 円形マスク版(PSCircle)に差し替える
        m_CirclePixelShader = m_Device->CreateShader({ RHI::ShaderStage::Pixel, shaderPath, "PSCircle" });
        RHI::PipelineStateDesc circlePipelineDesc = pipelineDesc;
        circlePipelineDesc.PixelShader = m_CirclePixelShader.get();
        m_CirclePipelineState = m_Device->CreatePipelineState(circlePipelineDesc);

        // DrawRoundedRect用。DrawCircleと同様、頂点シェーダー・頂点レイアウトはスプライトと共用し、
        // ピクセルシェーダーのみ角丸矩形マスク版(PSRoundedRect)に差し替える
        m_RoundedRectPixelShader = m_Device->CreateShader({ RHI::ShaderStage::Pixel, shaderPath, "PSRoundedRect" });
        RHI::PipelineStateDesc roundedRectPipelineDesc = pipelineDesc;
        roundedRectPipelineDesc.PixelShader = m_RoundedRectPixelShader.get();
        m_RoundedRectPipelineState = m_Device->CreatePipelineState(roundedRectPipelineDesc);

        // DrawPolyline用。頂点バッファを使わず頂点シェーダがSV_VertexIDで構造化バッファを引くため、
        // 入力レイアウトは空にする(DX11Device::CreatePipelineStateは空ならCreateInputLayoutを
        // 呼ばず、SetPipelineStateがIASetInputLayout(nullptr)を張るのでDX11でも成立する)
        const std::wstring polylineShaderPath = GetModuleDirectory() + L"Shaders\\Polyline2D.hlsl";
        m_PolylineVertexShader = m_Device->CreateShader({ RHI::ShaderStage::Vertex, polylineShaderPath, "VSPolyline" });
        m_PolylinePixelShader = m_Device->CreateShader({ RHI::ShaderStage::Pixel, polylineShaderPath, "PSPolyline" });
        RHI::PipelineStateDesc polylinePipelineDesc = pipelineDesc;
        polylinePipelineDesc.InputLayout.clear();
        polylinePipelineDesc.VertexShader = m_PolylineVertexShader.get();
        polylinePipelineDesc.PixelShader = m_PolylinePixelShader.get();
        m_PolylinePipelineState = m_Device->CreatePipelineState(polylinePipelineDesc);

        RHI::BufferDesc polylineBufferDesc;
        polylineBufferDesc.Usage = RHI::BufferUsage::StructuredReadOnly;
        polylineBufferDesc.SizeInBytes = kMaxPolylineVertices * sizeof(PolylineVertex);
        polylineBufferDesc.StrideInBytes = sizeof(PolylineVertex);
        // 1フレームに描ける本数ぶんのステージングリングを確保させる(既定の4本では足りない)
        polylineBufferDesc.MaxUpdatesPerFrame = kMaxPolylinesPerFrame;
        m_PolylineVertexBuffer = m_Device->CreateBuffer(polylineBufferDesc);
        m_PolylineVertices.reserve(kMaxPolylineVertices);

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

        // スプライト用。UVOffsetScaleでアトラスを切り出す用途と、タイリングするスプライトの
        // 両方に対応するためWrap。拡大縮小したスプライトのボケを抑えるため異方性16x
        RHI::SamplerDesc spriteSampler{};
        spriteSampler.Filter = RHI::SamplerFilter::Anisotropic;
        spriteSampler.AddressMode = RHI::SamplerAddressMode::Wrap;
        m_SamplerSet = m_Device->CreateSamplerSet(&spriteSampler, 1);

        RHI::BufferDesc frameConstantBufferDesc;
        frameConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        frameConstantBufferDesc.SizeInBytes = sizeof(FrameConstants);
        m_FrameConstantBuffer = m_Device->CreateBuffer(frameConstantBufferDesc);

        RHI::BufferDesc objectConstantBufferDesc;
        objectConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        objectConstantBufferDesc.SizeInBytes = sizeof(ObjectConstants);
        m_ObjectConstantBuffer = m_Device->CreateBuffer(objectConstantBufferDesc);

        m_WhiteTexture = CreateSolidColorTexture(255, 255, 255, 255); // DrawLineが使う

        // BuildFontAtlasはコンストラクタで(BeginFrame/Drawの前に)呼ぶ必要がある。DX12の
        // CreateTextureFromMemoryは内部でSubmitAndWaitIdle(コマンドリストのフラッシュ+リセット)を
        // 伴うため、BeginFrame後のフレーム中に呼ぶとレンダーターゲット/パイプラインステート等の
        // 設定済み状態が失われてしまう(DrawText初回呼び出し時の遅延生成にしたところクラッシュした)。
        // 以後DrawTextで未収録の文字が見つかった場合も、同じ理由でBeginFrame()の先頭でのみ追加する
        BuildFontAtlas(DefaultAsciiChars(), false);
        BuildFontAtlas(DefaultAsciiChars(), true);
    }

    KurenaiEngine2D::~KurenaiEngine2D()
    {
        // このクラスが持つGPUリソース(パイプラインステート・頂点/インデックスバッファ・
        // 定数バッファ・テクスチャ・フォントアトラス)を1つも壊す前に、GPUの実行完了を待つ。
        // 基底のKurenaiEngineBaseも待つが、そちらが走るのはこのクラスのメンバが
        // すべて破棄された後なので間に合わない(WaitForGPUIdleの宣言側コメント参照)
        WaitForGPUIdle();
    }

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
        // WM_SIZEが記録しておいたリサイズ要求をここで反映する。2Dはレンダースレッドを持たず
        // このスレッドがそのままスワップチェーンを使うため、描画コマンドを何も積んでいない
        // フレーム先頭のこの位置で呼べばよい(ApplyPendingResizeのコメント参照)
        ApplyPendingResize();

        // 前フレームのDrawText/MeasureTextで未収録の文字が見つかっていれば、まだ描画コマンドを
        // 何も積んでいないこのタイミングでアトラスを再構築する(BuildFontAtlasのコメント参照)
        RebuildFontAtlasIfPending(false);
        RebuildFontAtlasIfPending(true);

        const uint32_t width = GetWidth();
        const uint32_t height = GetHeight();
        if (width == 0 || height == 0)
        {
            return;
        }

        // 2Dはウィンドウのピクセル座標をそのままワールド座標として使う(原点は画面左下、Y-up)。
        // 実効的なカメラ中心・見える範囲・ビューポートはComputeViewStateが一元的に決める
        // (SetCameraPosition/SetCameraZoom/SetVirtualResolutionを一度も呼ばなければ、
        //  論理解像度=クライアント領域・ズーム1.0・カメラ中心=画面中央になり従来と同じ絵になる)
        const ViewState view = ComputeViewState();

        // ズームは「見える範囲を1/zoomへ狭める」ことで表現する。Core::Cameraは見える範囲を
        // ワールド単位で受け取る設計(Camera::SetOrthographic)なので、3D側と共有している
        // Core::Cameraにズーム専用のAPIを足す必要はない
        m_Camera.SetOrthographic(view.LogicalWidth / view.Zoom, view.LogicalHeight / view.Zoom, 0.0f, 1.0f);
        m_Camera.SetPosition({ view.CameraCenterX, view.CameraCenterY, -1.0f });

        FrameConstants frameConstants{};
        const DirectX::XMMATRIX viewProj = m_Camera.GetViewMatrix() * m_Camera.GetProjectionMatrix();
        DirectX::XMStoreFloat4x4(&frameConstants.ViewProj, DirectX::XMMatrixTranspose(viewProj));

        RHI::IRHICommandList* commandList = GetCommandList();
        commandList->SetRenderTarget(m_SwapChain.get());
        // ClearRenderTargetはビューポートに関係なくバックバッファ全体を塗るため、
        // SetVirtualResolution使用時のレターボックスの余白もこのクリア色になる
        // (黒帯にしたい場合はBeginFrame(0, 0, 0)を渡す)
        commandList->ClearRenderTarget({ clearR, clearG, clearB, clearA });
        commandList->SetViewport({ view.ViewportX, view.ViewportY, view.ViewportWidth, view.ViewportHeight, 0.0f, 1.0f });

        // SetViewportがシザー矩形をビューポート全体へ戻すので、前フレームに積み残された
        // クリップ矩形もここで捨てる(EndFrameが警告済み)。これによりPush/Popの数が
        // 合っていないアプリでも、クリップがフレームをまたいで残り続けることはない
        m_ClipRectStack.clear();

        // DrawPolylineの本数カウンタも1フレームぶん(DX12のステージングリングの寿命と同じ)
        m_PolylineDrawsThisFrame = 0;
        m_PolylineOverflowLogged = false;

        commandList->SetPipelineState(m_PipelineState.get());
        commandList->UpdateBuffer(m_FrameConstantBuffer.get(), &frameConstants, sizeof(frameConstants));
        commandList->SetConstantBuffer(0, m_FrameConstantBuffer.get());
        commandList->SetVertexBuffer(m_QuadVertexBuffer.get());
        commandList->SetIndexBuffer(m_QuadIndexBuffer.get());
        commandList->SetSamplerSet(m_SamplerSet.get());
    }

    KurenaiEngine2D::ViewState KurenaiEngine2D::ComputeViewState() const
    {
        const float clientWidth = static_cast<float>(GetWidth());
        const float clientHeight = static_cast<float>(GetHeight());

        ViewState state;
        state.Zoom = m_CameraZoom;

        if (m_HasVirtualResolution && clientWidth > 0.0f && clientHeight > 0.0f)
        {
            // 論理解像度が指定されている場合だけ、アスペクト比を保ったままクライアント領域の
            // 中央へ収める(レターボックス/ピラーボックス)。見えるワールドの範囲は
            // 論理解像度で決まるため、ウィンドウサイズに依存しなくなる
            state.LogicalWidth = m_VirtualWidth;
            state.LogicalHeight = m_VirtualHeight;
            const float scale = (std::min)(clientWidth / m_VirtualWidth, clientHeight / m_VirtualHeight);
            state.ViewportWidth = m_VirtualWidth * scale;
            state.ViewportHeight = m_VirtualHeight * scale;
            state.ViewportX = (clientWidth - state.ViewportWidth) * 0.5f;
            state.ViewportY = (clientHeight - state.ViewportHeight) * 0.5f;
        }
        else
        {
            // 既定。クライアント領域をそのまま論理解像度として使う(従来の挙動)
            state.LogicalWidth = clientWidth;
            state.LogicalHeight = clientHeight;
            state.ViewportWidth = clientWidth;
            state.ViewportHeight = clientHeight;
        }

        // SetCameraPositionを一度も呼んでいなければ論理解像度の中央。これにより
        // ワールド座標0〜LogicalWidth / 0〜LogicalHeightが過不足なく映る(従来の挙動)
        state.CameraCenterX = m_HasCameraPosition ? m_CameraX : state.LogicalWidth * 0.5f;
        state.CameraCenterY = m_HasCameraPosition ? m_CameraY : state.LogicalHeight * 0.5f;
        return state;
    }

    void KurenaiEngine2D::SetCameraPosition(float x, float y)
    {
        m_CameraX = x;
        m_CameraY = y;
        m_HasCameraPosition = true; // 以後はウィンドウサイズへの自動追従をやめ、この値で固定する
    }

    void KurenaiEngine2D::GetCameraPosition(float& outX, float& outY) const
    {
        // 未設定の場合も「実際に使われている中心」を返す(BeginFrameと同じ計算を通す)
        const ViewState view = ComputeViewState();
        outX = view.CameraCenterX;
        outY = view.CameraCenterY;
    }

    void KurenaiEngine2D::SetCameraZoom(float zoom)
    {
        // 0以下は0除算で投影行列が壊れ、NaNは画面全体が消える。
        // !(zoom > 0.0f)と書くことでNaNも同時に弾ける
        if (!(zoom > 0.0f))
        {
            Core::Logger::Error(
                "2D",
                "SetCameraZoom: ズーム倍率は0より大きい必要があります(指定値: " +
                    std::to_string(zoom) + ")。この呼び出しを無視します");
            return;
        }
        m_CameraZoom = zoom;
    }

    void KurenaiEngine2D::SetVirtualResolution(float width, float height)
    {
        // (0, 0)は「論理解像度の解除」= クライアント領域をそのまま使う既定へ戻す、と定義する
        if (width == 0.0f && height == 0.0f)
        {
            m_HasVirtualResolution = false;
            return;
        }

        if (!(width > 0.0f) || !(height > 0.0f))
        {
            Core::Logger::Error(
                "2D",
                "SetVirtualResolution: 幅・高さは0より大きい必要があります(指定値: " +
                    std::to_string(width) + "x" + std::to_string(height) +
                    ")。解除したい場合は(0, 0)を渡してください。この呼び出しを無視します");
            return;
        }

        m_VirtualWidth = width;
        m_VirtualHeight = height;
        m_HasVirtualResolution = true;
    }

    void KurenaiEngine2D::ClientToWorld(float clientX, float clientY, float& outWorldX, float& outWorldY) const
    {
        const ViewState view = ComputeViewState();
        if (view.ViewportWidth <= 0.0f || view.ViewportHeight <= 0.0f)
        {
            // 最小化直後などクライアント領域が0のフレーム。0除算でNaNを外へ出さないよう
            // カメラ中心を返す(BeginFrameもこの状態では何も描かずに戻るため実害はない)
            outWorldX = view.CameraCenterX;
            outWorldY = view.CameraCenterY;
            return;
        }

        // ビューポート内の正規化位置(0〜1)を経由する。クライアント座標はY-down、
        // ワールドはY-upなのでvだけ符号が反転する
        const float u = (clientX - view.ViewportX) / view.ViewportWidth;
        const float v = (clientY - view.ViewportY) / view.ViewportHeight;
        outWorldX = view.CameraCenterX + (u - 0.5f) * (view.LogicalWidth / view.Zoom);
        outWorldY = view.CameraCenterY - (v - 0.5f) * (view.LogicalHeight / view.Zoom);
    }

    void KurenaiEngine2D::WorldToClient(float worldX, float worldY, float& outClientX, float& outClientY) const
    {
        const ViewState view = ComputeViewState();
        const float visibleWidth = view.LogicalWidth / view.Zoom;
        const float visibleHeight = view.LogicalHeight / view.Zoom;
        if (visibleWidth <= 0.0f || visibleHeight <= 0.0f)
        {
            // ClientToWorldと同じ理由のガード
            outClientX = 0.0f;
            outClientY = 0.0f;
            return;
        }

        const float u = (worldX - view.CameraCenterX) / visibleWidth + 0.5f;
        const float v = 0.5f - (worldY - view.CameraCenterY) / visibleHeight;
        outClientX = view.ViewportX + u * view.ViewportWidth;
        outClientY = view.ViewportY + v * view.ViewportHeight;
    }

    void KurenaiEngine2D::GetMouseWorldPosition(float& outWorldX, float& outWorldY) const
    {
        const POINT mouse = GetClientMousePosition();
        ClientToWorld(static_cast<float>(mouse.x), static_cast<float>(mouse.y), outWorldX, outWorldY);
    }

    void KurenaiEngine2D::DrawSprite(
        float x, float y, float width, float height, float rotationRadians,
        TextureHandle texture, float r, float g, float b, float a)
    {
        // テクスチャ全体を描くのは「UV矩形が(0, 0)-(1, 1)」の特別な場合なので、実装は1本にまとめる
        DrawSpriteUV(x, y, width, height, rotationRadians, texture, 0.0f, 0.0f, 1.0f, 1.0f, r, g, b, a);
    }

    void KurenaiEngine2D::DrawSpriteUV(
        float x, float y, float width, float height, float rotationRadians,
        TextureHandle texture, float srcU0, float srcV0, float srcU1, float srcV1,
        float r, float g, float b, float a)
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
        // 頂点シェーダーが UVOffsetScale.xy + UV * UVOffsetScale.zw で変換するため、
        // オフセットと大きさの形で積む(DrawTextがフォントアトラスを切り出すのと同じ仕組み)
        objectConstants.UVOffsetScale = { srcU0, srcV0, srcU1 - srcU0, srcV1 - srcV0 };

        RHI::IRHICommandList* commandList = GetCommandList();
        commandList->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
        commandList->SetConstantBuffer(1, m_ObjectConstantBuffer.get());
        commandList->SetTexture(0, static_cast<RHI::IRHITexture*>(texture.m_Handle));
        commandList->DrawIndexed(6, 0, 0);
    }

    void KurenaiEngine2D::GetTextureSize(TextureHandle texture, uint32_t& outWidth, uint32_t& outHeight) const
    {
        outWidth = 0;
        outHeight = 0;
        if (!texture.IsValid())
        {
            Core::Logger::Error("2D", "GetTextureSize: 無効なテクスチャハンドルが渡されました。0を返します");
            return;
        }

        const auto* rhiTexture = static_cast<const RHI::IRHITexture*>(texture.m_Handle);
        outWidth = rhiTexture->GetWidth();
        outHeight = rhiTexture->GetHeight();
    }

    void KurenaiEngine2D::DrawCircle(
        float x, float y, float radius,
        float r, float g, float b, float a,
        float borderThicknessPixels,
        float borderR, float borderG, float borderB, float borderA)
    {
        ObjectConstants objectConstants{};
        const DirectX::XMMATRIX world = DirectX::XMMatrixScaling(radius * 2.0f, radius * 2.0f, 1.0f) *
            DirectX::XMMatrixTranslation(x, y, 0.0f);
        DirectX::XMStoreFloat4x4(&objectConstants.World, DirectX::XMMatrixTranspose(world));
        objectConstants.Color = { r, g, b, a };
        // PSCircleが枠線の太さをピクセルで扱えるよう、半径(ピクセル)を渡す。
        // レイアウトはDrawRoundedRectと共通で、xyは半幅・半高さ(円なのでどちらも半径)
        objectConstants.ShapeParams = { radius, radius, radius, borderThicknessPixels };
        objectConstants.BorderColor = { borderR, borderG, borderB, borderA };

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

    uint32_t KurenaiEngine2D::BuildPolylineGeometry(const std::vector<float>& points, float halfThickness)
    {
        // 連続する重複点は方向ベクトルが定義できないので除去する
        std::vector<Float2> path;
        path.reserve(points.size() / 2);
        for (size_t i = 0; i + 1 < points.size(); i += 2)
        {
            const Float2 p{ points[i], points[i + 1] };
            if (!path.empty() && Length(p - path.back()) < 1e-6f)
            {
                continue;
            }
            path.push_back(p);
            if (path.size() >= kMaxPolylinePoints)
            {
                break;
            }
        }

        if (path.size() < 2)
        {
            Core::Logger::Error("2D", "DrawPolyline: 重複点を除いた有効な点が2点未満です。描画しません");
            return 0;
        }

        const size_t pointCount = path.size();
        const size_t segmentCount = pointCount - 1;

        // セグメントごとの単位方向と左法線(ワールドはY-upなので、進行方向の左は(-dy, dx))
        std::vector<Float2> directions(segmentCount);
        std::vector<Float2> normals(segmentCount);
        std::vector<float> lengths(segmentCount);
        for (size_t i = 0; i < segmentCount; ++i)
        {
            const Float2 delta = path[i + 1] - path[i];
            lengths[i] = Length(delta);
            directions[i] = delta * (1.0f / lengths[i]);
            normals[i] = { -directions[i].Y, directions[i].X };
        }

        // 各点の左右レール。接合がベベルになる点だけ、外側が「入る側」「出る側」の2点に割れる
        struct Joint
        {
            Float2 LeftIn, LeftOut;   // 手前のセグメントが使う点 / 次のセグメントが使う点
            Float2 RightIn, RightOut;
            bool Bevel = false;
            bool LeftIsOuter = false; // ベベル時、どちら側が2点に割れているか
        };
        std::vector<Joint> joints(pointCount);

        // 端点(バットキャップ)。DrawLineが回転矩形=切りっぱなしなのに揃える
        joints[0].LeftIn = joints[0].LeftOut = path[0] + normals[0] * halfThickness;
        joints[0].RightIn = joints[0].RightOut = path[0] - normals[0] * halfThickness;
        const size_t last = pointCount - 1;
        joints[last].LeftIn = joints[last].LeftOut = path[last] + normals[segmentCount - 1] * halfThickness;
        joints[last].RightIn = joints[last].RightOut = path[last] - normals[segmentCount - 1] * halfThickness;

        for (size_t i = 1; i < last; ++i)
        {
            const Float2& prevNormal = normals[i - 1];
            const Float2& nextNormal = normals[i];
            const Float2 sum = prevNormal + nextNormal;
            const float sumLength = Length(sum);

            Joint& joint = joints[i];
            if (sumLength < 1e-6f)
            {
                // 180度の折り返し。normalize()が0除算でNaNになり、そのまま描くと画面全体が消えるため、
                // ここで潰す。オフセット0(接合点=元の点)のベベル扱いにする
                joint.Bevel = true;
                joint.LeftIsOuter = true;
                joint.LeftIn = path[i] + prevNormal * halfThickness;
                joint.LeftOut = path[i] + nextNormal * halfThickness;
                joint.RightIn = joint.RightOut = path[i];
                continue;
            }

            const Float2 miterDirection = sum * (1.0f / sumLength);
            const float denominator = Dot(miterDirection, prevNormal);
            const float miterLength = halfThickness / denominator;

            // 旋回方向。左へ曲がるなら左側が内側になる
            const float turn = Cross(directions[i - 1], directions[i]);
            const bool leftIsOuter = turn < 0.0f;

            // 内側は隣接する2セグメントの短いほうの長さでクランプする。これを忘れると
            // 鋭角+太線で内側レールが隣のセグメントを突き抜け、帯が自己交差して
            // その部分だけ色が濃くなる
            const float shorterSegment = (std::min)(lengths[i - 1], lengths[i]);
            const float innerLength = (std::min)(miterLength, shorterSegment);

            if (miterLength > halfThickness * kPolylineMiterLimit)
            {
                // 鋭角すぎるのでベベルへフォールバックする(外側だけ2点に割る)
                joint.Bevel = true;
                joint.LeftIsOuter = leftIsOuter;
                if (leftIsOuter)
                {
                    joint.LeftIn = path[i] + prevNormal * halfThickness;
                    joint.LeftOut = path[i] + nextNormal * halfThickness;
                    joint.RightIn = joint.RightOut = path[i] - miterDirection * innerLength;
                }
                else
                {
                    joint.RightIn = path[i] - prevNormal * halfThickness;
                    joint.RightOut = path[i] - nextNormal * halfThickness;
                    joint.LeftIn = joint.LeftOut = path[i] + miterDirection * innerLength;
                }
            }
            else
            {
                // マイター。外側は交点まで伸ばし、内側はクランプ後の長さを使う
                const float leftLength = leftIsOuter ? miterLength : innerLength;
                const float rightLength = leftIsOuter ? innerLength : miterLength;
                joint.LeftIn = joint.LeftOut = path[i] + miterDirection * leftLength;
                joint.RightIn = joint.RightOut = path[i] - miterDirection * rightLength;
            }
        }

        // 三角形を積む。2DのPSOは既定のラスタライザ(裏面カリング有効、時計回りが表)で作られており、
        // ワールドはY-upなので「符号付き面積が負(=Y-upで時計回り)」が表になる
        // (既存の単位クアッドの並びから逆算した規約)。旋回方向によって巻きが反転する
        // ベベル三角形があるため、面積の符号を見て必要なら2頂点を入れ替える
        m_PolylineVertices.clear();
        const auto emitTriangle = [this](Float2 a, Float2 b, Float2 c)
        {
            if (Cross(b - a, c - a) > 0.0f)
            {
                std::swap(b, c);
            }
            m_PolylineVertices.push_back({ { a.X, a.Y } });
            m_PolylineVertices.push_back({ { b.X, b.Y } });
            m_PolylineVertices.push_back({ { c.X, c.Y } });
        };

        for (size_t i = 0; i < segmentCount; ++i)
        {
            const Float2 leftStart = joints[i].LeftOut;
            const Float2 rightStart = joints[i].RightOut;
            const Float2 leftEnd = joints[i + 1].LeftIn;
            const Float2 rightEnd = joints[i + 1].RightIn;
            emitTriangle(leftStart, leftEnd, rightEnd);
            emitTriangle(leftStart, rightEnd, rightStart);
        }

        for (size_t i = 1; i < last; ++i)
        {
            const Joint& joint = joints[i];
            if (!joint.Bevel)
            {
                continue;
            }
            // 外側が割れてできた楔を1枚の三角形で埋める
            if (joint.LeftIsOuter)
            {
                emitTriangle(joint.RightIn, joint.LeftIn, joint.LeftOut);
            }
            else
            {
                emitTriangle(joint.LeftIn, joint.RightIn, joint.RightOut);
            }
        }

        const uint32_t vertexCount = static_cast<uint32_t>(m_PolylineVertices.size());
        if (vertexCount > kMaxPolylineVertices)
        {
            // 上限の計算(最悪ケース)が正しければ到達しない。防御的に弾いておく
            Core::Logger::Error(
                "2D",
                "DrawPolyline: 生成した頂点数がバッファ容量を超えました (" + std::to_string(vertexCount) +
                    " > " + std::to_string(kMaxPolylineVertices) + ")。描画しません");
            return 0;
        }
        return vertexCount;
    }

    void KurenaiEngine2D::DrawPolyline(const std::vector<float>& points, float thickness, float r, float g, float b, float a)
    {
        if (points.size() % 2 != 0)
        {
            Core::Logger::Error(
                "2D",
                "DrawPolyline: pointsの要素数が奇数です(" + std::to_string(points.size()) +
                    ")。{x, y}の並びで渡してください。描画しません");
            return;
        }
        if (points.size() < 4)
        {
            Core::Logger::Error("2D", "DrawPolyline: 点が2点未満です。描画しません");
            return;
        }
        if (!(thickness > 0.0f))
        {
            Core::Logger::Error(
                "2D",
                "DrawPolyline: 太さは0より大きい必要があります(指定値: " + std::to_string(thickness) + ")。描画しません");
            return;
        }
        if (points.size() / 2 > kMaxPolylinePoints)
        {
            Core::Logger::Error(
                "2D",
                "DrawPolyline: 点数が上限を超えました (" + std::to_string(points.size() / 2) + " > " +
                    std::to_string(kMaxPolylinePoints) + ")。先頭" + std::to_string(kMaxPolylinePoints) +
                    "点へ切り詰めて描画します");
        }

        if (m_PolylineDrawsThisFrame >= kMaxPolylinesPerFrame)
        {
            // DX12のステージングリングを周回すると描画結果が静かに壊れるため、その前に弾く。
            // ログは1フレームにつき1回だけ(毎フレーム大量に出るとログが埋まる)
            if (!m_PolylineOverflowLogged)
            {
                Core::Logger::Error(
                    "2D",
                    "DrawPolyline: 1フレームあたりの本数上限(" + std::to_string(kMaxPolylinesPerFrame) +
                        "本)を超えました。以降の呼び出しはこのフレームでは描画しません"
                        "(この警告はフレームにつき1回のみ)");
                m_PolylineOverflowLogged = true;
            }
            return;
        }

        const uint32_t vertexCount = BuildPolylineGeometry(points, thickness * 0.5f);
        if (vertexCount == 0)
        {
            return;
        }
        ++m_PolylineDrawsThisFrame;

        ObjectConstants objectConstants{};
        // 頂点は既にワールド座標なのでWorldは単位行列(シェーダ側でも掛けない)
        DirectX::XMStoreFloat4x4(&objectConstants.World, DirectX::XMMatrixIdentity());
        objectConstants.Color = { r, g, b, a };

        RHI::IRHICommandList* commandList = GetCommandList();
        commandList->SetPipelineState(m_PolylinePipelineState.get());
        // 【順序注意】UpdateBuffer → SetVertexShaderResourceBufferの順で呼ぶこと。
        // DX12のUpdateBufferは最後にリソースをPIXEL_SHADER_RESOURCEへ戻すため、
        // 逆順だと頂点段から不正な状態のリソースを読むことになる
        commandList->UpdateBuffer(
            m_PolylineVertexBuffer.get(), m_PolylineVertices.data(), vertexCount * sizeof(PolylineVertex));
        commandList->SetVertexShaderResourceBuffer(0, m_PolylineVertexBuffer.get());
        commandList->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
        commandList->SetConstantBuffer(1, m_ObjectConstantBuffer.get());
        commandList->Draw(vertexCount, 0);
        commandList->SetPipelineState(m_PipelineState.get()); // 以降のDrawSprite呼び出しのため戻す
    }

    void KurenaiEngine2D::DrawRoundedRect(
        float x, float y, float width, float height, float cornerRadiusPixels,
        float r, float g, float b, float a,
        float borderThicknessPixels,
        float borderR, float borderG, float borderB, float borderA,
        float rotationRadians)
    {
        ObjectConstants objectConstants{};
        // ShapeParamsはローカル空間(回転前)の半幅・半高さなので、回転を挟んでも
        // シェーダー側の角丸・枠線の判定には影響しない(DrawSpriteと同じ順序で掛ける)
        const DirectX::XMMATRIX world = DirectX::XMMatrixScaling(width, height, 1.0f) *
            DirectX::XMMatrixRotationZ(rotationRadians) *
            DirectX::XMMatrixTranslation(x, y, 0.0f);
        DirectX::XMStoreFloat4x4(&objectConstants.World, DirectX::XMMatrixTranspose(world));
        objectConstants.Color = { r, g, b, a };
        objectConstants.ShapeParams = { width * 0.5f, height * 0.5f, cornerRadiusPixels, borderThicknessPixels };
        objectConstants.BorderColor = { borderR, borderG, borderB, borderA };

        RHI::IRHICommandList* commandList = GetCommandList();
        commandList->SetPipelineState(m_RoundedRectPipelineState.get());
        commandList->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
        commandList->SetConstantBuffer(1, m_ObjectConstantBuffer.get());
        commandList->DrawIndexed(6, 0, 0);
        commandList->SetPipelineState(m_PipelineState.get()); // 以降のDrawSprite呼び出しのため戻す
    }

    std::vector<wchar_t> KurenaiEngine2D::DefaultAsciiChars()
    {
        std::vector<wchar_t> chars;
        chars.reserve(0x7E - 0x20 + 1);
        for (wchar_t ch = 0x20; ch <= 0x7E; ++ch)
        {
            chars.push_back(ch);
        }
        return chars;
    }

    void KurenaiEngine2D::BuildFontAtlas(const std::vector<wchar_t>& charsIn, bool bold)
    {
        std::vector<wchar_t> chars = charsIn;
        std::sort(chars.begin(), chars.end());
        chars.erase(std::unique(chars.begin(), chars.end()), chars.end());
        const int charCount = static_cast<int>(chars.size());

        HDC screenDC = GetDC(nullptr);
        HDC memDC = CreateCompatibleDC(screenDC);
        ReleaseDC(nullptr, screenDC);

        // ゲーム内HUD表示に使える程度の可読性があれば十分なため、厳密なフォントレンダリング
        // (ヒンティング等)は行わず、GDIでラスタライズしたビットマップフォントとして扱う。
        // ASCII文字だけでなくかな漢字も同じアトラスで扱うため、両方を含む標準的な日本語フォントを使う
        // (「Yu Gothic UI」はWindows 10/11に標準搭載されており、ASCII/かな/常用漢字を一通りカバーする)
        constexpr int kAtlasFontPixelHeight = 48;
        HFONT font = CreateFontW(
            -kAtlasFontPixelHeight, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Yu Gothic UI");
        if (!font)
        {
            DeleteDC(memDC);
            throw std::runtime_error("フォントアトラス用フォントの作成に失敗しました");
        }
        HFONT oldFont = static_cast<HFONT>(SelectObject(memDC, font));

        TEXTMETRICW metrics{};
        GetTextMetricsW(memDC, &metrics);

        // セルサイズは実際に使う文字の最大幅に合わせる。tmMaxCharWidthはフォント全体(数千字を含む
        // CJKフォントの場合、稀な全角記号等)の最大幅を返すため、それを使うとアトラスが不必要に
        // 肥大化してしまう
        constexpr int kPadding = 2;
        std::vector<SIZE> glyphSizes(charCount);
        int maxCharWidth = 1;
        for (int i = 0; i < charCount; ++i)
        {
            GetTextExtentPoint32W(memDC, &chars[i], 1, &glyphSizes[i]);
            maxCharWidth = (std::max)(maxCharWidth, static_cast<int>(glyphSizes[i].cx));
        }
        const int cellWidth = maxCharWidth + kPadding * 2;
        const int cellHeight = metrics.tmHeight + kPadding * 2;

        // 文字数に応じて概ね正方形になるグリッドに配置する(ASCIIのみの少数字数から、日本語を含む
        // 数百字規模まで、アトラスの縦横比が極端にならないようにするため)
        const int columns = (std::max)(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(charCount)))));
        const int rows = (charCount + columns - 1) / columns;
        const uint32_t atlasWidth = static_cast<uint32_t>(cellWidth * columns);
        const uint32_t atlasHeight = static_cast<uint32_t>(cellHeight * rows);

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(atlasWidth);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(atlasHeight); // 負値=トップダウンDIB(先頭行が画像上端)
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap)
        {
            SelectObject(memDC, oldFont);
            DeleteObject(font);
            DeleteDC(memDC);
            throw std::runtime_error("フォントアトラス用ビットマップの作成に失敗しました");
        }
        HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(memDC, bitmap));

        // 背景を黒、文字を白で描画し、後でRGB輝度(=白文字/黒背景なのでR=G=Bのグレースケール値)を
        // そのままアルファ値として使う(RGBは常に白のままにし、DrawText呼び出し時のColorで乗算ティントする)
        const RECT fillRect{ 0, 0, static_cast<LONG>(atlasWidth), static_cast<LONG>(atlasHeight) };
        FillRect(memDC, &fillRect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetTextColor(memDC, RGB(255, 255, 255));
        SetBkMode(memDC, TRANSPARENT);

        // 再構築のたびに古いグリフ情報を作り直す(古いアトラスのUV座標を引きずらないようにする)
        std::unordered_map<wchar_t, GlyphMetrics>& glyphs = bold ? m_BoldGlyphs : m_Glyphs;
        glyphs.clear();
        glyphs.reserve(charCount);
        for (int i = 0; i < charCount; ++i)
        {
            const wchar_t ch = chars[i];
            const int col = i % columns;
            const int row = i / columns;
            const int cellX = col * cellWidth;
            const int cellY = row * cellHeight;

            TextOutW(memDC, cellX + kPadding, cellY + kPadding, &ch, 1);

            GlyphMetrics glyph{};
            glyph.U0 = static_cast<float>(cellX) / atlasWidth;
            glyph.V0 = static_cast<float>(cellY) / atlasHeight;
            glyph.U1 = static_cast<float>(cellX + cellWidth) / atlasWidth;
            glyph.V1 = static_cast<float>(cellY + cellHeight) / atlasHeight;
            glyph.AdvancePixels = static_cast<float>(glyphSizes[i].cx);
            glyph.WidthPixels = static_cast<float>(cellWidth);
            glyph.HeightPixels = static_cast<float>(cellHeight);
            glyphs.emplace(ch, glyph);
        }

        GdiFlush();

        std::vector<uint8_t> pixels(static_cast<size_t>(atlasWidth) * atlasHeight * 4);
        const uint8_t* src = static_cast<const uint8_t*>(bits);
        const size_t pixelCount = static_cast<size_t>(atlasWidth) * atlasHeight;
        for (size_t p = 0; p < pixelCount; ++p)
        {
            const uint8_t coverage = src[p * 4 + 0]; // DIBはBGRA順。B成分=R=G(グレースケールAA)をアルファに使う
            pixels[p * 4 + 0] = 255;
            pixels[p * 4 + 1] = 255;
            pixels[p * 4 + 2] = 255;
            pixels[p * 4 + 3] = coverage;
        }

        SelectObject(memDC, oldBitmap);
        DeleteObject(bitmap);
        SelectObject(memDC, oldFont);
        DeleteObject(font);
        DeleteDC(memDC);

        std::unique_ptr<RHI::IRHITexture>& atlasTexture = bold ? m_BoldFontAtlasTexture : m_FontAtlasTexture;
        atlasTexture = m_Device->CreateTextureFromMemory(atlasWidth, atlasHeight, pixels.data());
        (bold ? m_BoldFontAtlasPixelHeight : m_FontAtlasPixelHeight) = static_cast<float>(metrics.tmHeight);
        (bold ? m_BoldFontAtlasCellHeight : m_FontAtlasCellHeight) = static_cast<float>(cellHeight);
    }

    void KurenaiEngine2D::RebuildFontAtlasIfPending(bool bold)
    {
        std::vector<wchar_t>& pendingChars = bold ? m_PendingBoldChars : m_PendingChars;
        if (pendingChars.empty())
        {
            return;
        }
        const std::unordered_map<wchar_t, GlyphMetrics>& glyphs = bold ? m_BoldGlyphs : m_Glyphs;

        std::vector<wchar_t> chars;
        chars.reserve(glyphs.size() + pendingChars.size());
        for (const auto& glyphEntry : glyphs)
        {
            chars.push_back(glyphEntry.first);
        }
        chars.insert(chars.end(), pendingChars.begin(), pendingChars.end());
        pendingChars.clear();
        BuildFontAtlas(chars, bold);
    }

    const KurenaiEngine2D::GlyphMetrics* KurenaiEngine2D::FindGlyph(wchar_t ch, bool bold)
    {
        std::unordered_map<wchar_t, GlyphMetrics>& glyphs = bold ? m_BoldGlyphs : m_Glyphs;
        const auto it = glyphs.find(ch);
        if (it != glyphs.end())
        {
            return &it->second;
        }

        // アトラス未収録の文字。次のBeginFrame()の先頭でアトラスへ追加されるまでの間、
        // この文字自体の描画・幅計測はスキップする(呼び出し側で対応する)
        std::vector<wchar_t>& pendingChars = bold ? m_PendingBoldChars : m_PendingChars;
        if (std::find(pendingChars.begin(), pendingChars.end(), ch) == pendingChars.end())
        {
            pendingChars.push_back(ch);
        }
        return nullptr;
    }

    std::vector<std::wstring> KurenaiEngine2D::SplitTextIntoLines(const std::wstring& text)
    {
        // 改行を含まない場合(大多数)は分割せずそのまま1行として返す
        std::vector<std::wstring> lines;
        std::wstring current;
        current.reserve(text.size());
        for (const wchar_t ch : text)
        {
            if (ch == L'\n')
            {
                lines.push_back(current);
                current.clear();
                continue;
            }
            if (ch == L'\r')
            {
                // CRLFの\rは読み飛ばす(\nだけで1改行として扱う)。単独の\rも同様に無視する
                continue;
            }
            current.push_back(ch);
        }
        lines.push_back(current); // 末尾が\nの場合は空行が1つ増える(見た目上も1行ぶん送られる)
        return lines;
    }

    void KurenaiEngine2D::DrawText(
        float x, float y, const std::wstring& text, float fontSize, float r, float g, float b, float a,
        bool bold, TextAlign align, TextVerticalAlign verticalAlign)
    {
        const float atlasPixelHeight = bold ? m_BoldFontAtlasPixelHeight : m_FontAtlasPixelHeight;
        const float scale = fontSize / atlasPixelHeight;
        RHI::IRHICommandList* commandList = GetCommandList();
        commandList->SetTexture(0, (bold ? m_BoldFontAtlasTexture : m_FontAtlasTexture).get());

        const std::vector<std::wstring> lines = SplitTextIntoLines(text);
        const float lineHeight = GetLineHeight(fontSize, bold);

        // verticalAlign=Topの場合はyがそのままテキストブロック上端基準になるため計算不要。
        // Middle/Bottomのときだけ、ブロック全体の高さ(行高さ×行数)からオフセットを求める
        // (verticalAlignは行ごとではなくテキストブロック全体に対して適用する)
        float lineTop = y;
        if (verticalAlign != TextVerticalAlign::Top)
        {
            const float blockHeight = lineHeight * static_cast<float>(lines.size());
            lineTop = (verticalAlign == TextVerticalAlign::Middle) ? y + blockHeight * 0.5f : y + blockHeight;
        }

        for (const std::wstring& line : lines)
        {
            // align=Left(既定はCenterだが、この分岐自体はLeftのときだけ計算を省く)の場合はxがそのまま
            // 行の左端基準になるため、MeasureTextによる幅の実測は不要。Center/Rightのときだけ実測する
            // (alignは行ごとに適用する。Centerなら各行がそれぞれ中央揃えになる)
            float penX = x;
            if (align != TextAlign::Left)
            {
                const float lineWidth = MeasureText(line, fontSize, bold);
                penX = (align == TextAlign::Center) ? x - lineWidth * 0.5f : x - lineWidth;
            }

            for (const wchar_t ch : line)
            {
                const GlyphMetrics* glyph = FindGlyph(ch, bold);
                if (!glyph)
                {
                    // アトラス未収録の文字。次のBeginFrame()の先頭でアトラスへ追加されるまでの間、
                    // この文字自体の描画はスキップする(ペン位置も進めない簡易実装)
                    continue;
                }
                const float glyphWidth = glyph->WidthPixels * scale;
                const float glyphHeight = glyph->HeightPixels * scale;

                ObjectConstants objectConstants{};
                // penXは行の左端基準、lineTopはその行の上端基準(align/verticalAlignによる
                // オフセットは適用済み)。DrawSpriteと同様ワールド座標はY-upなので、
                // グリフ矩形の中心はpenXから右へ、lineTopから下(Y-upなので減算方向)へずらした位置になる
                const DirectX::XMMATRIX world = DirectX::XMMatrixScaling(glyphWidth, glyphHeight, 1.0f) *
                    DirectX::XMMatrixTranslation(penX + glyphWidth * 0.5f, lineTop - glyphHeight * 0.5f, 0.0f);
                DirectX::XMStoreFloat4x4(&objectConstants.World, DirectX::XMMatrixTranspose(world));
                objectConstants.Color = { r, g, b, a };
                objectConstants.UVOffsetScale = { glyph->U0, glyph->V0, glyph->U1 - glyph->U0, glyph->V1 - glyph->V0 };

                commandList->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                commandList->SetConstantBuffer(1, m_ObjectConstantBuffer.get());
                commandList->DrawIndexed(6, 0, 0);

                penX += glyph->AdvancePixels * scale;
            }

            lineTop -= lineHeight; // ワールドはY-upなので、次の行は下=減算方向へ送る
        }
    }

    float KurenaiEngine2D::MeasureText(const std::wstring& text, float fontSize, bool bold)
    {
        const float atlasPixelHeight = bold ? m_BoldFontAtlasPixelHeight : m_FontAtlasPixelHeight;
        const float scale = fontSize / atlasPixelHeight;

        float width = 0.0f;
        for (const wchar_t ch : text)
        {
            const GlyphMetrics* glyph = FindGlyph(ch, bold);
            if (glyph)
            {
                width += glyph->AdvancePixels * scale;
            }
        }
        return width;
    }

    float KurenaiEngine2D::GetLineHeight(float fontSize, bool bold) const
    {
        const float atlasPixelHeight = bold ? m_BoldFontAtlasPixelHeight : m_FontAtlasPixelHeight;
        if (atlasPixelHeight <= 0.0f)
        {
            // BuildFontAtlasが一度も成功していない場合(コンストラクタで例外になるため通常は到達しない)
            Core::Logger::Error("2D", "GetLineHeight: フォントアトラスが構築されていません。0を返します");
            return 0.0f;
        }
        // DrawTextのグリフ拡大率と同じ式。セル高さはGDIのTEXTMETRICW::tmHeight+パディングで、
        // 文字集合によらずアトラス全体で共通
        return (bold ? m_BoldFontAtlasCellHeight : m_FontAtlasCellHeight) * (fontSize / atlasPixelHeight);
    }

    void KurenaiEngine2D::MeasureTextBlock(
        const std::wstring& text, float fontSize, float& outWidth, float& outHeight, bool bold)
    {
        const std::vector<std::wstring> lines = SplitTextIntoLines(text);

        outWidth = 0.0f;
        for (const std::wstring& line : lines)
        {
            outWidth = (std::max)(outWidth, MeasureText(line, fontSize, bold));
        }
        outHeight = GetLineHeight(fontSize, bold) * static_cast<float>(lines.size());
    }

    void KurenaiEngine2D::PushClipRect(float x, float y, float width, float height)
    {
        // ワールド(Y-up)矩形の対角2点をクライアント座標(Y-down)へ変換する。カメラ位置・ズーム・
        // レターボックスはWorldToClientがまとめて面倒を見る。Y-upとY-downで上下が入れ替わるため、
        // 変換後にmin/maxを取り直す(回転は無いので対角2点だけで軸平行矩形が確定する)
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
        WorldToClient(x - width * 0.5f, y + height * 0.5f, ax, ay); // ワールドの左上
        WorldToClient(x + width * 0.5f, y - height * 0.5f, bx, by); // ワールドの右下

        ClipRect rect;
        rect.Left = (std::min)(ax, bx);
        rect.Top = (std::min)(ay, by);
        rect.Right = (std::max)(ax, bx);
        rect.Bottom = (std::max)(ay, by);

        // ネスト時は現在の矩形との積。積が空(Left>=Right等)になってもそのまま積む
        // (対応するPopClipRectまで何も描かれない、という素直な結果になる)
        if (!m_ClipRectStack.empty())
        {
            const ClipRect& parent = m_ClipRectStack.back();
            rect.Left = (std::max)(rect.Left, parent.Left);
            rect.Top = (std::max)(rect.Top, parent.Top);
            rect.Right = (std::min)(rect.Right, parent.Right);
            rect.Bottom = (std::min)(rect.Bottom, parent.Bottom);
        }

        m_ClipRectStack.push_back(rect);
        ApplyClipRect();
    }

    void KurenaiEngine2D::PopClipRect()
    {
        if (m_ClipRectStack.empty())
        {
            Core::Logger::Error(
                "2D",
                "PopClipRect: クリップ矩形が1つも積まれていません。PushClipRectとの対応を"
                "確認してください。この呼び出しを無視します");
            return;
        }

        m_ClipRectStack.pop_back();
        ApplyClipRect();
    }

    void KurenaiEngine2D::ApplyClipRect()
    {
        RHI::IRHICommandList* commandList = GetCommandList();
        if (m_ClipRectStack.empty())
        {
            commandList->ResetScissorRect(); // ビューポート全体へ戻す
            return;
        }

        // D3D11/D3D12のシザーは「ピクセル中心が矩形の内側にあるピクセルを残す」規則
        // (ピクセルpが残る条件はLeft <= p < Right、中心はp+0.5)。したがって望みの実数矩形
        // [l, r)に対する正しい整数境界はfloor(l + 0.5) = 四捨五入になる。
        // ビューポート全体のときの外側丸め(MakeFullViewportScissorRect)とは目的が違う
        // (あちらは端の1pxを削らないため、こちらは1pxはみ出さないため)
        const ClipRect& rect = m_ClipRectStack.back();
        const auto toPixel = [](float value) { return static_cast<int32_t>(std::floor(value + 0.5f)); };
        commandList->SetScissorRect({ toPixel(rect.Left), toPixel(rect.Top), toPixel(rect.Right), toPixel(rect.Bottom) });
    }

    void KurenaiEngine2D::EndFrame(bool vsync)
    {
        if (!m_ClipRectStack.empty())
        {
            // PushClipRectとPopClipRectの数が合っていない。次のBeginFrameでスタックは捨てられ
            // シザー矩形もビューポート全体へ戻るため描画は続行できるが、意図しないクリップの
            // 原因になるので知らせる。毎フレーム出すとログが埋まるため最初の1回だけにする
            if (!m_ClipRectLeakLogged)
            {
                Core::Logger::Error(
                    "2D",
                    "EndFrame: PopClipRectされていないPushClipRectが" +
                        std::to_string(m_ClipRectStack.size()) +
                        "件残っています。次のBeginFrameで破棄しますが、Push/Popの対応を"
                        "確認してください(この警告は最初の1回のみ)");
                m_ClipRectLeakLogged = true;
            }
            m_ClipRectStack.clear();
        }

        m_SwapChain->Present(vsync);
    }
}
