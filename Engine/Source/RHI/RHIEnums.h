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

    enum class Format
    {
        R32G32_Float,
        R32G32B32_Float,
        R32G32B32A32_Float,
    };
}
