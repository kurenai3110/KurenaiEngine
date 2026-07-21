#include "DX11Texture.h"

#include <utility>

namespace Kurenai::RHI
{
    DX11Texture::DX11Texture(Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv)
        : m_Srv(std::move(srv))
    {
    }
}
