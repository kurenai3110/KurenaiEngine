#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "RHI/IRHIPipelineState.h"
#include "RHI/RHIEnums.h"

namespace Kurenai::RHI
{
    class DX12PipelineState : public IRHIPipelineState
    {
    public:
        DX12PipelineState(Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState, PrimitiveTopology topology);

        ID3D12PipelineState* GetPipelineState() const { return m_PipelineState.Get(); }
        D3D12_PRIMITIVE_TOPOLOGY GetTopology() const;

    private:
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_PipelineState;
        PrimitiveTopology m_Topology;
    };
}
