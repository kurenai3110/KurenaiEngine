#include "DX11PipelineState.h"

#include <utility>

namespace Kurenai::RHI
{
    DX11PipelineState::DX11PipelineState(
        Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout,
        DX11Shader* vertexShader,
        DX11Shader* pixelShader,
        PrimitiveTopology topology,
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState,
        Microsoft::WRL::ComPtr<ID3D11BlendState> blendState)
        : m_InputLayout(std::move(inputLayout))
        , m_VertexShader(vertexShader)
        , m_PixelShader(pixelShader)
        , m_Topology(topology)
        , m_DepthStencilState(std::move(depthStencilState))
        , m_BlendState(std::move(blendState))
    {
    }

    D3D11_PRIMITIVE_TOPOLOGY DX11PipelineState::GetTopology() const
    {
        switch (m_Topology)
        {
        case PrimitiveTopology::TriangleList:
        default:
            return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        }
    }
}
