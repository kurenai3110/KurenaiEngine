#include "DX12PipelineState.h"

#include <utility>

namespace Kurenai::RHI
{
    DX12PipelineState::DX12PipelineState(Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState, PrimitiveTopology topology)
        : m_PipelineState(std::move(pipelineState))
        , m_Topology(topology)
    {
    }

    D3D12_PRIMITIVE_TOPOLOGY DX12PipelineState::GetTopology() const
    {
        switch (m_Topology)
        {
        case PrimitiveTopology::TriangleList:
        default:
            return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        }
    }
}
