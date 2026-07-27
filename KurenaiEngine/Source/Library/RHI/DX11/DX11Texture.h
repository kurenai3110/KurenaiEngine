#pragma once

#include <d3d11.h>
#include <vector>
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
            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav = nullptr,
            std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> mipUavs = {},
            uint32_t cubeCount = 1);

        ID3D11ShaderResourceView* GetShaderResourceView() const { return m_Srv.Get(); }
        ID3D11RenderTargetView* GetRenderTargetView() const { return m_Rtv.Get(); }
        ID3D11DepthStencilView* GetDepthStencilView() const { return m_Dsv.Get(); }
        // CreateUAVTexture/CreateHiZTextureで作成した場合のみ非nullptr(コンピュートシェーダーからのRW用)。
        // CreateHiZTextureのミップチェーンテクスチャはミップごとに個別のUAVを持つためmipLevelで選択する
        // (CreateUAVTextureは常に1ミップのみなので既定値の0で単一UAVが返る)
        ID3D11UnorderedAccessView* GetUnorderedAccessView(uint32_t mipLevel = 0) const
        {
            return m_MipUavs.empty() ? m_Uav.Get() : m_MipUavs[mipLevel].Get();
        }

        // キューブマップ(CreateUAVTextureCube/CreateMippedUAVTextureCube/
        // CreateMippedUAVTextureCubeArray)専用。m_MipUavsに (mip*cubeCount + cubeIndex)*kCubeFaceCount + face
        // の順でフラットに格納しているため、この3引数版で個別のキューブ・面・ミップのUAVを取り出す。
        // 範囲外を指定した場合はログを出してnullptrを返す(不正なUAVをバインドしてGPU側の
        // 未定義動作を招くより、バインドを空にして描画結果の異常として現れる方が原因を追いやすい)
        static constexpr uint32_t kCubeFaceCount = 6;
        ID3D11UnorderedAccessView* GetCubeUnorderedAccessView(uint32_t face, uint32_t mipLevel = 0, uint32_t cubeIndex = 0) const;

    private:
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_Srv;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_Rtv;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_Dsv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_Uav;
        std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> m_MipUavs;
        // キューブマップ配列の枚数(非キューブマップ・単一キューブは1)。m_MipUavsのフラット添字計算に使う
        uint32_t m_CubeCount = 1;
    };
}
