#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "KurenaiEngineBase.h"
#include "KurenaiTypes.h"

#include "Core/Camera.h"

#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai
{
    // KurenaiEngine2D::LoadTexture / CreateSolidColorTexture が返す不透明なテクスチャハンドル。
    // 内部型を一切持たないため、KurenaiEngine.dllのエクスポート境界をまたいでも安全に値渡しできる
    class TextureHandle
    {
    public:
        TextureHandle() = default;
        bool IsValid() const { return m_Handle != nullptr; }

    private:
        explicit TextureHandle(void* handle) : m_Handle(handle) {}
        void* m_Handle = nullptr;
        friend class KurenaiEngine2D;
    };

    // 2Dサンプルプログラム向けの公開API。正射影カメラとアルファブレンドによる
    // スプライト描画を提供する。ワールド座標=ピクセル座標(原点は画面左下、Y-up)。
    //
    // 使い方:
    //   KurenaiEngine2D renderer(L"Title", 1280, 720);
    //   TextureHandle tex = renderer.CreateSolidColorTexture(255, 255, 255, 255);
    //   while (!renderer.ShouldClose())
    //   {
    //       renderer.PumpEvents();
    //       renderer.BeginFrame(0.1f, 0.1f, 0.1f);
    //       renderer.DrawSprite(x, y, w, h, 0.0f, tex, 1, 1, 1, 1);
    //       renderer.EndFrame();
    //   }
    class KURENAI_API KurenaiEngine2D : public KurenaiEngineBase
    {
    public:
        KurenaiEngine2D(const std::wstring& title, uint32_t width, uint32_t height, GraphicsAPI api = GraphicsAPI::DX11);
        ~KurenaiEngine2D();

        TextureHandle LoadTexture(const std::wstring& filePath, bool sRGB = false);
        TextureHandle CreateSolidColorTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

        // 画面をクリアし、以後のDrawSprite呼び出しの準備をする。1フレームにつき1回だけ呼ぶ
        void BeginFrame(float clearR, float clearG, float clearB, float clearA = 1.0f);

        // x, y はワールド=ピクセル座標(原点は画面左下、Y-up)のスプライト中心位置。
        // width, height はピクセル単位のスプライトサイズ。rotationRadiansはZ軸(画面手前向き)回転。
        // r, g, b, a はテクスチャに乗算されるティント色(半透明にしたい場合はaを1未満にする)
        void DrawSprite(
            float x, float y, float width, float height, float rotationRadians,
            TextureHandle texture, float r, float g, float b, float a);

        // 中心(x, y)、半径radiusの塗り円を描画する。r, g, b, aは塗りつぶし色(半透明可)
        void DrawCircle(float x, float y, float radius, float r, float g, float b, float a);

        // (x1, y1)-(x2, y2)を結ぶ、太さthicknessの線分を描画する。r, g, b, aは色(半透明可)
        void DrawLine(float x1, float y1, float x2, float y2, float thickness, float r, float g, float b, float a);

        // (x, y)を左下基準としてtextを描画する。fontSizeはおおよその文字高さ(ピクセル単位)。
        // ビットマップフォント方式のため、厳密なフォントレンダリング(ヒンティング等)は行わない。
        // ASCII印字可能文字(0x20〜0x7E)に加え、かな漢字を含む任意のUnicode文字(BMP範囲)に対応する。
        // ただし初めて描画する文字はその場ではアトラスに含まれていないため1フレームだけ表示されず、
        // 次のBeginFrame()でアトラスへ追加されてから以降のフレームで表示される
        // (フレーム中にテクスチャを作り直すとDX12でレンダーターゲット/パイプラインステートの設定が
        // 失われるため、追加はBeginFrame()の先頭でのみ行う設計になっている)
        void DrawText(float x, float y, const std::wstring& text, float fontSize, float r, float g, float b, float a);

        // 描画コマンドを確定してバックバッファへ表示する。1フレームにつき1回だけ呼ぶ
        void EndFrame(bool vsync = true);

    private:
        // DrawText用の1文字ぶんのメトリクス。すべてBuildFontAtlasの生成時解像度(m_FontAtlasPixelHeight)基準の値
        struct GlyphMetrics
        {
            float U0 = 0.0f, V0 = 0.0f, U1 = 0.0f, V1 = 0.0f; // アトラス内のUV矩形
            float AdvancePixels = 0.0f;
            float WidthPixels = 0.0f, HeightPixels = 0.0f;
        };
        // GDIでcharsに含まれる文字一式をラスタライズし、m_FontAtlasTexture/m_Glyphsを(既存の内容を
        // 置き換えて)再構築する。コンストラクタ、またはBeginFrame()の先頭でのみ呼ぶ必要がある
        // (DX12のCreateTextureFromMemoryは内部でコマンドリストをフラッシュ・リセットするため、
        // 通常の描画コマンドを積んだ後のフレーム中に呼ぶと、それらの設定が失われクラッシュする)
        void BuildFontAtlas(const std::vector<wchar_t>& chars);

        // 初回のASCII一式(0x20〜0x7E)を返す。コンストラクタでのBuildFontAtlas呼び出し用
        static std::vector<wchar_t> DefaultAsciiChars();

        Core::Camera m_Camera;

        std::unique_ptr<RHI::IRHIShader> m_VertexShader;
        std::unique_ptr<RHI::IRHIShader> m_PixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_PipelineState;

        // DrawCircle用。頂点シェーダー・頂点/インデックスバッファはスプライトと共用し、
        // ピクセルシェーダーとパイプラインステートのみ専用のものを使う
        std::unique_ptr<RHI::IRHIShader> m_CirclePixelShader;
        std::unique_ptr<RHI::IRHIPipelineState> m_CirclePipelineState;

        // DrawLineは太さ・長さに拡縮縮小した矩形として、この不透明白テクスチャを使ってDrawSpriteと
        // 同じスプライトパイプラインで描画する
        TextureHandle m_WhiteTexture;

        std::unique_ptr<RHI::IRHIBuffer> m_QuadVertexBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_QuadIndexBuffer;

        std::unique_ptr<RHI::IRHISampler> m_Sampler;
        std::unique_ptr<RHI::IRHIBuffer> m_FrameConstantBuffer;
        std::unique_ptr<RHI::IRHIBuffer> m_ObjectConstantBuffer;

        // LoadTexture/CreateSolidColorTextureで読み込んだテクスチャの実体を保持する
        // (TextureHandleは所有権を持たない借用ポインタのため)
        std::vector<std::unique_ptr<RHI::IRHITexture>> m_Textures;

        // DrawText用。コンストラクタでBuildFontAtlasにより生成される
        std::unique_ptr<RHI::IRHITexture> m_FontAtlasTexture;
        std::unordered_map<wchar_t, GlyphMetrics> m_Glyphs;
        // BuildFontAtlasが生成したフォントの基準ピクセル高さ。DrawTextのfontSizeはこれに対する
        // 拡大率(fontSize / m_FontAtlasPixelHeight)としてグリフの表示サイズに反映される
        float m_FontAtlasPixelHeight = 0.0f;
        // DrawTextでm_Glyphsに見つからなかった(=アトラス未収録の)文字を一時的に溜めておくキュー。
        // 次のBeginFrame()の先頭でm_Glyphsの既存キーと合わせてBuildFontAtlasに渡され、消費後クリアされる
        std::vector<wchar_t> m_PendingChars;
    };
}

#pragma warning(pop)
