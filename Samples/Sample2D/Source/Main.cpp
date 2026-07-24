// KurenaiEngine 2Dサンプルプログラム。
// 公開API(KurenaiEngine2D)のみを使い、正射影カメラ・アルファブレンドの詳細はエンジン側に隠蔽されている。
// 画面内を跳ね回る半透明の色つきスプライトを描画する。Escキーで終了する。

#include <Windows.h>

#include <objbase.h>

#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <fstream>
#include <string>

#include "KurenaiEngine2D.h"
#include "KurenaiTypes.h"

using namespace Kurenai;

namespace
{
    struct Sprite
    {
        float PositionX = 0.0f;
        float PositionY = 0.0f; // ワールド=ピクセル座標(原点は画面左下、Y-up)
        float VelocityX = 0.0f;
        float VelocityY = 0.0f;
        float Size = 0.0f;
        float RotationSpeed = 0.0f;
        float Rotation = 0.0f;
        float R = 1.0f, G = 1.0f, B = 1.0f, A = 1.0f;
    };

    constexpr uint32_t kWindowWidth = 1280;
    constexpr uint32_t kWindowHeight = 720;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int exitCode = 0;
    try
    {
        KurenaiEngine2D renderer(L"KurenaiEngine Sample2D", kWindowWidth, kWindowHeight, GraphicsAPI::DX11);

        const TextureHandle whiteTexture = renderer.CreateSolidColorTexture(255, 255, 255, 255);

        const std::array<std::array<float, 4>, 6> palette = { {
            { 0.95f, 0.25f, 0.30f, 0.8f },
            { 0.25f, 0.65f, 0.95f, 0.8f },
            { 0.95f, 0.80f, 0.20f, 0.8f },
            { 0.35f, 0.85f, 0.45f, 0.8f },
            { 0.75f, 0.35f, 0.90f, 0.8f },
            { 0.95f, 0.55f, 0.20f, 0.8f },
        } };

        std::array<Sprite, 24> sprites{};
        for (size_t i = 0; i < sprites.size(); ++i)
        {
            const float t = static_cast<float>(i);
            Sprite& sprite = sprites[i];
            sprite.PositionX = 120.0f + std::fmodf(t * 173.0f, kWindowWidth - 240.0f);
            sprite.PositionY = 100.0f + std::fmodf(t * 251.0f, kWindowHeight - 200.0f);
            const float angle = t * 0.9f;
            sprite.VelocityX = std::cosf(angle) * 180.0f;
            sprite.VelocityY = std::sinf(angle) * 140.0f;
            sprite.Size = 48.0f + std::fmodf(t * 37.0f, 64.0f);
            sprite.RotationSpeed = (i % 2 == 0 ? 1.0f : -1.0f) * (0.4f + 0.05f * t);
            const auto& color = palette[i % palette.size()];
            sprite.R = color[0];
            sprite.G = color[1];
            sprite.B = color[2];
            sprite.A = color[3];
        }

        auto lastFrameTime = std::chrono::steady_clock::now();

        while (!renderer.ShouldClose())
        {
            renderer.PumpEvents();

            const auto now = std::chrono::steady_clock::now();
            const float deltaTime = std::chrono::duration<float>(now - lastFrameTime).count();
            lastFrameTime = now;

            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
            {
                renderer.Close();
            }

            const uint32_t width = renderer.GetWidth();
            const uint32_t height = renderer.GetHeight();
            if (width == 0 || height == 0)
            {
                continue;
            }

            // スプライトの移動・反射・回転を更新する
            for (Sprite& sprite : sprites)
            {
                sprite.PositionX += sprite.VelocityX * deltaTime;
                sprite.PositionY += sprite.VelocityY * deltaTime;
                sprite.Rotation += sprite.RotationSpeed * deltaTime;

                const float half = sprite.Size * 0.5f;
                if (sprite.PositionX - half < 0.0f)
                {
                    sprite.PositionX = half;
                    sprite.VelocityX = -sprite.VelocityX;
                }
                else if (sprite.PositionX + half > static_cast<float>(width))
                {
                    sprite.PositionX = static_cast<float>(width) - half;
                    sprite.VelocityX = -sprite.VelocityX;
                }
                if (sprite.PositionY - half < 0.0f)
                {
                    sprite.PositionY = half;
                    sprite.VelocityY = -sprite.VelocityY;
                }
                else if (sprite.PositionY + half > static_cast<float>(height))
                {
                    sprite.PositionY = static_cast<float>(height) - half;
                    sprite.VelocityY = -sprite.VelocityY;
                }
            }

            renderer.BeginFrame(0.08f, 0.08f, 0.12f);
            for (const Sprite& sprite : sprites)
            {
                renderer.DrawSprite(
                    sprite.PositionX, sprite.PositionY, sprite.Size, sprite.Size, sprite.Rotation,
                    whiteTexture, sprite.R, sprite.G, sprite.B, sprite.A);
            }
            renderer.EndFrame(true);
        }
    }
    catch (const std::exception& e)
    {
        std::ofstream log("error.log", std::ios::app);
        log << e.what() << std::endl;
        MessageBoxA(nullptr, e.what(), "KurenaiEngine Sample2D - 初期化エラー", MB_OK | MB_ICONERROR);
        exitCode = 1;
    }

    if (SUCCEEDED(comResult))
    {
        CoUninitialize();
    }

    return exitCode;
}
