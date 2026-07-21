#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "RHIEnums.h"

namespace Kurenai::RHI
{
    struct BufferDesc
    {
        BufferUsage Usage = BufferUsage::Vertex;
        uint32_t SizeInBytes = 0;
        uint32_t StrideInBytes = 0;
        const void* InitialData = nullptr;
    };

    struct ShaderDesc
    {
        ShaderStage Stage = ShaderStage::Vertex;
        std::wstring FilePath;
        std::string EntryPoint;
    };

    struct InputElementDesc
    {
        std::string SemanticName;
        uint32_t SemanticIndex = 0;
        Format Format = Format::R32G32B32_Float;
        uint32_t AlignedByteOffset = 0;
    };

    class IRHIShader;

    struct PipelineStateDesc
    {
        std::vector<InputElementDesc> InputLayout;
        IRHIShader* VertexShader = nullptr;
        IRHIShader* PixelShader = nullptr;
        PrimitiveTopology Topology = PrimitiveTopology::TriangleList;
    };
}
