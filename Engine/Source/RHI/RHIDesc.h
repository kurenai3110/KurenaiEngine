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

        // DX12のパイプラインステートオブジェクト作成時にレンダーターゲット/深度のフォーマットを
        // 事前に確定させる必要があるため保持する。DX11実装では参照しない
        std::vector<Format> RenderTargetFormats;
        bool HasDepthStencil = false;

        // Reverse-Z(深度比較をGREATERにし、近平面=1.0/遠平面=0.0にマッピングする)を使うか。
        // 浮動小数点深度バッファと組み合わせて遠方のZファイティングを抑えるための設定で、
        // 透視投影のメインカメラパスにのみ使う(正射影のシャドウマップは元々Zが線形分布のため対象外)
        bool ReverseZ = false;
    };
}
