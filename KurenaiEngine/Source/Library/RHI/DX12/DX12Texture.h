#pragma once

#include <cstdint>
#include <d3d12.h>
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
            uint32_t dsvIndex);
        ~DX12Texture() override;

        ID3D12Resource* GetResource() const { return m_Resource.Get(); }
        D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpuHandle() const;
        D3D12_CPU_DESCRIPTOR_HANDLE GetRtvCpuHandle() const;
        D3D12_CPU_DESCRIPTOR_HANDLE GetDsvCpuHandle() const;

        // 現在の状態と異なる場合のみバリアを発行して遷移する
        void TransitionTo(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES newState);

    private:
        DX12Device* m_Device;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_Resource;
        D3D12_RESOURCE_STATES m_CurrentState;
        uint32_t m_SrvIndex;
        uint32_t m_RtvIndex;
        uint32_t m_DsvIndex;
    };
}
