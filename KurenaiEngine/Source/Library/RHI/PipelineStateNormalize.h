#pragma once

#include "RHIEnums.h"

// パイプラインステートとサンプラーの「意味の決定」を、DX11とDX12で1箇所に寄せるための層。
//
// 【何をここへ置き、何を置かないのか】置くのは *どう振る舞うと決めたか* であって、
// D3Dの型ではない。BlendMode::Multiply が「元の色 × 描く色」であること、
// Reverse-Z かつ DepthAllowEqual なら比較が GREATER_EQUAL になること、
// 未知の SamplerFilter は警告を出して Linear で代用すること —— これらは
// バックエンドに依らない仕様である。D3D11_BLEND / D3D12_BLEND への最終変換だけを
// 各バックエンドに残す(そこは型が違うだけで判断が無い)。
//
// 【なぜ分けるのか】以前は BlendMode の 5 分岐 × 6 フィールドと、サンプラーの
// フィルタ/アドレスの分岐と警告文が、DX11Device.cpp と DX12Device.cpp に
// 1文字も違わずに書き写されていた。BlendMode を1つ足したときに片方だけ直すと、
// **エラーも警告も出ないまま、DX11とDX12で違う絵が出る**。
//
// このヘッダは IRHI*.h のどれからもインクルードされず、公開インターフェースに
// D3D固有の型を漏らさない(依存は RHIEnums.h だけ)
namespace Kurenai::RHI
{
    // --- ブレンド ---------------------------------------------------------------------------

    // ブレンド係数の意味値。D3D11_BLEND / D3D12_BLEND のどちらの列挙でもない中立表現で、
    // 実際に使っているものだけを持つ(使っていない係数を足しても、どのバックエンドも
    // 変換できずに黙って既定へ落ちるだけなので増やさない)
    enum class BlendFactorValue
    {
        Zero,
        One,
        SrcAlpha,
        InvSrcAlpha,
        DestColor,
        DestAlpha,
    };

    // ブレンド演算の意味値。現状 Add しか使っていないが、
    // 「演算を選んでいる」ことを構造に残すために列挙のまま持つ
    enum class BlendOpValue
    {
        Add,
    };

    // レンダーターゲット1枚ぶんのブレンド設定。D3D11_RENDER_TARGET_BLEND_DESC /
    // D3D12_RENDER_TARGET_BLEND_DESC のうち、このエンジンが実際に決めている項目だけを持つ
    struct BlendStateValues
    {
        // false のとき残りのフィールドは意味を持たない(D3D側もブレンド無効なら参照しない)
        bool Enable = false;
        BlendFactorValue SrcColor = BlendFactorValue::One;
        BlendFactorValue DestColor = BlendFactorValue::Zero;
        BlendOpValue ColorOp = BlendOpValue::Add;
        BlendFactorValue SrcAlpha = BlendFactorValue::One;
        BlendFactorValue DestAlpha = BlendFactorValue::Zero;
        BlendOpValue AlphaOp = BlendOpValue::Add;
    };

    // BlendMode を係数と演算の組へ展開する。
    // 未知の値は BlendMode::Opaque と同じ扱い(ブレンド無効)にする —— 元の実装からの挙動
    BlendStateValues NormalizeBlendMode(BlendMode blendMode);

    // --- 深度比較 ---------------------------------------------------------------------------

    // 深度比較の意味値
    enum class DepthCompareValue
    {
        Less,
        LessEqual,
        Greater,
        GreaterEqual,
    };

    // Reverse-Z(近平面=1.0)なら大きいほうを手前として通すので GREATER 系、
    // 通常のZなら LESS 系。DepthAllowEqual は等値も通すかどうかで、
    // 深度プリパスが書いた値と同じ断片だけを通すために使う(PipelineStateDesc のコメント参照)
    DepthCompareValue NormalizeDepthCompare(bool reverseZ, bool depthAllowEqual);

    // --- サンプラー -------------------------------------------------------------------------

    // フィルタの意味値。SamplerFilter をそのまま使わないのは、
    // 「未知の値を代用に落としたあと」の、必ず変換できる値であることを型で示すため
    enum class SamplerFilterValue
    {
        Point,
        Linear,
        Anisotropic,
    };

    enum class SamplerAddressValue
    {
        Wrap,
        Clamp,
    };

    // 未知の値は警告を出して Linear / Wrap で代用する。
    // backendTag はログのタグ("DX11" / "DX12")。**警告文もここが唯一の出所**で、
    // 片方のバックエンドだけ文面や代用先が変わることを防ぐ
    SamplerFilterValue NormalizeSamplerFilter(SamplerFilter filter, const char* backendTag);
    SamplerAddressValue NormalizeSamplerAddressMode(SamplerAddressMode addressMode, const char* backendTag);
}
