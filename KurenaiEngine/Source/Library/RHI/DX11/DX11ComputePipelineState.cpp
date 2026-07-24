#include "DX11ComputePipelineState.h"

namespace Kurenai::RHI
{
    DX11ComputePipelineState::DX11ComputePipelineState(DX11Shader* computeShader)
        : m_ComputeShader(computeShader)
    {
    }
}
