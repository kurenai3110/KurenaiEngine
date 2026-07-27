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
            PrimitiveTopology topology,
            Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState,
            Microsoft::WRL::ComPtr<ID3D11BlendState> blendState,
            Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState);

        ID3D11InputLayout* GetInputLayout() const { return m_InputLayout.Get(); }
        DX11Shader* GetVertexShader() const { return m_VertexShader; }
        DX11Shader* GetPixelShader() const { return m_PixelShader; }
        D3D11_PRIMITIVE_TOPOLOGY GetTopology() const;
        ID3D11DepthStencilState* GetDepthStencilState() const { return m_DepthStencilState.Get(); }
        ID3D11BlendState* GetBlendState() const { return m_BlendState.Get(); }
        ID3D11RasterizerState* GetRasterizerState() const { return m_RasterizerState.Get(); }

    private:
        Microsoft::WRL::ComPtr<ID3D11InputLayout> m_InputLayout;
        DX11Shader* m_VertexShader;
        DX11Shader* m_PixelShader;
        PrimitiveTopology m_Topology;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_DepthStencilState;
        Microsoft::WRL::ComPtr<ID3D11BlendState> m_BlendState;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_RasterizerState;
    };
}
