#include "DX12Shader.h"

#include <utility>

namespace Kurenai::RHI
{
    DX12Shader::DX12Shader(ShaderStage stage, Microsoft::WRL::ComPtr<ID3DBlob> bytecode)
        : m_Stage(stage)
        , m_Bytecode(std::move(bytecode))
    {
    }

    D3D12_SHADER_BYTECODE DX12Shader::GetBytecode() const
    {
        D3D12_SHADER_BYTECODE bytecode{};
        bytecode.pShaderBytecode = m_Bytecode->GetBufferPointer();
        bytecode.BytecodeLength = m_Bytecode->GetBufferSize();
        return bytecode;
    }
}
