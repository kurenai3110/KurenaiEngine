#include "DX12Texture.h"

#include <utility>

#include "DX12Device.h"

namespace Kurenai::RHI
{
    DX12Texture::DX12Texture(
        DX12Device* device,
        Microsoft::WRL::ComPtr<ID3D12Resource> resource,
        D3D12_RESOURCE_STATES initialState,
        uint32_t srvIndex,
        uint32_t rtvIndex,
        uint32_t dsvIndex)
        : m_Device(device)
        , m_Resource(std::move(resource))
        , m_CurrentState(initialState)
        , m_SrvIndex(srvIndex)
        , m_RtvIndex(rtvIndex)
        , m_DsvIndex(dsvIndex)
    {
    }

    DX12Texture::~DX12Texture()
    {
        if (m_SrvIndex != kInvalid)
        {
            m_Device->GetSrvCpuHeap()->Free(m_SrvIndex);
        }
        if (m_RtvIndex != kInvalid)
        {
            m_Device->GetRtvHeap()->Free(m_RtvIndex);
        }
        if (m_DsvIndex != kInvalid)
        {
            m_Device->GetDsvHeap()->Free(m_DsvIndex);
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetSrvCpuHandle() const
    {
        return m_Device->GetSrvCpuHeap()->GetCpuHandle(m_SrvIndex);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetRtvCpuHandle() const
    {
        return m_Device->GetRtvHeap()->GetCpuHandle(m_RtvIndex);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetDsvCpuHandle() const
    {
        return m_Device->GetDsvHeap()->GetCpuHandle(m_DsvIndex);
    }

    void DX12Texture::TransitionTo(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES newState)
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
}
