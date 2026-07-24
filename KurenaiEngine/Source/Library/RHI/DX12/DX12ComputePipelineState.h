#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "RHI/IRHIPipelineState.h"

namespace Kurenai::RHI
{
    class DX12ComputePipelineState : public IRHIPipelineState
    {
    public:
        explicit DX12ComputePipelineState(Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState);

        ID3D12PipelineState* GetPipelineState() const { return m_PipelineState.Get(); }

    private:
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_PipelineState;
    };
}
