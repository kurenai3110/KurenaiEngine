#pragma once

// KurenaiEngine{Library,3D,2D}.dllのエクスポート境界。DLLごとに独立したマクロを持ち、
// 各.vcxprojはビルド対象のときだけ対応する_EXPORTSマクロを定義する。
// KURENAI_LIB_APIはSource/Library配下とKurenaiEngineBaseに、KURENAI_3D_APIはKurenaiEngine3Dに、
// KURENAI_2D_APIはKurenaiEngine2Dに使う
#if defined(KURENAI_LIBRARY_EXPORTS)
    #define KURENAI_LIB_API __declspec(dllexport)
#else
    #define KURENAI_LIB_API __declspec(dllimport)
#endif

#if defined(KURENAI_ENGINE3D_EXPORTS)
    #define KURENAI_3D_API __declspec(dllexport)
#else
    #define KURENAI_3D_API __declspec(dllimport)
#endif

#if defined(KURENAI_ENGINE2D_EXPORTS)
    #define KURENAI_2D_API __declspec(dllexport)
#else
    #define KURENAI_2D_API __declspec(dllimport)
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

    // KurenaiEngine2D::DrawTextの水平方向の文字揃え。xの意味がalignごとに変わる
    // (Left: テキスト左端基準/Center: テキスト中央基準(既定)/Right: テキスト右端基準)
    enum class TextAlign
    {
        Left,
        Center,
        Right,
    };

    // KurenaiEngine2D::DrawTextの垂直方向の文字揃え。yの意味がverticalAlignごとに変わる
    // (Bottom: テキスト下端基準/Middle: テキスト上下中央基準(既定)/Top: テキスト上端基準)
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
