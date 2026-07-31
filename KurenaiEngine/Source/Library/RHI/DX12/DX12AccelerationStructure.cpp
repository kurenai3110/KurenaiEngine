#include "DX12AccelerationStructure.h"

#include <utility>

#include "DX12Device.h"

namespace Kurenai::RHI
{
    DX12AccelerationStructure::DX12AccelerationStructure(
        DX12Device* device, Microsoft::WRL::ComPtr<ID3D12Resource> resource, uint32_t srvIndex)
        : m_Device(device)
        , m_Resource(std::move(resource))
        , m_SrvIndex(srvIndex)
    {
    }

    DX12AccelerationStructure::~DX12AccelerationStructure()
    {
        if (m_SrvIndex != kInvalid && m_Device)
        {
            m_Device->GetAssetSrvCpuHeap()->Free(m_SrvIndex);
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12AccelerationStructure::GetSrvCpuHandle() const
    {
        return m_Device->GetAssetSrvCpuHeap()->GetCpuHandle(m_SrvIndex);
    }
}
