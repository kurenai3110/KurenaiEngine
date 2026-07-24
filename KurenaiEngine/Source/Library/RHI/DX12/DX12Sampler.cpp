#include "DX12Sampler.h"

#include "DX12Device.h"

namespace Kurenai::RHI
{
    DX12Sampler::DX12Sampler(DX12Device* device, uint32_t descriptorIndex)
        : m_Device(device)
        , m_DescriptorIndex(descriptorIndex)
    {
    }

    DX12Sampler::~DX12Sampler()
    {
        m_Device->GetSamplerCpuHeap()->Free(m_DescriptorIndex);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Sampler::GetCpuHandle() const
    {
        return m_Device->GetSamplerCpuHeap()->GetCpuHandle(m_DescriptorIndex);
    }
}
