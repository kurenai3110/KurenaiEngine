#include "DX12SamplerSet.h"

#include "DX12Device.h"

namespace Kurenai::RHI
{
    DX12SamplerSet::DX12SamplerSet(DX12Device* device, uint32_t baseDescriptorIndex)
        : m_Device(device)
        , m_BaseDescriptorIndex(baseDescriptorIndex)
    {
    }

    D3D12_GPU_DESCRIPTOR_HANDLE DX12SamplerSet::GetBaseGpuHandle() const
    {
        return m_Device->GetShaderVisibleSamplerHeap()->GetGpuHandle(m_BaseDescriptorIndex);
    }
}
