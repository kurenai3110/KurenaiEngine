#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "RHI/IRHITexture.h"

namespace Kurenai::RHI
{
    class DX11Texture : public IRHITexture
    {
    public:
        explicit DX11Texture(Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv);

        ID3D11ShaderResourceView* GetShaderResourceView() const { return m_Srv.Get(); }

    private:
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_Srv;
    };
}
