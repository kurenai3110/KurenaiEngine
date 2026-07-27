#include "DX11Texture.h"

#include <string>
#include <utility>

#include "Core/Logger.h"

namespace Kurenai::RHI
{
    DX11Texture::DX11Texture(
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv,
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv,
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv,
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav,
        std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> mipUavs,
        uint32_t cubeCount)
        : m_Srv(std::move(srv))
        , m_Rtv(std::move(rtv))
        , m_Dsv(std::move(dsv))
        , m_Uav(std::move(uav))
        , m_MipUavs(std::move(mipUavs))
        , m_CubeCount(cubeCount)
    {
    }

    ID3D11UnorderedAccessView* DX11Texture::GetCubeUnorderedAccessView(uint32_t face, uint32_t mipLevel, uint32_t cubeIndex) const
    {
        if (face >= kCubeFaceCount || cubeIndex >= m_CubeCount)
        {
            Core::Logger::Error(
                "DX11",
                "GetCubeUnorderedAccessView: 範囲外の指定です (face=" + std::to_string(face) +
                    ", cubeIndex=" + std::to_string(cubeIndex) + ", cubeCount=" + std::to_string(m_CubeCount) + ")");
            return nullptr;
        }

        const size_t index = (static_cast<size_t>(mipLevel) * m_CubeCount + cubeIndex) * kCubeFaceCount + face;
        if (index >= m_MipUavs.size())
        {
            Core::Logger::Error(
                "DX11",
                "GetCubeUnorderedAccessView: ミップレベルが範囲外です (mipLevel=" + std::to_string(mipLevel) +
                    ", UAV数=" + std::to_string(m_MipUavs.size()) + ")");
            return nullptr;
        }

        return m_MipUavs[index].Get();
    }
}
