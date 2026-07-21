#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "RHI/IRHIPipelineState.h"
#include "RHI/RHIEnums.h"

namespace Kurenai::RHI
{
    class DX11Shader;

    class DX11PipelineState : public IRHIPipelineState
    {
    public:
        DX11PipelineState(
            Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout,
            DX11Shader* vertexShader,
            DX11Shader* pixelShader,
            PrimitiveTopology topology);

        ID3D11InputLayout* GetInputLayout() const { return m_InputLayout.Get(); }
        DX11Shader* GetVertexShader() const { return m_VertexShader; }
        DX11Shader* GetPixelShader() const { return m_PixelShader; }
        D3D11_PRIMITIVE_TOPOLOGY GetTopology() const;

    private:
        Microsoft::WRL::ComPtr<ID3D11InputLayout> m_InputLayout;
        DX11Shader* m_VertexShader;
        DX11Shader* m_PixelShader;
        PrimitiveTopology m_Topology;
    };
}
