#include "DX11Shader.h"

#include <utility>

namespace Kurenai::RHI
{
    DX11Shader::DX11Shader(ShaderStage stage, Microsoft::WRL::ComPtr<ID3D11DeviceChild> shader, Microsoft::WRL::ComPtr<ID3DBlob> bytecode)
        : m_Stage(stage)
        , m_Shader(std::move(shader))
        , m_Bytecode(std::move(bytecode))
    {
    }

    ID3D11VertexShader* DX11Shader::GetVertexShader() const
    {
        return static_cast<ID3D11VertexShader*>(m_Shader.Get());
    }

    ID3D11PixelShader* DX11Shader::GetPixelShader() const
    {
        return static_cast<ID3D11PixelShader*>(m_Shader.Get());
    }

    ID3D11ComputeShader* DX11Shader::GetComputeShader() const
    {
        return static_cast<ID3D11ComputeShader*>(m_Shader.Get());
    }
}
