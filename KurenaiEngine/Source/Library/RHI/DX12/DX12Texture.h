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
            uint32_t cubeCount = 1);
        ~DX12Texture() override;

        ID3D12Resource* GetResource() const { return m_Resource.Get(); }
        D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpuHandle() const;
        D3D12_CPU_DESCRIPTOR_HANDLE GetRtvCpuHandle() const;
        D3D12_CPU_DESCRIPTOR_HANDLE GetDsvCpuHandle() const;
        // CreateUAVTexture/CreateHiZTextureで作成した場合のみ有効(コンピュートシェーダーからのRW用)。
        // CreateHiZTextureのミップチェーンテクスチャはミップごとに個別のUAVを持つためmipLevelで選択する
        // (CreateUAVTextureは常に1ミップのみなので既定値の0で単一UAVが返る)
        D3D12_CPU_DESCRIPTOR_HANDLE GetUavCpuHandle(uint32_t mipLevel = 0) const;

        // キューブマップ(CreateUAVTextureCube/CreateMippedUAVTextureCube/
        // CreateMippedUAVTextureCubeArray)専用。m_MipUavIndicesに
        // (mip*cubeCount + cubeIndex)*kCubeFaceCount + face の順でフラットに格納しているため、
        // この3引数版で個別のキューブ・面・ミップのUAVを取り出す。
        // 範囲外を指定した場合はログを出してハンドル0を返す(呼び出し側で無効判定できるようにする)
        static constexpr uint32_t kCubeFaceCount = 6;
        D3D12_CPU_DESCRIPTOR_HANDLE GetCubeUavCpuHandle(uint32_t face, uint32_t mipLevel = 0, uint32_t cubeIndex = 0) const;

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
        // キューブマップ配列の枚数(非キューブマップ・単一キューブは1)。m_MipUavIndicesのフラット添字計算に使う
        uint32_t m_CubeCount = 1;
    };
}
