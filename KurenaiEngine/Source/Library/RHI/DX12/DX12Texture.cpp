#include "DX12Texture.h"

#include <string>
#include <utility>

#include "Core/Logger.h"

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
        uint32_t cubeCount)
        : m_Device(device)
        , m_Resource(std::move(resource))
        , m_CurrentState(initialState)
        , m_SrvIndex(srvIndex)
        , m_RtvIndex(rtvIndex)
        , m_DsvIndex(dsvIndex)
        , m_UavIndex(uavIndex)
        , m_MipUavIndices(std::move(mipUavIndices))
        , m_CubeCount(cubeCount)
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

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetUavCpuHandle(uint32_t mipLevel) const
    {
        const uint32_t index = m_MipUavIndices.empty() ? m_UavIndex : m_MipUavIndices[mipLevel];
        return m_Device->GetSrvCpuHeap()->GetCpuHandle(index);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetCubeUavCpuHandle(uint32_t face, uint32_t mipLevel, uint32_t cubeIndex) const
    {
        if (face >= kCubeFaceCount || cubeIndex >= m_CubeCount)
        {
            Core::Logger::Error(
                "DX12",
                "GetCubeUavCpuHandle: 範囲外の指定です (face=" + std::to_string(face) +
                    ", cubeIndex=" + std::to_string(cubeIndex) + ", cubeCount=" + std::to_string(m_CubeCount) + ")");
            return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        }

        const size_t flatIndex = (static_cast<size_t>(mipLevel) * m_CubeCount + cubeIndex) * kCubeFaceCount + face;
        if (flatIndex >= m_MipUavIndices.size())
        {
            Core::Logger::Error(
                "DX12",
                "GetCubeUavCpuHandle: ミップレベルが範囲外です (mipLevel=" + std::to_string(mipLevel) +
                    ", UAV数=" + std::to_string(m_MipUavIndices.size()) + ")");
            return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        }

        return m_Device->GetSrvCpuHeap()->GetCpuHandle(m_MipUavIndices[flatIndex]);
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
