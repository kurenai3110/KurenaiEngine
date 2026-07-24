#include "DX11Texture.h"

#include <utility>

namespace Kurenai::RHI
{
    DX11Texture::DX11Texture(
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv,
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv,
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv,
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav,
        std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> mipUavs)
        : m_Srv(std::move(srv))
        , m_Rtv(std::move(rtv))
        , m_Dsv(std::move(dsv))
        , m_Uav(std::move(uav))
        , m_MipUavs(std::move(mipUavs))
    {
    }
}
