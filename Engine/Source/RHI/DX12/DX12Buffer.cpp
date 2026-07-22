#include "DX12Buffer.h"

#include <utility>

namespace Kurenai::RHI
{
    DX12Buffer::DX12Buffer(
        Microsoft::WRL::ComPtr<ID3D12Resource> resource,
        void* mappedPtr,
        uint32_t sizeInBytes,
        uint32_t strideInBytes,
        BufferUsage usage,
        uint32_t ringCapacity)
        : m_Resource(std::move(resource))
        , m_MappedPtr(mappedPtr)
        , m_SlotSizeInBytes(sizeInBytes)
        , m_RingCapacity(ringCapacity)
    {
        const D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = m_Resource->GetGPUVirtualAddress();

        if (usage == BufferUsage::Vertex)
        {
            m_VertexBufferView.BufferLocation = gpuAddress;
            m_VertexBufferView.SizeInBytes = sizeInBytes;
            m_VertexBufferView.StrideInBytes = strideInBytes;
        }
        else if (usage == BufferUsage::Index)
        {
            m_IndexBufferView.BufferLocation = gpuAddress;
            m_IndexBufferView.SizeInBytes = sizeInBytes;
            m_IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
        }
    }

    D3D12_GPU_VIRTUAL_ADDRESS DX12Buffer::GetGPUVirtualAddress() const
    {
        return m_Resource->GetGPUVirtualAddress() + static_cast<UINT64>(m_CurrentRingIndex) * m_SlotSizeInBytes;
    }

    void* DX12Buffer::AdvanceRingAndGetWritePtr()
    {
        m_CurrentRingIndex = (m_CurrentRingIndex + 1) % m_RingCapacity;
        return static_cast<uint8_t*>(m_MappedPtr) + static_cast<size_t>(m_CurrentRingIndex) * m_SlotSizeInBytes;
    }
}
