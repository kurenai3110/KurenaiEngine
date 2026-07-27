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
        if (m_SrvIndex == kInvalid)
        {
            // SRVを持たないテクスチャ(深度専用など)をSetTextureへ渡した場合。無効なハンドルを
            // 作ってD3D12へ渡すとデバイス削除に至るため、0を返すnullディスクリプタで代替する
            // (DX11がnullptrのSRVをバインドしてサンプル結果が0になるのと同じ挙動)
            Core::Logger::Error("DX12", "GetSrvCpuHandle: SRVを持たないテクスチャが参照されました。nullディスクリプタで代替します");
            return m_Device->GetNullSrvCpuHandle();
        }
        return m_Device->GetSrvCpuHeap()->GetCpuHandle(m_SrvIndex);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetRtvCpuHandle() const
    {
        // RTVには代替できる有効なディスクリプタが無いため、呼び出し側(DX12CommandList::SetRenderTargets)が
        // HasRtv()で事前に弾く。ここへ到達した時点で呼び出し側の判定漏れなので必ずログを残す
        if (m_RtvIndex == kInvalid)
        {
            Core::Logger::Error("DX12", "GetRtvCpuHandle: RTVを持たないテクスチャが参照されました");
            return D3D12_CPU_DESCRIPTOR_HANDLE{};
        }
        return m_Device->GetRtvHeap()->GetCpuHandle(m_RtvIndex);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetDsvCpuHandle(uint32_t arraySlice) const
    {
        if (!m_SliceDsvIndices.empty())
        {
            if (arraySlice >= m_SliceDsvIndices.size())
            {
                Core::Logger::Error(
                    "DX12",
                    "GetDsvCpuHandle: 深度配列スライス" + std::to_string(arraySlice) + "が範囲外です(スライス数: " +
                        std::to_string(m_SliceDsvIndices.size()) + ")");
                return D3D12_CPU_DESCRIPTOR_HANDLE{};
            }
            return m_Device->GetDsvHeap()->GetCpuHandle(m_SliceDsvIndices[arraySlice]);
        }

        if (m_DsvIndex == kInvalid)
        {
            Core::Logger::Error("DX12", "GetDsvCpuHandle: DSVを持たないテクスチャが参照されました");
            return D3D12_CPU_DESCRIPTOR_HANDLE{};
        }
        return m_Device->GetDsvHeap()->GetCpuHandle(m_DsvIndex);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetUavCpuHandle(uint32_t mipLevel) const
    {
        if (!m_MipUavIndices.empty())
        {
            if (mipLevel >= m_MipUavIndices.size())
            {
                Core::Logger::Error(
                    "DX12",
                    "GetUavCpuHandle: ミップレベル" + std::to_string(mipLevel) + "が範囲外です(ミップ数: " +
                        std::to_string(m_MipUavIndices.size()) + ")。nullディスクリプタで代替します");
                return m_Device->GetNullUavCpuHandle();
            }
            return m_Device->GetSrvCpuHeap()->GetCpuHandle(m_MipUavIndices[mipLevel]);
        }

        if (m_UavIndex == kInvalid)
        {
            Core::Logger::Error("DX12", "GetUavCpuHandle: UAVを持たないテクスチャが参照されました。nullディスクリプタで代替します");
            return m_Device->GetNullUavCpuHandle();
        }
        return m_Device->GetSrvCpuHeap()->GetCpuHandle(m_UavIndex);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetCubeUavCpuHandle(uint32_t face, uint32_t mipLevel) const
    {
        const size_t index = static_cast<size_t>(mipLevel) * kCubeFaceCount + face;
        if (face >= kCubeFaceCount || index >= m_MipUavIndices.size())
        {
            Core::Logger::Error(
                "DX12",
                "GetCubeUavCpuHandle: 面" + std::to_string(face) + " / ミップ" + std::to_string(mipLevel) +
                    "が範囲外です(UAV数: " + std::to_string(m_MipUavIndices.size()) + ")。nullディスクリプタで代替します");
            return m_Device->GetNullUavCpuHandle();
        }
        return m_Device->GetSrvCpuHeap()->GetCpuHandle(m_MipUavIndices[index]);
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
