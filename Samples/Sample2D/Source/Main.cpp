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
        Shapes,      // 4: 図形(角丸矩形の回転・円の枠線)
        Atlas,       // 5: テクスチャアトラス(DrawSpriteUV / GetTextureSize)
        Text,        // 6: テキスト(複数行・行高さ・ブロック計測)
        Camera,      // 7: 2Dカメラ(位置・ズーム・論理解像度)
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
        case DemoScene::Shapes: return L"4: 図形 (角丸矩形の回転・円の枠線)";
        case DemoScene::Atlas: return L"5: テクスチャアトラス (DrawSpriteUV / GetTextureSize)";
        case DemoScene::Text: return L"6: テキスト (複数行・GetLineHeight・MeasureTextBlock)";
        case DemoScene::Camera: return L"7: 2Dカメラ (位置・ズーム・論理解像度)";
        default: return L"(不明なデモ画面)";
        }
    }

    // クライアント座標(左上原点・Y-down)を基準に、画面に貼り付いた大きさでテキストを描く。
    //
    // 2Dの描画APIはワールド座標を取るため、カメラの位置・ズーム・論理解像度を変えると
    // 見出しやHUDまで一緒に動いて拡大される。ClientToWorldで位置を求め、
    // 「クライアント1pxがワールドいくつぶんか」で文字サイズを割ることで画面固定にする
    void DrawScreenText(
        KurenaiEngine2D& renderer, float clientX, float clientY, const std::wstring& text,
        float fontSizePixels, float r, float g, float b, float a,
        bool bold = false, TextAlign align = TextAlign::Left, TextVerticalAlign verticalAlign = TextVerticalAlign::Top)
    {
        // クライアント座標で100px下がワールド座標でいくつぶんかを測る
        constexpr float kProbePixels = 100.0f;
        float originX = 0.0f, originY = 0.0f;
        float probeX = 0.0f, probeY = 0.0f;
        renderer.ClientToWorld(clientX, clientY, originX, originY);
        renderer.ClientToWorld(clientX, clientY + kProbePixels, probeX, probeY);
        const float worldPerPixel = (originY - probeY) / kProbePixels;
        if (!(worldPerPixel > 0.0f))
        {
            return; // クライアント領域が0のフレーム
        }

        renderer.DrawText(originX, originY, text, fontSizePixels * worldPerPixel, r, g, b, a, bold, align, verticalAlign);
    }

    // 描画できるクライアント矩形(レターボックス適用後のビューポート)。
    //
    // SetVirtualResolutionを使うと余白(レターボックス/ピラーボックス)はビューポートの外になり、
    // そこへ向けて描いたものは投影のクリップで消える。画面に貼り付けるHUDはこの矩形の内側へ置く。
    // 計算はKurenaiEngine2D::ComputeViewStateと同じ「アスペクト比を保って収める」式
    struct DrawableRect
    {
        float X = 0.0f, Y = 0.0f, Width = 0.0f, Height = 0.0f;
    };

    DrawableRect GetDrawableClientRect(float clientWidth, float clientHeight, float virtualWidth, float virtualHeight)
    {
        DrawableRect rect{ 0.0f, 0.0f, clientWidth, clientHeight };
        if (virtualWidth > 0.0f && virtualHeight > 0.0f && clientWidth > 0.0f && clientHeight > 0.0f)
        {
            const float scale = (std::min)(clientWidth / virtualWidth, clientHeight / virtualHeight);
            rect.Width = virtualWidth * scale;
            rect.Height = virtualHeight * scale;
            rect.X = (clientWidth - rect.Width) * 0.5f;
            rect.Y = (clientHeight - rect.Height) * 0.5f;
        }
        return rect;
    }

    // 画面上端に、選択中のデモ画面名と操作説明を出す(全デモ画面で共通)
    void DrawHeader(KurenaiEngine2D& renderer, DemoScene scene, GraphicsAPI api, const DrawableRect& drawable)
    {
        const std::wstring title = std::wstring(api == GraphicsAPI::DX12 ? L"[DX12] " : L"[DX11] ") + GetSceneTitle(scene);
        DrawScreenText(renderer, drawable.X + 16.0f, drawable.Y + 12.0f, title, 22.0f, 1.0f, 1.0f, 1.0f, 1.0f, true);
        DrawScreenText(renderer, drawable.X + 16.0f, drawable.Y + 40.0f, L"数字キー: デモ切り替え / Esc: 終了", 16.0f, 0.7f, 0.7f, 0.75f, 1.0f);
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

    // アトラスのデモ用に、4x4の区画に分けた24bit BMPを書き出す。区画ごとに色を変え、
    // 左上へ向かうグラデーションを付けてDrawSpriteUVで切り出した向きが分かるようにする。
    // リポジトリに画像ファイルを持たせずにアトラスの部分描画を示すため
    // (LoadTextureはWIC経由なのでBMPをそのまま読める)
    constexpr uint32_t kAtlasCellCount = 4;   // 1辺あたりの区画数
    constexpr uint32_t kAtlasCellPixels = 64; // 1区画のピクセル数

    bool WriteAtlasBmp(const std::wstring& filePath)
    {
        constexpr uint32_t kSize = kAtlasCellCount * kAtlasCellPixels;
        // 24bit BMPは行を4バイト境界へ揃える必要がある(kSize=256なので256*3=768で既に揃っている)
        constexpr uint32_t kRowBytes = kSize * 3;
        std::vector<uint8_t> pixels(static_cast<size_t>(kRowBytes) * kSize, 0);

        for (uint32_t y = 0; y < kSize; ++y)
        {
            for (uint32_t x = 0; x < kSize; ++x)
            {
                const uint32_t cellX = x / kAtlasCellPixels;
                const uint32_t cellY = y / kAtlasCellPixels;
                const uint32_t index = cellY * kAtlasCellCount + cellX;

                // 区画ごとに色相をずらした色。区画の境界が分かるよう外周1pxは暗くする
                const float hue = static_cast<float>(index) / static_cast<float>(kAtlasCellCount * kAtlasCellCount);
                float rf = 0.5f + 0.5f * std::sinf(hue * 6.2831853f);
                float gf = 0.5f + 0.5f * std::sinf(hue * 6.2831853f + 2.0944f);
                float bf = 0.5f + 0.5f * std::sinf(hue * 6.2831853f + 4.1888f);

                // 区画内での位置に応じた明暗。左上が明るく右下が暗いので、切り出した向きが分かる
                const float localX = static_cast<float>(x % kAtlasCellPixels) / static_cast<float>(kAtlasCellPixels);
                const float localY = static_cast<float>(y % kAtlasCellPixels) / static_cast<float>(kAtlasCellPixels);
                const float shade = 1.0f - 0.6f * (localX + localY) * 0.5f;
                rf *= shade;
                gf *= shade;
                bf *= shade;

                const bool isEdge =
                    (x % kAtlasCellPixels) == 0 || (x % kAtlasCellPixels) == kAtlasCellPixels - 1 ||
                    (y % kAtlasCellPixels) == 0 || (y % kAtlasCellPixels) == kAtlasCellPixels - 1;
                if (isEdge)
                {
                    rf *= 0.25f;
                    gf *= 0.25f;
                    bf *= 0.25f;
                }

                // BMPはボトムアップ(先頭行が画像の最下行)なのでyを反転して書く
                const size_t offset = static_cast<size_t>(kSize - 1 - y) * kRowBytes + static_cast<size_t>(x) * 3;
                pixels[offset + 0] = static_cast<uint8_t>(bf * 255.0f); // BGRの順
                pixels[offset + 1] = static_cast<uint8_t>(gf * 255.0f);
                pixels[offset + 2] = static_cast<uint8_t>(rf * 255.0f);
            }
        }

        std::ofstream file(filePath, std::ios::binary);
        if (!file)
        {
            return false;
        }

        const auto writeU32 = [&file](uint32_t value) { file.write(reinterpret_cast<const char*>(&value), 4); };
        const auto writeU16 = [&file](uint16_t value) { file.write(reinterpret_cast<const char*>(&value), 2); };
        const uint32_t dataBytes = static_cast<uint32_t>(pixels.size());

        file.write("BM", 2);                 // BITMAPFILEHEADER
        writeU32(14 + 40 + dataBytes);       // ファイル全体のサイズ
        writeU16(0);
        writeU16(0);
        writeU32(14 + 40);                   // 画素データまでのオフセット
        writeU32(40);                        // BITMAPINFOHEADERのサイズ
        writeU32(kSize);
        writeU32(kSize);
        writeU16(1);                         // プレーン数
        writeU16(24);                        // ビット深度
        writeU32(0);                         // BI_RGB(無圧縮)
        writeU32(dataBytes);
        writeU32(2835);                      // 解像度(72dpi相当。表示には影響しない)
        writeU32(2835);
        writeU32(0);
        writeU32(0);
        file.write(reinterpret_cast<const char*>(pixels.data()), dataBytes);
        return file.good();
    }

    // 「5: テクスチャアトラス」のデモ
    void DrawAtlasDemo(KurenaiEngine2D& renderer, TextureHandle atlas, float width, float height)
    {
        if (!atlas.IsValid())
        {
            renderer.DrawText(width * 0.5f, height * 0.5f, L"アトラス用のBMPを用意できなかったため、このデモは無効です",
                20.0f, 0.88f, 0.90f, 0.96f, 1.0f, true);
            return;
        }

        uint32_t atlasWidth = 0;
        uint32_t atlasHeight = 0;
        renderer.GetTextureSize(atlas, atlasWidth, atlasHeight);

        renderer.DrawText(width * 0.5f, height * 0.86f,
            L"GetTextureSize: " + std::to_wstring(atlasWidth) + L" x " + std::to_wstring(atlasHeight) + L" px",
            20.0f, 0.88f, 0.90f, 0.96f, 1.0f, true);

        // 左: DrawSpriteでアトラス全体を表示
        const float wholeSize = 256.0f;
        const float wholeX = width * 0.25f;
        const float wholeY = height * 0.5f;
        renderer.DrawText(wholeX, wholeY + wholeSize * 0.5f + 24.0f, L"DrawSprite (テクスチャ全体)", 18.0f, 0.8f, 0.83f, 0.9f, 1.0f);
        renderer.DrawSprite(wholeX, wholeY, wholeSize, wholeSize, 0.0f, atlas, 1.0f, 1.0f, 1.0f, 1.0f);

        // 右: DrawSpriteUVで区画を1つずつ、ピクセル矩形から正規化UVを求めて切り出す
        const float cellDrawSize = 56.0f;
        const float gridOriginX = width * 0.68f;
        const float gridOriginY = height * 0.5f + cellDrawSize * 1.5f;
        renderer.DrawText(gridOriginX + cellDrawSize * 1.5f, gridOriginY + cellDrawSize * 0.5f + 24.0f,
            L"DrawSpriteUV (区画ごとに切り出して並べ替え)", 18.0f, 0.8f, 0.83f, 0.9f, 1.0f);

        if (atlasWidth == 0 || atlasHeight == 0)
        {
            return;
        }

        for (uint32_t cellY = 0; cellY < kAtlasCellCount; ++cellY)
        {
            for (uint32_t cellX = 0; cellX < kAtlasCellCount; ++cellX)
            {
                // ピクセル矩形 -> 正規化UV
                const float u0 = static_cast<float>(cellX * kAtlasCellPixels) / static_cast<float>(atlasWidth);
                const float v0 = static_cast<float>(cellY * kAtlasCellPixels) / static_cast<float>(atlasHeight);
                const float u1 = static_cast<float>((cellX + 1) * kAtlasCellPixels) / static_cast<float>(atlasWidth);
                const float v1 = static_cast<float>((cellY + 1) * kAtlasCellPixels) / static_cast<float>(atlasHeight);

                // 左右を反転して並べ、切り出しが区画単位で効いていることを分かりやすくする
                const uint32_t drawX = kAtlasCellCount - 1 - cellX;
                renderer.DrawSpriteUV(
                    gridOriginX + drawX * (cellDrawSize + 6.0f),
                    gridOriginY - cellY * (cellDrawSize + 6.0f),
                    cellDrawSize, cellDrawSize, 0.0f,
                    atlas, u0, v0, u1, v1,
                    1.0f, 1.0f, 1.0f, 1.0f);
            }
        }
    }

    // 「7: 2Dカメラ」のデモ。論理解像度の指定を切り替えるためのフラグだけ持つ
    struct CameraDemoState
    {
        bool UseVirtualResolution = false;
    };

    // このデモが使う論理解像度。ウィンドウ(既定16:9)に対して4:3にしてあるので、
    // 有効にすると左右にピラーボックスが出て効いていることがすぐ分かる
    constexpr float kVirtualWidth = 800.0f;
    constexpr float kVirtualHeight = 600.0f;

    void UpdateCameraDemo(KurenaiEngine2D& renderer, CameraDemoState& state, float deltaTime)
    {
        // ホイールでズーム。1ノッチあたり1.15倍
        const float wheel = renderer.GetMouseWheelDelta();
        if (wheel != 0.0f)
        {
            renderer.SetCameraZoom(renderer.GetCameraZoom() * std::powf(1.15f, wheel));
        }

        // WASD/矢印キーでカメラを動かす。移動量はズームに反比例させ、
        // 画面上の移動速度が拡大率によらず一定になるようにする
        float cameraX = 0.0f;
        float cameraY = 0.0f;
        renderer.GetCameraPosition(cameraX, cameraY);
        const float speed = 400.0f * deltaTime / renderer.GetCameraZoom();
        if (renderer.IsKeyDown('A') || renderer.IsKeyDown(VK_LEFT)) { cameraX -= speed; }
        if (renderer.IsKeyDown('D') || renderer.IsKeyDown(VK_RIGHT)) { cameraX += speed; }
        if (renderer.IsKeyDown('S') || renderer.IsKeyDown(VK_DOWN)) { cameraY -= speed; }
        if (renderer.IsKeyDown('W') || renderer.IsKeyDown(VK_UP)) { cameraY += speed; }
        renderer.SetCameraPosition(cameraX, cameraY);

        // Vで論理解像度の指定を入り切りする
        if (renderer.WasKeyPressed('V'))
        {
            state.UseVirtualResolution = !state.UseVirtualResolution;
            if (state.UseVirtualResolution)
            {
                renderer.SetVirtualResolution(kVirtualWidth, kVirtualHeight);
            }
            else
            {
                renderer.SetVirtualResolution(0.0f, 0.0f); // 解除
            }
        }

        // Rで既定へ戻す
        if (renderer.WasKeyPressed('R'))
        {
            state.UseVirtualResolution = false;
            renderer.SetVirtualResolution(0.0f, 0.0f);
            renderer.SetCameraZoom(1.0f);
            renderer.SetCameraPosition(kVirtualWidth * 0.5f, kVirtualHeight * 0.5f);
        }
    }

    void DrawCameraDemo(KurenaiEngine2D& renderer, const CameraDemoState& state, const DrawableRect& drawable)
    {
        // 論理解像度(800x600)の範囲を示す枠と、100px間隔の格子。
        // カメラを動かしてもワールド座標に固定されているので、パン・ズームが目に見える
        renderer.DrawRoundedRect(
            kVirtualWidth * 0.5f, kVirtualHeight * 0.5f, kVirtualWidth, kVirtualHeight, 0.0f,
            0.10f, 0.12f, 0.18f, 1.0f, 2.0f, 0.55f, 0.65f, 0.85f, 1.0f);

        for (int i = 0; i <= 8; ++i)
        {
            const float x = i * 100.0f;
            renderer.DrawLine(x, 0.0f, x, kVirtualHeight, 1.0f, 0.35f, 0.40f, 0.50f, 1.0f);
        }
        for (int i = 0; i <= 6; ++i)
        {
            const float y = i * 100.0f;
            renderer.DrawLine(0.0f, y, kVirtualWidth, y, 1.0f, 0.35f, 0.40f, 0.50f, 1.0f);
        }

        // 格子の交点にワールド座標を書く(ズームすると文字も一緒に拡大される)
        for (int gx = 0; gx <= 8; gx += 2)
        {
            for (int gy = 0; gy <= 6; gy += 2)
            {
                renderer.DrawText(
                    gx * 100.0f, gy * 100.0f,
                    L"(" + std::to_wstring(gx * 100) + L", " + std::to_wstring(gy * 100) + L")",
                    14.0f, 0.65f, 0.70f, 0.80f, 1.0f);
            }
        }

        // マウス位置(ClientToWorld経由)にリングを出す。ズーム・パン・レターボックスを
        // すべて考慮した変換になっていれば、カーソルへ正確に追従する
        float mouseWorldX = 0.0f;
        float mouseWorldY = 0.0f;
        renderer.GetMouseWorldPosition(mouseWorldX, mouseWorldY);
        renderer.DrawCircle(mouseWorldX, mouseWorldY, 24.0f, 0.0f, 0.0f, 0.0f, 0.0f, 3.0f, 0.95f, 0.75f, 0.30f, 1.0f);

        float cameraX = 0.0f;
        float cameraY = 0.0f;
        renderer.GetCameraPosition(cameraX, cameraY);
        const std::wstring info =
            L"WASD/矢印: カメラ移動  ホイール: ズーム  V: 論理解像度 " +
                std::wstring(state.UseVirtualResolution ? L"[ON]" : L"[OFF]") + L"  R: 既定へ戻す\n" +
            L"カメラ中心 (" + FormatFloat(cameraX) + L", " + FormatFloat(cameraY) + L")" +
            L"  ズーム " + FormatFloat(renderer.GetCameraZoom()) + L"倍\n" +
            L"マウスのワールド座標 (" + FormatFloat(mouseWorldX) + L", " + FormatFloat(mouseWorldY) + L")";
        // 情報表示はカメラ操作で動かないよう画面へ貼り付ける(ClientToWorld経由)
        DrawScreenText(
            renderer, drawable.X + 16.0f, drawable.Y + drawable.Height - 90.0f, info, 18.0f, 0.88f, 0.90f, 0.96f, 1.0f,
            false, TextAlign::Left, TextVerticalAlign::Top);
    }

    // 「6: テキスト」のデモ。MeasureTextBlockで測った大きさのパネルへ複数行テキストを収める
    void DrawTextDemo(KurenaiEngine2D& renderer, float width, float height)
    {
        const std::wstring body =
            L"ユニットの説明文をパネル内へ折り返して表示する例。\n"
            L"'\\n' で改行され、2行目以降は GetLineHeight ぶん下へ送られる。\n"
            L"align は行ごとに、verticalAlign はブロック全体に適用される。\n"
            L"パネルの大きさは MeasureTextBlock の戻り値から決めている。";

        constexpr float kBodyFontSize = 20.0f;
        constexpr float kPadding = 20.0f;

        float blockWidth = 0.0f;
        float blockHeight = 0.0f;
        renderer.MeasureTextBlock(body, kBodyFontSize, blockWidth, blockHeight);

        // 測ったブロックの大きさ + 余白でパネルを描き、そこへ文字を収める
        const float panelX = width * 0.5f;
        const float panelY = height * 0.55f;
        renderer.DrawRoundedRect(
            panelX, panelY, blockWidth + kPadding * 2.0f, blockHeight + kPadding * 2.0f, 10.0f,
            0.14f, 0.17f, 0.24f, 1.0f,
            2.0f, 0.45f, 0.55f, 0.72f, 1.0f);
        renderer.DrawText(panelX, panelY, body, kBodyFontSize, 0.88f, 0.90f, 0.96f, 1.0f);

        renderer.DrawText(
            panelX, panelY + blockHeight * 0.5f + kPadding + 26.0f,
            L"MeasureTextBlock: " + FormatFloat(blockWidth) + L" x " + FormatFloat(blockHeight) + L" px" +
                L" / GetLineHeight: " + FormatFloat(renderer.GetLineHeight(kBodyFontSize)) + L" px",
            18.0f, 0.75f, 0.78f, 0.85f, 1.0f);

        // align の効き方(行ごとに適用される)を3種類並べて見せる
        const std::wstring aligned = L"1行目\n2行目はすこし長い\n3行目";
        const float sampleY = height * 0.22f;
        const struct { const wchar_t* Label; TextAlign Align; float X; } samples[] = {
            { L"align = Left",   TextAlign::Left,   width * 0.18f },
            { L"align = Center", TextAlign::Center, width * 0.50f },
            { L"align = Right",  TextAlign::Right,  width * 0.82f },
        };
        for (const auto& sample : samples)
        {
            renderer.DrawText(sample.X, sampleY + 60.0f, sample.Label, 16.0f, 0.75f, 0.78f, 0.85f, 1.0f);
            renderer.DrawText(sample.X, sampleY, aligned, 18.0f, 0.85f, 0.90f, 0.70f, 1.0f, false, sample.Align);
            // 基準線。alignがどの位置を基準にしているかが分かる
            renderer.DrawLine(sample.X, sampleY - 44.0f, sample.X, sampleY + 44.0f, 1.0f, 0.9f, 0.4f, 0.4f, 0.6f);
        }
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

        // 下段: DrawCircleの枠線。塗りなし(a=0)+枠線がリングになることを見る。
        // 背景として色つきの帯を敷き、リングの内側が透けていることを分かるようにする
        const float circleY = height * 0.28f;
        renderer.DrawText(width * 0.5f, circleY + 110.0f, L"DrawCircle の枠線(塗りa=0 + 枠線ありでリングになる)",
            20.0f, 0.88f, 0.90f, 0.96f, 1.0f, true);
        renderer.DrawRoundedRect(width * 0.5f, circleY, width * 0.9f, 40.0f, 0.0f, 0.45f, 0.30f, 0.16f, 1.0f);

        struct CircleStyle
        {
            const wchar_t* Label;
            float FillA;
            float BorderThickness;
        };
        const CircleStyle styles[] = {
            { L"塗りのみ",           1.0f, 0.0f },
            { L"塗り + 枠線",        1.0f, 4.0f },
            { L"半透明の塗り + 枠線", 0.35f, 4.0f },
            { L"塗りなし + 枠線",     0.0f, 4.0f },
        };
        for (int i = 0; i < 4; ++i)
        {
            const float x = spacing * (i + 1);
            renderer.DrawCircle(
                x, circleY, 52.0f,
                0.20f, 0.42f, 0.70f, styles[i].FillA,
                styles[i].BorderThickness, 0.95f, 0.85f, 0.35f, 1.0f);
            renderer.DrawText(x, circleY - 80.0f, styles[i].Label, 16.0f, 0.75f, 0.78f, 0.85f, 1.0f);
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

        // デモ用のアセット(正弦波WAV・アトラスBMP)を実行ファイルと同じフォルダへ生成して読み込む。
        // リポジトリにバイナリ資産を持たせないための措置
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        const std::wstring moduleDirectory = [&modulePath]()
        {
            const std::wstring full = modulePath;
            const size_t lastSeparator = full.find_last_of(L"\\/");
            return lastSeparator == std::wstring::npos ? std::wstring() : full.substr(0, lastSeparator + 1);
        }();

        SoundDemoState soundDemo{};
        if (WriteSineWaveWav(moduleDirectory + L"SineWave.wav"))
        {
            soundDemo.Sound = renderer.LoadSound(moduleDirectory + L"SineWave.wav");
            soundDemo.Available = soundDemo.Sound.IsValid();
        }

        TextureHandle atlasTexture;
        if (WriteAtlasBmp(moduleDirectory + L"DemoAtlas.bmp"))
        {
            atlasTexture = renderer.LoadTexture(moduleDirectory + L"DemoAtlas.bmp");
        }

        CameraDemoState cameraDemo{};

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
                    const DemoScene next = static_cast<DemoScene>(i);
                    if (scene == DemoScene::Camera && next != DemoScene::Camera)
                    {
                        // カメラのデモを抜けるときはズームと論理解像度を戻す。カメラ状態は
                        // エンジン側が保持し続けるため、戻さないと他のデモ画面がずれたままになる
                        // (カメラ位置は下のループで毎フレーム入れ直している)
                        cameraDemo = CameraDemoState{};
                        renderer.SetVirtualResolution(0.0f, 0.0f);
                        renderer.SetCameraZoom(1.0f);
                    }
                    else if (next == DemoScene::Camera && scene != DemoScene::Camera)
                    {
                        // 論理解像度(800x600)の中央から始める
                        renderer.SetCameraPosition(kVirtualWidth * 0.5f, kVirtualHeight * 0.5f);
                    }
                    scene = next;
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
            else if (scene == DemoScene::Camera)
            {
                UpdateCameraDemo(renderer, cameraDemo, deltaTime);
            }

            if (scene != DemoScene::Camera)
            {
                // カメラのデモ以外は既定の見え方(クライアント領域を過不足なく映す)に固定する。
                // 一度SetCameraPositionを呼ぶとウィンドウサイズへの自動追従が止まるため、
                // リサイズにも追従するよう毎フレーム入れ直す
                renderer.SetCameraPosition(width * 0.5f, height * 0.5f);
            }

            // HUDを置ける範囲(レターボックスの余白の外へ描くとクリップされる)
            const DrawableRect drawable = (scene == DemoScene::Camera && cameraDemo.UseVirtualResolution)
                ? GetDrawableClientRect(width, height, kVirtualWidth, kVirtualHeight)
                : GetDrawableClientRect(width, height, 0.0f, 0.0f);

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
            case DemoScene::Atlas:
                DrawAtlasDemo(renderer, atlasTexture, width, height);
                break;
            case DemoScene::Text:
                DrawTextDemo(renderer, width, height);
                break;
            case DemoScene::Camera:
                DrawCameraDemo(renderer, cameraDemo, drawable);
                break;
            default:
                break;
            }

            DrawHeader(renderer, scene, api, drawable);
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
