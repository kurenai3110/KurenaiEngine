// KurenaiEngine 2Dサンプルプログラム。
// 公開API(KurenaiEngine2D)のみを使い、正射影カメラ・アルファブレンドの詳細はエンジン側に隠蔽されている。
//
// 数字キーで2Dの各機能のデモ画面を切り替えられる。Escキーで終了する。
// 「-dx12」引数を付けて起動するとDX12バックエンドになる(再ビルド無しでDX11/DX12を見比べるため)。

#include <Windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <fstream>
#include <string>
#include <vector>

#include "KurenaiEngine2D.h"
#include "KurenaiTypes.h"

using namespace Kurenai;

namespace
{
    // デモ画面。数字キー(1〜)で切り替える。issueごとに1画面ずつ足していく
    enum class DemoScene
    {
        Sprites = 0, // 1: 跳ね回る半透明スプライト(従来のサンプル内容)
        Input,       // 2: 入力(押下エッジ・解放エッジ・ホイール)
        Sound,       // 3: サウンド(ボイス音量のフェード・マスター音量)
        Shapes,      // 4: 図形(角丸矩形の回転)
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
        case DemoScene::Input: return L"2: 入力 (押下エッジ・解放エッジ・ホイール)";
        case DemoScene::Sound: return L"3: サウンド (ボイス音量のフェード・マスター音量)";
        case DemoScene::Shapes: return L"4: 図形 (角丸矩形の回転)";
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

    // サウンドのデモ用に、440Hz・16bitモノラル・44.1kHzのWAVを生成して書き出す。
    // リポジトリに音声ファイルを持たせずにサウンドAPIを動かすため
    // (AudioEngine::LoadSoundはファイルパスしか受け取らないのでメモリ上では渡せない)。
    // 戻り値は書き出せたかどうか
    bool WriteSineWaveWav(const std::wstring& filePath)
    {
        constexpr uint32_t kSampleRate = 44100;
        constexpr uint32_t kSampleCount = kSampleRate; // 1秒
        constexpr float kFrequency = 440.0f;
        // ループ再生でつなぎ目のプチノイズが出ないよう、両端を短くフェードさせる
        constexpr uint32_t kFadeSamples = 512;

        std::vector<int16_t> samples(kSampleCount);
        for (uint32_t i = 0; i < kSampleCount; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
            float gain = 0.3f;
            if (i < kFadeSamples)
            {
                gain *= static_cast<float>(i) / static_cast<float>(kFadeSamples);
            }
            else if (i >= kSampleCount - kFadeSamples)
            {
                gain *= static_cast<float>(kSampleCount - i) / static_cast<float>(kFadeSamples);
            }
            samples[i] = static_cast<int16_t>(std::sinf(2.0f * 3.14159265f * kFrequency * t) * gain * 32767.0f);
        }

        const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
        std::ofstream file(filePath, std::ios::binary);
        if (!file)
        {
            return false;
        }

        const auto writeU32 = [&file](uint32_t value) { file.write(reinterpret_cast<const char*>(&value), 4); };
        const auto writeU16 = [&file](uint16_t value) { file.write(reinterpret_cast<const char*>(&value), 2); };

        file.write("RIFF", 4);
        writeU32(36 + dataBytes);
        file.write("WAVE", 4);
        file.write("fmt ", 4);
        writeU32(16);            // fmtチャンクのサイズ
        writeU16(1);             // WAVE_FORMAT_PCM
        writeU16(1);             // モノラル
        writeU32(kSampleRate);
        writeU32(kSampleRate * 2); // バイト/秒 = サンプリング周波数 × ブロックサイズ
        writeU16(2);             // ブロックサイズ(16bitモノラル)
        writeU16(16);            // ビット深度
        file.write("data", 4);
        writeU32(dataBytes);
        file.write(reinterpret_cast<const char*>(samples.data()), dataBytes);
        return file.good();
    }

    // 「4: 図形」のデモ。elapsedSecondsで回転角を進める
    void DrawShapesDemo(KurenaiEngine2D& renderer, float elapsedSeconds, float width, float height)
    {
        const float angle = elapsedSeconds * 0.6f;

        // 上段: DrawRoundedRectの回転。回しても角丸半径・枠線の太さが変わらないことを見る
        const float rowY = height * 0.62f;
        const float spacing = width / 5.0f;
        renderer.DrawText(width * 0.5f, rowY + 110.0f, L"DrawRoundedRect の回転(角丸半径・枠線の太さは回転で変わらない)",
            20.0f, 0.88f, 0.90f, 0.96f, 1.0f, true);

        for (int i = 0; i < 4; ++i)
        {
            const float x = spacing * (i + 1);
            const float cornerRadius = 4.0f + i * 12.0f;
            renderer.DrawRoundedRect(
                x, rowY, 150.0f, 90.0f, cornerRadius,
                0.18f, 0.24f, 0.34f, 1.0f,
                3.0f, 0.55f, 0.75f, 0.95f, 1.0f,
                angle);
            renderer.DrawText(x, rowY - 90.0f, L"角丸 " + FormatFloat(cornerRadius) + L"px", 16.0f, 0.75f, 0.78f, 0.85f, 1.0f);
        }

        // 下段: 回転が0のときは従来どおりであることの比較用(静止した同じ図形)
        const float compareY = height * 0.28f;
        renderer.DrawText(width * 0.5f, compareY + 90.0f, L"回転なし(既定値0。従来の呼び出しと同じ)",
            20.0f, 0.88f, 0.90f, 0.96f, 1.0f, true);
        for (int i = 0; i < 4; ++i)
        {
            renderer.DrawRoundedRect(
                spacing * (i + 1), compareY, 150.0f, 90.0f, 4.0f + i * 12.0f,
                0.18f, 0.24f, 0.34f, 1.0f,
                3.0f, 0.55f, 0.75f, 0.95f, 1.0f);
        }
    }

    // 「3: サウンド」のデモが持ち越す状態
    struct SoundDemoState
    {
        SoundHandle Sound;
        VoiceHandle Voice;
        bool Available = false;   // WAVの生成・読み込みに成功したか
        bool FadingIn = false;
        float VoiceVolume = 0.0f; // フェードで動かしている、このボイスの音量
    };

    void UpdateSoundDemo(KurenaiEngine2D& renderer, SoundDemoState& state, float deltaTime)
    {
        if (!state.Available)
        {
            return;
        }

        // Spaceでフェードイン/フェードアウトを切り替える。フェードそのものはエンジンに無く、
        // SetVoiceVolumeを毎フレーム呼ぶことでアプリ側が作る
        if (renderer.WasKeyPressed(VK_SPACE))
        {
            if (!state.Voice.IsValid())
            {
                state.VoiceVolume = 0.0f;
                state.Voice = renderer.PlaySound(state.Sound, 0.0f, true); // ループ再生
                state.FadingIn = true;
            }
            else
            {
                state.FadingIn = !state.FadingIn;
            }
        }

        if (state.Voice.IsValid())
        {
            constexpr float kFadeSecondsToFull = 1.5f;
            state.VoiceVolume += (state.FadingIn ? 1.0f : -1.0f) * (deltaTime / kFadeSecondsToFull);
            state.VoiceVolume = (std::max)(0.0f, (std::min)(1.0f, state.VoiceVolume));
            renderer.SetVoiceVolume(state.Voice, state.VoiceVolume);

            // フェードアウトし切ったら止める(再生位置は先頭へ戻る)
            if (!state.FadingIn && state.VoiceVolume <= 0.0f)
            {
                renderer.StopSound(state.Voice);
                state.Voice = VoiceHandle();
            }
        }

        // 左右キーでマスター音量
        const float masterStep = deltaTime * 0.8f;
        if (renderer.IsKeyDown(VK_LEFT))
        {
            renderer.SetMasterVolume((std::max)(0.0f, renderer.GetMasterVolume() - masterStep));
        }
        if (renderer.IsKeyDown(VK_RIGHT))
        {
            renderer.SetMasterVolume((std::min)(1.0f, renderer.GetMasterVolume() + masterStep));
        }
    }

    void DrawSoundDemo(KurenaiEngine2D& renderer, const SoundDemoState& state, float width, float height)
    {
        const float centerX = width * 0.5f;
        float textY = height * 0.62f;
        const auto line = [&](const std::wstring& text, bool bold = false)
        {
            renderer.DrawText(centerX, textY, text, bold ? 22.0f : 18.0f, 0.88f, 0.90f, 0.96f, 1.0f, bold);
            textY -= 32.0f;
        };

        if (!state.Available)
        {
            line(L"デモ用のWAVを用意できなかったため、このデモは無効です", true);
            line(L"(実行ファイルと同じフォルダへ書き込めない場合に起きる)");
            return;
        }

        line(L"Space: BGMのフェードイン / フェードアウト", true);
        line(L"左右キー: マスター音量");

        // 音量をバーで可視化する
        const auto bar = [&](const std::wstring& label, float value, float y)
        {
            constexpr float kBarWidth = 360.0f;
            constexpr float kBarHeight = 20.0f;
            renderer.DrawText(centerX - kBarWidth * 0.5f - 16.0f, y, label, 18.0f, 0.8f, 0.83f, 0.9f, 1.0f, false, TextAlign::Right);
            renderer.DrawRoundedRect(centerX, y, kBarWidth, kBarHeight, 6.0f, 0.16f, 0.18f, 0.24f, 1.0f, 1.0f, 0.4f, 0.45f, 0.55f, 1.0f);
            const float filled = kBarWidth * value;
            if (filled > 0.0f)
            {
                renderer.DrawRoundedRect(
                    centerX - kBarWidth * 0.5f + filled * 0.5f, y, filled, kBarHeight, 6.0f,
                    0.35f, 0.62f, 0.85f, 1.0f);
            }
            renderer.DrawText(centerX + kBarWidth * 0.5f + 16.0f, y, FormatFloat(value), 18.0f, 0.8f, 0.83f, 0.9f, 1.0f, false, TextAlign::Left);
        };

        textY -= 16.0f;
        bar(L"ボイス音量", state.VoiceVolume, textY);
        textY -= 40.0f;
        bar(L"マスター音量", renderer.GetMasterVolume(), textY);
        textY -= 44.0f;
        line(state.Voice.IsValid()
            ? (state.FadingIn ? L"状態: フェードイン中 / 再生中" : L"状態: フェードアウト中")
            : L"状態: 停止");
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

        // サウンドデモ用のWAVを実行ファイルと同じフォルダへ生成して読み込む
        SoundDemoState soundDemo{};
        {
            wchar_t modulePath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            std::wstring wavPath = modulePath;
            const size_t lastSeparator = wavPath.find_last_of(L"\\/");
            wavPath = (lastSeparator == std::wstring::npos ? std::wstring() : wavPath.substr(0, lastSeparator + 1)) + L"SineWave.wav";

            if (WriteSineWaveWav(wavPath))
            {
                soundDemo.Sound = renderer.LoadSound(wavPath);
                soundDemo.Available = soundDemo.Sound.IsValid();
            }
        }

        DemoScene scene = DemoScene::Sprites;
        float elapsedSeconds = 0.0f; // アニメーションするデモ画面の時間軸
        auto lastFrameTime = std::chrono::steady_clock::now();

        while (!renderer.ShouldClose())
        {
            renderer.PumpEvents();

            const auto now = std::chrono::steady_clock::now();
            const float deltaTime = std::chrono::duration<float>(now - lastFrameTime).count();
            lastFrameTime = now;
            elapsedSeconds += deltaTime;

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
            else if (scene == DemoScene::Sound)
            {
                UpdateSoundDemo(renderer, soundDemo, deltaTime);
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
            case DemoScene::Sound:
                DrawSoundDemo(renderer, soundDemo, width, height);
                break;
            case DemoScene::Shapes:
                DrawShapesDemo(renderer, elapsedSeconds, width, height);
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
