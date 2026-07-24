#include "DX12ComputePipelineState.h"

#include <utility>

namespace Kurenai::RHI
{
    DX12ComputePipelineState::DX12ComputePipelineState(Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState)
        : m_PipelineState(std::move(pipelineState))
    {
    }
}
