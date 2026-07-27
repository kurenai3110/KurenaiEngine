#pragma once

#include <cstdint>
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

#include "RHI/IRHITexture.h"

namespace Kurenai::RHI
{
    class DX12Device;

    // リソース本体に加え、現在のリソース状態(バリア用)とSRV/RTV/DSVの各ディスクリプタインデックスを保持する。
    // RTV/DSVを持たない場合はkInvalidを格納する
    class DX12Texture : public IRHITexture
    {
    public:
        static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

        DX12Texture(
            DX12Device* device,
            Microsoft::WRL::ComPtr<ID3D12Resource> resource,
            D3D12_RESOURCE_STATES initialState,
            uint32_t srvIndex,
            uint32_t rtvIndex,
            uint32_t dsvIndex,
            uint32_t uavIndex = kInvalid,
            std::vector<uint32_t> mipUavIndices = {},
            std::vector<uint32_t> sliceDsvIndices = {});
        ~DX12Texture() override;

        ID3D12Resource* GetResource() const { return m_Resource.Get(); }

        // 対応するビューを持っているか。持っていないインデックス(kInvalid)でGetCpuHandleを呼ぶと
        // 「ヒープ先頭 + 0xFFFFFFFF × ディスクリプタサイズ」というでたらめなハンドルができ、
        // それをそのままD3D12へ渡すとデバイス削除(TDR)に至る。DX11は同じ状況でnullptrを
        // バインドして静かに描画をやめるだけなので、呼び出し側はこれで事前に判定する
        bool HasSrv() const { return m_SrvIndex != kInvalid; }
        bool HasRtv() const { return m_RtvIndex != kInvalid; }
        bool HasDsv() const { return m_DsvIndex != kInvalid || !m_SliceDsvIndices.empty(); }

        D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpuHandle() const;
        D3D12_CPU_DESCRIPTOR_HANDLE GetRtvCpuHandle() const;
        // CreateDepthTextureArrayで作成した場合のみスライスごとの個別DSVを持ち、arraySliceで選択する
        // (通常のCreateDepthTextureは単一DSVのため既定値の0でそのDSVが返る)
        D3D12_CPU_DESCRIPTOR_HANDLE GetDsvCpuHandle(uint32_t arraySlice = 0) const;
        // 深度スライス数(0 = テクスチャ配列ではない通常の深度テクスチャ)。範囲外指定時のログ用
        uint32_t GetDepthSliceCount() const { return static_cast<uint32_t>(m_SliceDsvIndices.size()); }
        // CreateUAVTexture/CreateHiZTextureで作成した場合のみ有効(コンピュートシェーダーからのRW用)。
        // CreateHiZTextureのミップチェーンテクスチャはミップごとに個別のUAVを持つためmipLevelで選択する
        // (CreateUAVTextureは常に1ミップのみなので既定値の0で単一UAVが返る)
        D3D12_CPU_DESCRIPTOR_HANDLE GetUavCpuHandle(uint32_t mipLevel = 0) const;

        // キューブマップ(CreateUAVTextureCube/CreateMippedUAVTextureCube)専用。m_MipUavIndicesに
        // mip*kCubeFaceCount+face の順でフラットに格納しているため、この2引数版で個別の面・
        // ミップのUAVを取り出す
        static constexpr uint32_t kCubeFaceCount = 6;
        D3D12_CPU_DESCRIPTOR_HANDLE GetCubeUavCpuHandle(uint32_t face, uint32_t mipLevel = 0) const;

        // 現在の状態と異なる場合のみバリアを発行して遷移する
        void TransitionTo(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES newState);

    private:
        DX12Device* m_Device;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_Resource;
        D3D12_RESOURCE_STATES m_CurrentState;
        uint32_t m_SrvIndex;
        uint32_t m_RtvIndex;
        uint32_t m_DsvIndex;
        uint32_t m_UavIndex;
        std::vector<uint32_t> m_MipUavIndices;
        std::vector<uint32_t> m_SliceDsvIndices;
    };
}
