#pragma once

#include <cstdint>
#include <d3d11.h>
#include <vector>
#include <wrl/client.h>

#include "RHI/IRHIShader.h"
#include "RHI/RHIEnums.h"

namespace Kurenai::RHI
{
    class DX11Shader : public IRHIShader
    {
    public:
        DX11Shader(ShaderStage stage, Microsoft::WRL::ComPtr<ID3D11DeviceChild> shader, std::vector<uint8_t> bytecode);

        ShaderStage GetStage() const { return m_Stage; }
        ID3D11VertexShader* GetVertexShader() const;
        ID3D11PixelShader* GetPixelShader() const;
        ID3D11ComputeShader* GetComputeShader() const;

        // 頂点シェーダーのバイトコード。CreateInputLayoutが入力レイアウトの検証に使うため、
        // シェーダーオブジェクトを作った後も保持し続ける必要がある。
        // 【ID3DBlobではなくstd::vectorで持つ】DX12Shaderと同じ理由(バイトコードの出どころが
        // 実行時コンパイルの結果から事前コンパイル済みの.kshaderへ変わったため)
        const std::vector<uint8_t>& GetBytecode() const { return m_Bytecode; }

    private:
        ShaderStage m_Stage;
        Microsoft::WRL::ComPtr<ID3D11DeviceChild> m_Shader;
        std::vector<uint8_t> m_Bytecode;
    };
}
