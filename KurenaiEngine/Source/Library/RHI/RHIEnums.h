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

    // パイプラインステートのアルファブレンド設定。2Dスプライトなど半透明描画を行う場合はAlphaBlendを指定する
    enum class BlendMode
    {
        Opaque,     // ブレンドなし(不透明。3Dの通常描画はこちら)
        AlphaBlend, // src.rgb * src.a + dst.rgb * (1 - src.a) の標準アルファブレンド
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
