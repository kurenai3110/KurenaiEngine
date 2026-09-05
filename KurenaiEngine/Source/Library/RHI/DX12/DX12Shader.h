#pragma once

#include <cstdint>
#include <d3d12.h>
#include <vector>

#include "RHI/IRHIShader.h"
#include "RHI/RHIEnums.h"

namespace Kurenai::RHI
{
    // DX12はパイプラインステートオブジェクトにバイトコードを直接埋め込むため、
    // DX11のようなシェーダオブジェクト(ID3D11VertexShader等)は不要でバイトコードの保持のみでよい。
    //
    // 【ID3DBlobではなくstd::vectorで持つ】バイトコードの出どころが実行時コンパイルの
    // ID3DBlobから、事前コンパイル済みの.kshader(ShaderLoaderが読んだバイト列)へ変わったため。
    // ここでID3DBlobに詰め直す意味は無い(パイプラインステートはポインタと長さしか見ない)
    class DX12Shader : public IRHIShader
    {
    public:
        DX12Shader(ShaderStage stage, std::vector<uint8_t> bytecode);

        ShaderStage GetStage() const { return m_Stage; }
        D3D12_SHADER_BYTECODE GetBytecode() const;

    private:
        ShaderStage m_Stage;
        std::vector<uint8_t> m_Bytecode;
    };
}
