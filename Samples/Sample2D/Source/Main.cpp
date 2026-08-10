// KurenaiEngine 2Dサンプルプログラム。
// 公開API(KurenaiEngine2D)のみを使い、正射影カメラ・アルファブレンドの詳細はエンジン側に隠蔽されている。
//
// 数字キーで2Dの各機能のデモ画面を切り替えられる。Escキーで終了する。
// 「-dx12」引数を付けて起動するとDX12バックエンドになる(再ビルド無しでDX11/DX12を見比べるため)。

#include <Windows.h>

#include <objbase.h>
#include <shellapi.h>

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
    // デモ画面。数字キー(1〜)で切り替える。issueごとに1画面ずつ足していく
    enum class DemoScene
    {
        Sprites = 0, // 1: 跳ね回る半透明スプライト(従来のサンプル内容)
        Input,       // 2: 入力(押下エッジ・解放エッジ)
        Count
    };

    constexpr uint32_t kWindowWidth = 1280;
    constexpr uint32_t kWindowHeight = 720;

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

    // 「-dx12」引数が指定されていればDX12バックエンドを使う(Sample3Dと同じ流儀)
    GraphicsAPI ParseGraphicsAPI()
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            return GraphicsAPI::DX11;
        }

        GraphicsAPI api = GraphicsAPI::DX11;
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"-dx12") == 0)
            {
                api = GraphicsAPI::DX12;
                break;
            }
        }

        LocalFree(argv);
        return api;
    }

    const wchar_t* GetSceneTitle(DemoScene scene)
    {
        switch (scene)
        {
        case DemoScene::Sprites: return L"1: DrawSprite (跳ね回る半透明スプライト)";
        case DemoScene::Input: return L"2: 入力 (押下エッジ・解放エッジ)";
        default: return L"(不明なデモ画面)";
        }
    }

    // 画面上端に、選択中のデモ画面名と操作説明を出す(全デモ画面で共通)
    void DrawHeader(KurenaiEngine2D& renderer, DemoScene scene, GraphicsAPI api, float clientHeight)
    {
        const std::wstring title = std::wstring(api == GraphicsAPI::DX12 ? L"[DX12] " : L"[DX11] ") + GetSceneTitle(scene);
        renderer.DrawText(
            16.0f, clientHeight - 12.0f, title, 22.0f, 1.0f, 1.0f, 1.0f, 1.0f,
            true, TextAlign::Left, TextVerticalAlign::Top);
        renderer.DrawText(
            16.0f, clientHeight - 40.0f, L"数字キー: デモ切り替え / Esc: 終了", 16.0f, 0.7f, 0.7f, 0.75f, 1.0f,
            false, TextAlign::Left, TextVerticalAlign::Top);
    }

    void InitSprites(std::array<Sprite, 24>& sprites)
    {
        const std::array<std::array<float, 4>, 6> palette = { {
            { 0.95f, 0.25f, 0.30f, 0.8f },
            { 0.25f, 0.65f, 0.95f, 0.8f },
            { 0.95f, 0.80f, 0.20f, 0.8f },
            { 0.35f, 0.85f, 0.45f, 0.8f },
            { 0.75f, 0.35f, 0.90f, 0.8f },
            { 0.95f, 0.55f, 0.20f, 0.8f },
        } };

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
    }

    void UpdateSprites(std::array<Sprite, 24>& sprites, float deltaTime, float width, float height)
    {
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
            else if (sprite.PositionX + half > width)
            {
                sprite.PositionX = width - half;
                sprite.VelocityX = -sprite.VelocityX;
            }
            if (sprite.PositionY - half < 0.0f)
            {
                sprite.PositionY = half;
                sprite.VelocityY = -sprite.VelocityY;
            }
            else if (sprite.PositionY + half > height)
            {
                sprite.PositionY = height - half;
                sprite.VelocityY = -sprite.VelocityY;
            }
        }
    }

    // 「2: 入力」のデモが持ち越す状態。エッジは1フレームしか立たないため、
    // 目視できるよう回数を数えて表示する
    struct InputDemoState
    {
        int LeftPressCount = 0;
        int LeftReleaseCount = 0;
        int SpacePressCount = 0;
        int SpaceReleaseCount = 0;
        int ClickCount = 0;   // 「押下→同じボタン上での解放」で確定したクリックの回数
        int CancelCount = 0;  // ボタンの外で離してキャンセルされた回数
        bool ButtonArmed = false; // ボタンの上で押下された(= 解放でクリック確定できる)状態か
        float WheelDelta = 0.0f;      // 直近フレームの回転量(0でないフレームだけ更新して見えるようにする)
        float WheelAccumulated = 0.0f; // 起動してからの累積(スクロール量に相当)
    };

    // 一般的なUIのボタンと同じく「押下→同じボタン上での解放」でクリックを確定する。
    // WasMouseButtonReleasedが無いとこの挙動は書けない(押したまま外へ出すキャンセルが作れない)
    void UpdateInputDemo(KurenaiEngine2D& renderer, InputDemoState& state, float buttonX, float buttonY, float buttonW, float buttonH)
    {
        const POINT clientMouse = renderer.GetClientMousePosition();
        const float mouseX = static_cast<float>(clientMouse.x);
        // クライアント座標はY-down、描画はY-upなので上下を反転する
        const float mouseY = static_cast<float>(renderer.GetHeight()) - static_cast<float>(clientMouse.y);
        const bool inside =
            mouseX >= buttonX - buttonW * 0.5f && mouseX <= buttonX + buttonW * 0.5f &&
            mouseY >= buttonY - buttonH * 0.5f && mouseY <= buttonY + buttonH * 0.5f;

        if (renderer.WasMouseButtonPressed(MouseButton::Left))
        {
            ++state.LeftPressCount;
            state.ButtonArmed = inside;
        }
        if (renderer.WasMouseButtonReleased(MouseButton::Left))
        {
            ++state.LeftReleaseCount;
            if (state.ButtonArmed)
            {
                if (inside)
                {
                    ++state.ClickCount;
                }
                else
                {
                    ++state.CancelCount;
                }
            }
            state.ButtonArmed = false;
        }

        if (renderer.WasKeyPressed(VK_SPACE))
        {
            ++state.SpacePressCount;
        }
        if (renderer.WasKeyReleased(VK_SPACE))
        {
            ++state.SpaceReleaseCount;
        }

        const float wheel = renderer.GetMouseWheelDelta();
        if (wheel != 0.0f)
        {
            // 回転量は1フレームしか立たないため、目視できるよう直近の値と累積を保持する
            state.WheelDelta = wheel;
            state.WheelAccumulated += wheel;
        }
    }

    // 小数第2位までの文字列。ホイールの回転量は高分解能ホイールだと小数になり得る
    std::wstring FormatFloat(float value)
    {
        wchar_t buffer[32]{};
        swprintf_s(buffer, L"%.2f", value);
        return buffer;
    }

    void DrawInputDemo(KurenaiEngine2D& renderer, const InputDemoState& state, float buttonX, float buttonY, float buttonW, float buttonH)
    {
        const bool held = state.ButtonArmed && renderer.IsMouseButtonDown(MouseButton::Left);
        renderer.DrawRoundedRect(
            buttonX, buttonY, buttonW, buttonH, 10.0f,
            held ? 0.30f : 0.18f, held ? 0.45f : 0.24f, held ? 0.70f : 0.34f, 1.0f,
            2.0f, 0.55f, 0.65f, 0.85f, 1.0f);
        renderer.DrawText(buttonX, buttonY, L"解放でクリック確定", 20.0f, 1.0f, 1.0f, 1.0f, 1.0f, true);

        const float textX = 40.0f;
        float textY = buttonY - 70.0f;
        const auto line = [&](const std::wstring& text)
        {
            renderer.DrawText(textX, textY, text, 18.0f, 0.85f, 0.88f, 0.95f, 1.0f, false, TextAlign::Left, TextVerticalAlign::Top);
            textY -= 26.0f;
        };
        line(L"左ボタン 押下: " + std::to_wstring(state.LeftPressCount) + L" / 解放: " + std::to_wstring(state.LeftReleaseCount));
        line(L"Space 押下: " + std::to_wstring(state.SpacePressCount) + L" / 解放: " + std::to_wstring(state.SpaceReleaseCount));
        line(L"クリック確定: " + std::to_wstring(state.ClickCount) + L" / 外で離してキャンセル: " + std::to_wstring(state.CancelCount));
        line(L"ホイール 直近: " + FormatFloat(state.WheelDelta) + L" / 累積: " + FormatFloat(state.WheelAccumulated));
        line(L"ボタンの上で押し、外へ出して離すとキャンセルになる");
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int exitCode = 0;
    try
    {
        const GraphicsAPI api = ParseGraphicsAPI();
        KurenaiEngine2D renderer(L"KurenaiEngine Sample2D", kWindowWidth, kWindowHeight, api);

        const TextureHandle whiteTexture = renderer.CreateSolidColorTexture(255, 255, 255, 255);

        std::array<Sprite, 24> sprites{};
        InitSprites(sprites);
        InputDemoState inputDemo{};

        DemoScene scene = DemoScene::Sprites;
        auto lastFrameTime = std::chrono::steady_clock::now();

        while (!renderer.ShouldClose())
        {
            renderer.PumpEvents();

            const auto now = std::chrono::steady_clock::now();
            const float deltaTime = std::chrono::duration<float>(now - lastFrameTime).count();
            lastFrameTime = now;

            if (renderer.WasKeyPressed(VK_ESCAPE))
            {
                renderer.Close();
            }

            // 数字キー'1'〜でデモ画面を切り替える
            for (int i = 0; i < static_cast<int>(DemoScene::Count); ++i)
            {
                if (renderer.WasKeyPressed('1' + i))
                {
                    scene = static_cast<DemoScene>(i);
                }
            }

            const float width = static_cast<float>(renderer.GetWidth());
            const float height = static_cast<float>(renderer.GetHeight());
            if (width <= 0.0f || height <= 0.0f)
            {
                continue;
            }

            // 入力デモのボタン。ウィンドウサイズに追従させるため毎フレーム求める
            const float buttonWidth = 260.0f;
            const float buttonHeight = 64.0f;
            const float buttonX = width * 0.5f;
            const float buttonY = height * 0.6f;

            if (scene == DemoScene::Sprites)
            {
                UpdateSprites(sprites, deltaTime, width, height);
            }
            else if (scene == DemoScene::Input)
            {
                UpdateInputDemo(renderer, inputDemo, buttonX, buttonY, buttonWidth, buttonHeight);
            }

            renderer.BeginFrame(0.08f, 0.08f, 0.12f);

            switch (scene)
            {
            case DemoScene::Sprites:
                for (const Sprite& sprite : sprites)
                {
                    renderer.DrawSprite(
                        sprite.PositionX, sprite.PositionY, sprite.Size, sprite.Size, sprite.Rotation,
                        whiteTexture, sprite.R, sprite.G, sprite.B, sprite.A);
                }
                break;
            case DemoScene::Input:
                DrawInputDemo(renderer, inputDemo, buttonX, buttonY, buttonWidth, buttonHeight);
                break;
            default:
                break;
            }

            DrawHeader(renderer, scene, api, height);
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
