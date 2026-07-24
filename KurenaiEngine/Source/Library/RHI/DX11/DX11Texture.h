#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "RHI/IRHITexture.h"

namespace Kurenai::RHI
{
    class DX11Texture : public IRHITexture
    {
    public:
        explicit DX11Texture(
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv,
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv = nullptr,
            Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv = nullptr,
            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav = nullptr);

        ID3D11ShaderResourceView* GetShaderResourceView() const { return m_Srv.Get(); }
        ID3D11RenderTargetView* GetRenderTargetView() const { return m_Rtv.Get(); }
        ID3D11DepthStencilView* GetDepthStencilView() const { return m_Dsv.Get(); }
        // CreateUAVTextureで作成した場合のみ非nullptr(コンピュートシェーダーからのRW用)
        ID3D11UnorderedAccessView* GetUnorderedAccessView() const { return m_Uav.Get(); }

    private:
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_Srv;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_Rtv;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_Dsv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_Uav;
    };
}
