#include "DX12Buffer.h"

#include <utility>

#include "DX12Device.h"

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

    DX12Buffer::DX12Buffer(DX12Device* device, Microsoft::WRL::ComPtr<ID3D12Resource> resource, uint32_t uavIndex, uint32_t sizeInBytes, uint32_t strideInBytes)
        : m_Device(device)
        , m_Resource(std::move(resource))
        , m_MappedPtr(nullptr)
        , m_SlotSizeInBytes(sizeInBytes)
        , m_RingCapacity(1)
        , m_UavIndex(uavIndex)
    {
        (void)strideInBytes;
    }

    DX12Buffer::DX12Buffer(
        DX12Device* device,
        Microsoft::WRL::ComPtr<ID3D12Resource> resource,
        D3D12_RESOURCE_STATES initialState,
        uint32_t srvIndex,
        Microsoft::WRL::ComPtr<ID3D12Resource> uploadResource,
        void* uploadMappedPtr,
        uint32_t sizeInBytes,
        uint32_t strideInBytes,
        uint32_t uploadRingCapacity)
        : m_Device(device)
        , m_Resource(std::move(resource))
        , m_MappedPtr(nullptr)
        , m_SlotSizeInBytes(sizeInBytes)
        , m_RingCapacity(1)
        , m_CurrentState(initialState)
        , m_SrvIndex(srvIndex)
        , m_UploadResource(std::move(uploadResource))
        , m_UploadMappedPtr(uploadMappedPtr)
        , m_UploadRingCapacity(uploadRingCapacity)
    {
        (void)strideInBytes;
    }

    DX12Buffer::~DX12Buffer()
    {
        if (m_UavIndex != kInvalid)
        {
            m_Device->GetSrvCpuHeap()->Free(m_UavIndex);
        }
        if (m_SrvIndex != kInvalid)
        {
            m_Device->GetSrvCpuHeap()->Free(m_SrvIndex);
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

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Buffer::GetUavCpuHandle() const
    {
        return m_Device->GetSrvCpuHeap()->GetCpuHandle(m_UavIndex);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Buffer::GetSrvCpuHandle() const
    {
        return m_Device->GetSrvCpuHeap()->GetCpuHandle(m_SrvIndex);
    }

    void DX12Buffer::TransitionTo(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES newState)
    {
        if (m_CurrentState == newState)
        {
            return;
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_Resource.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = m_CurrentState;
        barrier.Transition.StateAfter = newState;
        commandList->ResourceBarrier(1, &barrier);

        m_CurrentState = newState;
    }

    void* DX12Buffer::AdvanceUploadRingAndGetWritePtr()
    {
        m_UploadRingIndex = (m_UploadRingIndex + 1) % m_UploadRingCapacity;
        return static_cast<uint8_t*>(m_UploadMappedPtr) + static_cast<size_t>(m_UploadRingIndex) * m_SlotSizeInBytes;
    }
}
