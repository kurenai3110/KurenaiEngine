#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "RHI/IRHIShader.h"
#include "RHI/RHIEnums.h"

namespace Kurenai::RHI
{
    // DX12はパイプラインステートオブジェクトにバイトコードを直接埋め込むため、
    // DX11のようなシェーダオブジェクト(ID3D11VertexShader等)は不要でバイトコードの保持のみでよい
    class DX12Shader : public IRHIShader
    {
    public:
        DX12Shader(ShaderStage stage, Microsoft::WRL::ComPtr<ID3DBlob> bytecode);

        ShaderStage GetStage() const { return m_Stage; }
        D3D12_SHADER_BYTECODE GetBytecode() const;

    private:
        ShaderStage m_Stage;
        Microsoft::WRL::ComPtr<ID3DBlob> m_Bytecode;
    };
}
