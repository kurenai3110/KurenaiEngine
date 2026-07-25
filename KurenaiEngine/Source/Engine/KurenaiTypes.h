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
