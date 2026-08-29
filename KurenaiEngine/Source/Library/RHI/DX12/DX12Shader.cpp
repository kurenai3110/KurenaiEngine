#include "DX12Shader.h"

#include <utility>

namespace Kurenai::RHI
{
    DX12Shader::DX12Shader(ShaderStage stage, std::vector<uint8_t> bytecode)
        : m_Stage(stage)
        , m_Bytecode(std::move(bytecode))
    {
    }

    D3D12_SHADER_BYTECODE DX12Shader::GetBytecode() const
    {
        D3D12_SHADER_BYTECODE bytecode{};
        bytecode.pShaderBytecode = m_Bytecode.data();
        bytecode.BytecodeLength = m_Bytecode.size();
        return bytecode;
    }
}
