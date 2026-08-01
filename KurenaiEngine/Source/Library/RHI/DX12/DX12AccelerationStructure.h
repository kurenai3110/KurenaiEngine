#pragma once

#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

#include "RHI/IRHIAccelerationStructure.h"

namespace Kurenai::RHI
{
    class DX12Device;

    // DXRの高速化構造(BLAS/TLAS)の実体。
    //
    // 中身はD3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE状態のバッファ1本で、
    // この状態は生成後いっさい遷移しない(D3D12の仕様上、ASバッファは他の状態へ移せない)。
    // そのためDX12Texture/DX12BufferのようなTransitionToは持たない。
    //
    // 構築時の一時領域(スクラッチバッファ)はDX12Device::CreateBottomLevelAS/CreateTopLevelASが
    // ローカルに確保し、構築完了を同期的に待ってから破棄するためここでは保持しない。
    // TLASのみ、インスタンス記述子を置いたUPLOADバッファも同様に構築時だけの一時領域となる。
    //
    // SRVを持つのはTLASだけ。DXRのAS用SRVは他と違いpResource=nullptrで作り、
    // RaytracingAccelerationStructure.Locationへ「GPU仮想アドレス」を直接書くという特殊な作法を
    // 取る(ディスクリプタがリソースではなくアドレスを指す)。そのため他のSRVと違い
    // 「リソースを破棄してもディスクリプタはアドレスを指したまま」になり、寿命管理は
    // 呼び出し側(TLASより長くBLASを生存させる)の責任になる
    class DX12AccelerationStructure : public IRHIAccelerationStructure
    {
    public:
        static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

        // srvIndexはTLASのみ有効(BLASはkInvalidを渡す)
        DX12AccelerationStructure(DX12Device* device, Microsoft::WRL::ComPtr<ID3D12Resource> resource, uint32_t srvIndex);
        ~DX12AccelerationStructure() override;

        D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const { return m_Resource->GetGPUVirtualAddress(); }
        // TLASとして作られた場合のみ有効。BLASではkInvalidを返すため、バインド側は
        // これでTLASかどうかを判別できる
        uint32_t GetSrvIndex() const { return m_SrvIndex; }
        bool IsTopLevel() const { return m_SrvIndex != kInvalid; }
        D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpuHandle() const;

    private:
        DX12Device* m_Device = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_Resource;
        uint32_t m_SrvIndex = kInvalid;
    };
}
