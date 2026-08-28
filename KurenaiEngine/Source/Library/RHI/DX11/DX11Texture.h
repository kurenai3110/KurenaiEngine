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
            std::vector<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>> sliceDsvs = {},
            uint32_t cubeCount = 1);

        ID3D11ShaderResourceView* GetShaderResourceView() const { return m_Srv.Get(); }
        ID3D11RenderTargetView* GetRenderTargetView() const { return m_Rtv.Get(); }
        // CreateDepthTextureArrayで作成した場合のみスライスごとの個別DSVを持ち、arraySliceで選択する
        // (通常のCreateDepthTextureは単一DSVのため既定値の0でそのDSVが返る)。
        // 範囲外はnullptrを返し、呼び出し側(DX11CommandList::SetRenderTargets)がログを出す
        ID3D11DepthStencilView* GetDepthStencilView(uint32_t arraySlice = 0) const
        {
            if (m_SliceDsvs.empty())
            {
                return m_Dsv.Get();
            }
            return arraySlice < m_SliceDsvs.size() ? m_SliceDsvs[arraySlice].Get() : nullptr;
        }
        // 深度スライス数(0 = テクスチャ配列ではない通常の深度テクスチャ)。範囲外指定時のログ用
        uint32_t GetDepthSliceCount() const { return static_cast<uint32_t>(m_SliceDsvs.size()); }
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

        uint32_t GetWidth() const override { return m_Width; }
        uint32_t GetHeight() const override { return m_Height; }
        uint32_t GetMipLevels() const override { return m_MipLevels; }

        // レンダーターゲット・深度・UAVのいずれかを持つか。ストリーミングの差し替え対象は
        // SRVしか持たないアセット由来のテクスチャに限るため、その判定に使う
        bool HasNonSrvViews() const
        {
            return m_Rtv || m_Dsv || m_Uav || !m_MipUavs.empty() || !m_SliceDsvs.empty();
        }

        // SRVを差し替える。IRHITextureのオブジェクト同一性は保たれるため、
        // Assets::Meshが持つIRHITexture*の生ポインタを貼り替えずに済む。
        //
        // 【古いテクスチャの寿命】DX11は「GPUが参照し終えるまで実体の破棄を遅らせる」ことを
        // ランタイムが保証しているため、ここでComPtrを手放してよい(DX12は自前で
        // Nフレーム遅延解放する必要がある。DX12Device::RetireResource参照)
        void SwapShaderResourceView(Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> newSrv);

    private:
        // 手持ちのビューからm_Width/m_Height/m_MipLevelsを取り直す
        void CaptureDimensionsFromViews();

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_Srv;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_Rtv;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_Dsv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_Uav;
        std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> m_MipUavs;
        std::vector<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>> m_SliceDsvs;
        // キューブマップ配列の枚数(非キューブマップ・単一キューブは1)。m_MipUavsのフラット添字計算に使う
        uint32_t m_CubeCount = 1;
        // ミップ0のピクセルサイズとミップ段数。生成経路が多く引数からは受け取れないため、
        // コンストラクタで手持ちのビュー(SRV→RTV→DSV→UAVの順)からリソースを引いてGetDescで求める
        uint32_t m_Width = 0;
        uint32_t m_Height = 0;
        uint32_t m_MipLevels = 0;
    };
}
