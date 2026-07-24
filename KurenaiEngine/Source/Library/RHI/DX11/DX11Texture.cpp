#include "DX11Texture.h"

#include <utility>

namespace Kurenai::RHI
{
    DX11Texture::DX11Texture(
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv,
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv,
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv)
        : m_Srv(std::move(srv))
        , m_Rtv(std::move(rtv))
        , m_Dsv(std::move(dsv))
    {
    }
}
