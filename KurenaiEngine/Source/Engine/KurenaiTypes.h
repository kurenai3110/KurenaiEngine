#pragma once

// KurenaiEngine.dllのエクスポート境界。KurenaiEngine本体をビルドするときのみ
// KURENAI_ENGINE_EXPORTSが定義される(KurenaiEngine.vcxproj参照)。
// 公開クラス(KurenaiEngineBase/KurenaiEngine3D/KurenaiEngine2D)のみがこのマクロを持ち、
// Library/配下の内部実装クラスはDLLからエクスポートされない
#if defined(KURENAI_ENGINE_EXPORTS)
    #define KURENAI_API __declspec(dllexport)
#else
    #define KURENAI_API __declspec(dllimport)
#endif

namespace Kurenai
{
    // 使用するグラフィックスAPIバックエンドの選択(サンプルプログラム向け公開API)
    enum class GraphicsAPI
    {
        DX11,
        DX12,
    };

    enum class MouseButton
    {
        Left,
        Right,
        Middle,
    };

    // KurenaiEngine2D::DrawTextの水平方向の文字揃え。(x, y)の意味がalignごとに変わる
    // (Left: テキスト左下基準/Center: テキスト中央下基準/Right: テキスト右下基準)
    enum class TextAlign
    {
        Left,
        Center,
        Right,
    };

    // KurenaiEngine2D::DrawTextの垂直方向の文字揃え。(x, y)のyの意味がverticalAlignごとに変わる
    // (Bottom: テキスト下端基準(既定、TextAlignのみだった頃と同じ挙動)/Middle: テキスト上下中央基準/
    // Top: テキスト上端基準)
    enum class TextVerticalAlign
    {
        Bottom,
        Middle,
        Top,
    };

    // Win32仮想キーコード(VK_ESCAPE, 'A'〜'Z'など)をそのまま使う。
    // GetAsyncKeyStateから移行する際にキー定数をそのまま流用できるようにするため
    using KeyCode = int;
}
