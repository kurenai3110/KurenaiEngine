#include "KurenaiEngine2D.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

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

        // DrawRoundedRect用。DrawCircleと同様、頂点シェーダー・頂点レイアウトはスプライトと共用し、
        // ピクセルシェーダーのみ角丸矩形マスク版(PSRoundedRect)に差し替える
        m_RoundedRectPixelShader = m_Device->CreateShader({ RHI::ShaderStage::Pixel, shaderPath, "PSRoundedRect" });
        RHI::PipelineStateDesc roundedRectPipelineDesc = pipelineDesc;
        roundedRectPipelineDesc.PixelShader = m_RoundedRectPixelShader.get();
        m_RoundedRectPipelineState = m_Device->CreatePipelineState(roundedRectPipelineDesc);

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

        // BuildFontAtlasはコンストラクタで(BeginFrame/Drawの前に)呼ぶ必要がある。DX12の
        // CreateTextureFromMemoryは内部でSubmitAndWaitIdle(コマンドリストのフラッシュ+リセット)を
        // 伴うため、BeginFrame後のフレーム中に呼ぶとレンダーターゲット/パイプラインステート等の
        // 設定済み状態が失われてしまう(DrawText初回呼び出し時の遅延生成にしたところクラッシュした)。
        // 以後DrawTextで未収録の文字が見つかった場合も、同じ理由でBeginFrame()の先頭でのみ追加する
        BuildFontAtlas(DefaultAsciiChars(), false);
        BuildFontAtlas(DefaultAsciiChars(), true);
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

    void KurenaiEngine2D::DrawRoundedRect(
        float x, float y, float width, float height, float cornerRadiusPixels,
        float r, float g, float b, float a,
        float borderThicknessPixels,
        float borderR, float borderG, float borderB, float borderA)
    {
        ObjectConstants objectConstants{};
        const DirectX::XMMATRIX world = DirectX::XMMatrixScaling(width, height, 1.0f) *
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

    void KurenaiEngine2D::DrawText(
        float x, float y, const std::wstring& text, float fontSize, float r, float g, float b, float a,
        bool bold, TextAlign align, TextVerticalAlign verticalAlign)
    {
        const float atlasPixelHeight = bold ? m_BoldFontAtlasPixelHeight : m_FontAtlasPixelHeight;
        const float scale = fontSize / atlasPixelHeight;
        RHI::IRHICommandList* commandList = GetCommandList();
        commandList->SetTexture(0, (bold ? m_BoldFontAtlasTexture : m_FontAtlasTexture).get());

        // align=Left(既定はCenterだが、この分岐自体はLeftのときだけ計算を省く)の場合はxがそのまま
        // テキスト左端基準になるため、MeasureTextによる幅の実測は不要。Center/Rightのときだけ実測する
        float penX = x;
        if (align != TextAlign::Left)
        {
            const float totalWidth = MeasureText(text, fontSize, bold);
            penX = (align == TextAlign::Center) ? x - totalWidth * 0.5f : x - totalWidth;
        }

        // verticalAlign=Topの場合はyがそのままテキスト上端基準になるため、セル高さの取得は不要。
        // Middle/Bottomのときだけ、アトラス全体で共通の1文字ぶんのセル高さ
        // (m_FontAtlasCellHeight/m_BoldFontAtlasCellHeight)を使ってオフセットを計算する
        float penYFromTop = y;
        if (verticalAlign != TextVerticalAlign::Top)
        {
            const float lineHeight = (bold ? m_BoldFontAtlasCellHeight : m_FontAtlasCellHeight) * scale;
            penYFromTop = (verticalAlign == TextVerticalAlign::Middle) ? y + lineHeight * 0.5f : y + lineHeight;
        }

        for (const wchar_t ch : text)
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
            // penXはテキスト左端基準、penYFromTopはテキスト上端基準(align/verticalAlignによる
            // オフセットは呼び出し前に適用済み)。DrawSpriteと同様ワールド座標はY-upなので、
            // グリフ矩形の中心はpenXから右へ、penYFromTopから下(Y-upなので減算方向)へずらした位置になる
            const DirectX::XMMATRIX world = DirectX::XMMatrixScaling(glyphWidth, glyphHeight, 1.0f) *
                DirectX::XMMatrixTranslation(penX + glyphWidth * 0.5f, penYFromTop - glyphHeight * 0.5f, 0.0f);
            DirectX::XMStoreFloat4x4(&objectConstants.World, DirectX::XMMatrixTranspose(world));
            objectConstants.Color = { r, g, b, a };
            objectConstants.UVOffsetScale = { glyph->U0, glyph->V0, glyph->U1 - glyph->U0, glyph->V1 - glyph->V0 };

            commandList->UpdateBuffer(m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
            commandList->SetConstantBuffer(1, m_ObjectConstantBuffer.get());
            commandList->DrawIndexed(6, 0, 0);

            penX += glyph->AdvancePixels * scale;
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

    void KurenaiEngine2D::EndFrame(bool vsync)
    {
        m_SwapChain->Present(vsync);
    }
}
