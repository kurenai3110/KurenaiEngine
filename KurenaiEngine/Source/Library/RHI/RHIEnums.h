#pragma once

namespace Kurenai::RHI
{
    enum class ShaderStage
    {
        Vertex,
        Pixel,
    };

    enum class BufferUsage
    {
        Vertex,
        Index,
        Constant,
    };

    enum class PrimitiveTopology
    {
        TriangleList,
    };

    // パイプラインステートのアルファブレンド設定。2Dスプライトやエフェクト描画で半透明合成を行う場合に指定する
    enum class BlendMode
    {
        Opaque,             // ブレンドなし(不透明。3Dの通常描画はこちら)
        AlphaBlend,         // src.rgb * src.a + dst.rgb * (1 - src.a) の標準アルファブレンド
        Additive,           // src.rgb * src.a + dst.rgb の加算合成(炎・光などの発光エフェクト向け)
        Multiply,           // src.rgb * dst.rgb の乗算合成(影・すりガラスなどの減光エフェクト向け)
        PremultipliedAlpha, // src.rgb + dst.rgb * (1 - src.a) の事前乗算済みアルファブレンド(テクスチャ側でRGBに既にAを乗算済みの場合に使う)
    };

    enum class Format
    {
        R32G32_Float,
        R32G32B32_Float,
        R32G32B32A32_Float,
        R8G8B8A8_UNorm,
    };

    // 使用するグラフィックスAPIバックエンドの選択
    enum class GraphicsAPI
    {
        DX11,
        DX12,
    };
}
