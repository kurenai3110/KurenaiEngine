#include "DX11Texture.h"

#include <string>
#include <utility>

#include "Core/Logger.h"

namespace Kurenai::RHI
{
    namespace
    {
        // 手持ちのビューからリソースを引き、ミップ0の幅・高さを取り出す。
        // DX11Textureはビューしか受け取らない(生成経路が10種類以上ありどれも寸法を渡してこない)ため、
        // 経路ごとに記録するのではなくここで一括して求め、記録漏れが起きないようにする。
        // ID3D11Texture2D以外(バッファのSRV等)は寸法を持たないため0のままになる
        void QueryTextureSize(ID3D11View* view, uint32_t& outWidth, uint32_t& outHeight, uint32_t& outMipLevels)
        {
            if (!view)
            {
                return;
            }

            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            view->GetResource(&resource);
            if (!resource)
            {
                return;
            }

            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture2D;
            if (FAILED(resource.As(&texture2D)))
            {
                return;
            }

            D3D11_TEXTURE2D_DESC desc{};
            texture2D->GetDesc(&desc);
            outWidth = desc.Width;
            outHeight = desc.Height;
            outMipLevels = desc.MipLevels;
        }
    }

    DX11Texture::DX11Texture(
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv,
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv,
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv,
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav,
        std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> mipUavs,
        std::vector<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>> sliceDsvs,
        uint32_t cubeCount)
        : m_Srv(std::move(srv))
        , m_Rtv(std::move(rtv))
        , m_Dsv(std::move(dsv))
        , m_Uav(std::move(uav))
        , m_MipUavs(std::move(mipUavs))
        , m_SliceDsvs(std::move(sliceDsvs))
        , m_CubeCount(cubeCount)
    {
        CaptureDimensionsFromViews();
    }

    void DX11Texture::CaptureDimensionsFromViews()
    {
        m_Width = 0;
        m_Height = 0;
        m_MipLevels = 0;

        // 持っているビューを優先順に試す(SRVを持たないレンダーターゲット/深度テクスチャもあるため)
        QueryTextureSize(m_Srv.Get(), m_Width, m_Height, m_MipLevels);
        if (m_Width == 0)
        {
            QueryTextureSize(m_Rtv.Get(), m_Width, m_Height, m_MipLevels);
        }
        if (m_Width == 0)
        {
            QueryTextureSize(m_Dsv.Get(), m_Width, m_Height, m_MipLevels);
        }
        if (m_Width == 0)
        {
            QueryTextureSize(m_Uav.Get(), m_Width, m_Height, m_MipLevels);
        }
    }

    void DX11Texture::SwapShaderResourceView(Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> newSrv)
    {
        m_Srv = std::move(newSrv);
        CaptureDimensionsFromViews();
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
