#include "DX11Texture.h"

#include <cstring>
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
        void QueryTextureSize(
            ID3D11View* view,
            uint32_t& outWidth,
            uint32_t& outHeight,
            uint32_t& outMipLevels,
            DXGI_FORMAT& outFormat,
            uint32_t& outArraySize)
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
            // 【ビューのフォーマットではなくリソースのフォーマット】深度はリソースが
            // R32_TYPELESSで、DSVはD32_FLOAT・SRVはR32_FLOATという別のフォーマットを持つ。
            // リードバックの受け皿はリソース側に合わせる必要がある
            outFormat = desc.Format;
            outArraySize = desc.ArraySize;
        }

        // 手持ちのビューからリソース本体(ID3D11Texture2D)を引く。優先順はCaptureDimensionsFromViewsと同じ
        Microsoft::WRL::ComPtr<ID3D11Texture2D> QueryTexture2D(ID3D11View* view)
        {
            if (!view)
            {
                return nullptr;
            }

            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            view->GetResource(&resource);
            if (!resource)
            {
                return nullptr;
            }

            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture2D;
            if (FAILED(resource.As(&texture2D)))
            {
                return nullptr;
            }
            return texture2D;
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

    DX11Texture::DX11Texture(
        Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTexture,
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context,
        const TextureReadbackDesc& readbackDesc)
        : m_Width(readbackDesc.Width)
        , m_Height(readbackDesc.Height)
        , m_MipLevels(1)
        , m_StagingTexture(std::move(stagingTexture))
        , m_Context(std::move(context))
        , m_ReadbackDesc(readbackDesc)
    {
        // 【CaptureDimensionsFromViewsを呼ばない】ビューを1つも持たないため何も取れない。
        // 寸法は受け皿を作った時点で分かっているものをそのまま控える
        if (m_StagingTexture)
        {
            D3D11_TEXTURE2D_DESC desc{};
            m_StagingTexture->GetDesc(&desc);
            m_Format = desc.Format;
        }
    }

    void DX11Texture::CaptureDimensionsFromViews()
    {
        m_Width = 0;
        m_Height = 0;
        m_MipLevels = 0;
        m_Format = DXGI_FORMAT_UNKNOWN;
        m_ArraySize = 1;

        // 持っているビューを優先順に試す(SRVを持たないレンダーターゲット/深度テクスチャもあるため)
        QueryTextureSize(m_Srv.Get(), m_Width, m_Height, m_MipLevels, m_Format, m_ArraySize);
        if (m_Width == 0)
        {
            QueryTextureSize(m_Rtv.Get(), m_Width, m_Height, m_MipLevels, m_Format, m_ArraySize);
        }
        if (m_Width == 0)
        {
            QueryTextureSize(m_Dsv.Get(), m_Width, m_Height, m_MipLevels, m_Format, m_ArraySize);
        }
        if (m_Width == 0)
        {
            QueryTextureSize(m_Uav.Get(), m_Width, m_Height, m_MipLevels, m_Format, m_ArraySize);
        }
        // 配列スライスごとのDSVしか持たない深度テクスチャ配列(CreateDepthTextureArray)は
        // 上の4つがどれも空なので、スライス0のDSVから引く
        if (m_Width == 0 && !m_SliceDsvs.empty())
        {
            QueryTextureSize(m_SliceDsvs[0].Get(), m_Width, m_Height, m_MipLevels, m_Format, m_ArraySize);
        }
        // ミップごとのUAVしか持たないテクスチャ(CreateHiZTexture等)も同様にミップ0から引く
        if (m_Width == 0 && !m_MipUavs.empty())
        {
            QueryTextureSize(m_MipUavs[0].Get(), m_Width, m_Height, m_MipLevels, m_Format, m_ArraySize);
        }
    }

    ID3D11Texture2D* DX11Texture::GetTexture2D() const
    {
        if (m_StagingTexture)
        {
            return m_StagingTexture.Get();
        }

        // 【毎回ビューから引き直す】リソースをメンバで持つとSwapShaderResourceViewでの
        // 差し替えと二重管理になり、片方だけ古くなる。呼ばれるのはリードバックのときだけで、
        // 毎フレームの経路には入らないのでコストは問題にならない
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture = QueryTexture2D(m_Srv.Get());
        if (!texture)
        {
            texture = QueryTexture2D(m_Rtv.Get());
        }
        if (!texture)
        {
            texture = QueryTexture2D(m_Dsv.Get());
        }
        if (!texture)
        {
            texture = QueryTexture2D(m_Uav.Get());
        }
        if (!texture && !m_SliceDsvs.empty())
        {
            texture = QueryTexture2D(m_SliceDsvs[0].Get());
        }
        if (!texture && !m_MipUavs.empty())
        {
            texture = QueryTexture2D(m_MipUavs[0].Get());
        }
        // ComPtrはここで解放されるが、リソース本体はビューが握っているので生ポインタは有効
        return texture.Get();
    }

    void DX11Texture::SetDebugName(const char* name)
    {
        ID3D11Texture2D* texture = GetTexture2D();
        if (texture == nullptr || name == nullptr || name[0] == '\0')
        {
            return;
        }

        // D3D11にはSetNameが無く、決められたGUIDのプライベートデータとして文字列を持たせる
        // (dxguid.libをリンク済み。KurenaiEngineLibrary.vcxprojのAdditionalDependencies)。
        // **長さにNUL終端を含めない**(含めるとデバッガ側の表示に終端が混ざる)
        texture->SetPrivateData(
            WKPDID_D3DDebugObjectName, static_cast<UINT>(std::strlen(name)), name);
    }

    TextureReadbackDesc DX11Texture::GetReadbackDesc(uint32_t mipLevel) const
    {
        if (!m_StagingTexture)
        {
            // リードバック用ではないテクスチャ。ElementType::Unknownのまま返す
            return TextureReadbackDesc{};
        }
        if (mipLevel != 0)
        {
            // 受け皿は常に1サブリソースぶん(CreateReadbackTextureの時点でミップを選んである)
            Core::Logger::Error(
                "DX11",
                "GetReadbackDesc: リードバック用テクスチャは常に1サブリソースぶんです "
                "(mipLevel=" + std::to_string(mipLevel) + " が指定されました)");
            return TextureReadbackDesc{};
        }
        return m_ReadbackDesc;
    }

    bool DX11Texture::ReadbackData(void* outData, uint32_t sizeInBytes)
    {
        if (!m_StagingTexture)
        {
            Core::Logger::Error("DX11", "ReadbackData: リードバック用ではないテクスチャから読もうとしました");
            return false;
        }
        if (outData == nullptr || sizeInBytes == 0)
        {
            Core::Logger::Error("DX11", "ReadbackData: 出力先がnullptrかサイズが0です");
            return false;
        }
        if (!m_Context)
        {
            Core::Logger::Error("DX11", "ReadbackData: デバイスコンテキストがありません");
            return false;
        }

        // パディングを剥がしたあとの必要バイト数。呼び出し側にはこれを要求する
        const uint32_t tightRowPitch = m_ReadbackDesc.Width * m_ReadbackDesc.BytesPerTexel;
        const uint64_t tightTotal = static_cast<uint64_t>(tightRowPitch) * m_ReadbackDesc.Height;
        if (sizeInBytes < tightTotal)
        {
            Core::Logger::Error(
                "DX11",
                "ReadbackData: 出力先のサイズ(" + std::to_string(sizeInBytes) + ")が必要量(" +
                    std::to_string(tightTotal) + ")に足りません");
            return false;
        }

        // 【DO_NOT_WAITでGPUを待たない】待つとCPUがGPUに追いつくまで止まり、
        // 計測のために計測対象を変えてしまう(IRHITexture::ReadbackDataのコメント参照)。
        // まだコピーが終わっていなければDXGI_ERROR_WAS_STILL_DRAWINGが返るので、
        // そのフレームは諦めて呼び出し側に判断を返す。**これはエラーではない**のでログは出さない
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr =
            m_Context->Map(m_StagingTexture.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        {
            return false;
        }
        if (FAILED(hr))
        {
            Core::Logger::Error("DX11", "ReadbackData: リードバックテクスチャのMapに失敗しました");
            return false;
        }

        // 【行のパディングをここで剥がす】MapのRowPitchはドライバが決めた値で、
        // 幅×テクセルバイト数より大きいことがある。行ごとにコピーしてタイトに詰め直す
        const auto* src = static_cast<const uint8_t*>(mapped.pData);
        auto* dst = static_cast<uint8_t*>(outData);
        for (uint32_t y = 0; y < m_ReadbackDesc.Height; ++y)
        {
            std::memcpy(
                dst + static_cast<size_t>(y) * tightRowPitch,
                src + static_cast<size_t>(y) * mapped.RowPitch,
                tightRowPitch);
        }

        m_Context->Unmap(m_StagingTexture.Get(), 0);
        return true;
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
