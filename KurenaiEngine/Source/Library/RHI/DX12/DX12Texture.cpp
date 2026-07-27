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
        uint32_t dsvIndex,
        uint32_t uavIndex,
        std::vector<uint32_t> mipUavIndices,
        std::vector<uint32_t> sliceDsvIndices)
        : m_Device(device)
        , m_Resource(std::move(resource))
        , m_CurrentState(initialState)
        , m_SrvIndex(srvIndex)
        , m_RtvIndex(rtvIndex)
        , m_DsvIndex(dsvIndex)
        , m_UavIndex(uavIndex)
        , m_MipUavIndices(std::move(mipUavIndices))
        , m_SliceDsvIndices(std::move(sliceDsvIndices))
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
        if (m_UavIndex != kInvalid)
        {
            m_Device->GetSrvCpuHeap()->Free(m_UavIndex);
        }
        for (const uint32_t mipUavIndex : m_MipUavIndices)
        {
            m_Device->GetSrvCpuHeap()->Free(mipUavIndex);
        }
        // スライスごとのDSVはSRVヒープではなくDSVヒープから確保しているため、解放先も分ける
        // (CreateDepthTextureArrayで作成した場合はm_DsvIndexをkInvalidのままにしてあるので、
        //  上のm_DsvIndex解放と二重に解放されることはない)
        for (const uint32_t sliceDsvIndex : m_SliceDsvIndices)
        {
            m_Device->GetDsvHeap()->Free(sliceDsvIndex);
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

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetDsvCpuHandle(uint32_t arraySlice) const
    {
        const uint32_t index = m_SliceDsvIndices.empty() ? m_DsvIndex : m_SliceDsvIndices[arraySlice];
        return m_Device->GetDsvHeap()->GetCpuHandle(index);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetUavCpuHandle(uint32_t mipLevel) const
    {
        const uint32_t index = m_MipUavIndices.empty() ? m_UavIndex : m_MipUavIndices[mipLevel];
        return m_Device->GetSrvCpuHeap()->GetCpuHandle(index);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetCubeUavCpuHandle(uint32_t face, uint32_t mipLevel) const
    {
        const uint32_t index = m_MipUavIndices[mipLevel * kCubeFaceCount + face];
        return m_Device->GetSrvCpuHeap()->GetCpuHandle(index);
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
