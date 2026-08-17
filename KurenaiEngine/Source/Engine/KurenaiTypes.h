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

    // KurenaiEngine2D::SetSpriteFilterで選ぶ、スプライトを拡大縮小したときのテクスチャの補間方法。
    // RHI::SamplerFilterと同じ選択肢だが、公開APIにRHIの型を出さないため2D向けに別の列挙として持つ
    // (GraphicsAPIと同じ流儀)
    enum class SpriteFilter
    {
        // バイリニア補間。写真的なスプライト・UI素材の既定的な選択
        Linear,
        // 異方性フィルタリング(16x)。KurenaiEngine2Dの既定。
        // 【ミップ付きテクスチャにしか効かない】LoadTextureで読んだ画像はミップが生成されるため
        // 効くが、CreateSolidColorTexture・フォントアトラスはミップ1枚なのでLinearと同じ結果になる
        Anisotropic,
        // 最近傍(点)サンプリング。ドット絵を整数倍に拡大しても輪郭が滲まない。
        // 【整数倍スナップと併用すること】拡大率が実数だとテクセル中心が画素中心からずれ、
        // 点サンプリングでも隣のテクセルを拾って輪郭が1px単位で不均一になる
        // (KurenaiEngine2D::SetVirtualResolutionのsnapToIntegerScale)
        Point,
    };

    // KurenaiEngine2D::SetSpriteAddressModeで選ぶ、UVが0.0〜1.0の外へ出たときの扱い
    enum class SpriteAddressMode
    {
        // UVを繰り返す。KurenaiEngine2Dの既定(タイリングするスプライト向け)
        Wrap,
        // 端のテクセルを引き伸ばす。DrawSpriteUVでアトラスの区画を切り出す場合はこちら。
        // Wrapのままだと区画の端でフィルタのタップが反対側へ回り込み、隣の区画の色が混ざる
        Clamp,
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
