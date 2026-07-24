#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "RHI/IRHIShader.h"
#include "RHI/RHIEnums.h"

namespace Kurenai::RHI
{
    class DX11Shader : public IRHIShader
    {
    public:
        DX11Shader(ShaderStage stage, Microsoft::WRL::ComPtr<ID3D11DeviceChild> shader, Microsoft::WRL::ComPtr<ID3DBlob> bytecode);

        ShaderStage GetStage() const { return m_Stage; }
        ID3D11VertexShader* GetVertexShader() const;
        ID3D11PixelShader* GetPixelShader() const;
        ID3DBlob* GetBytecode() const { return m_Bytecode.Get(); }

    private:
        ShaderStage m_Stage;
        Microsoft::WRL::ComPtr<ID3D11DeviceChild> m_Shader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_Bytecode;
    };
}
