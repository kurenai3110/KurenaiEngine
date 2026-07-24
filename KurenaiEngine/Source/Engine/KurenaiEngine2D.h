#pragma once

#include <cstdint>
#include <memory>
#include <string>
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

        // 描画コマンドを確定してバックバッファへ表示する。1フレームにつき1回だけ呼ぶ
        void EndFrame(bool vsync = true);

    private:
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
    };
}

#pragma warning(pop)
