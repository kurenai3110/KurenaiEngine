#include "DX12TilePool.h"

#include <string>

#include "Core/Logger.h"

namespace Kurenai::RHI
{
    DX12TilePool::DX12TilePool(ID3D12Device* device)
        : m_Device(device)
    {
    }

    bool DX12TilePool::GrowLocked()
    {
        if (m_Device == nullptr)
        {
            return false;
        }

        D3D12_HEAP_DESC desc{};
        desc.SizeInBytes = kTileSizeBytes * kTilesPerBlock;
        desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        // テクスチャのタイルしか置かないため、レンダーターゲット/深度を除いた種別に限定する
        // (ResourceHeapTier 1のアダプタでは種別を混ぜられないため、明示しておく)
        desc.Flags = D3D12_HEAP_FLAG_DENY_BUFFERS | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;

        Microsoft::WRL::ComPtr<ID3D12Heap> heap;
        const HRESULT hr = m_Device->CreateHeap(&desc, IID_PPV_ARGS(&heap));
        if (FAILED(hr))
        {
            Core::Logger::Error(
                "DX12",
                "タイルプールのヒープを確保できませんでした(既存 " + std::to_string(m_Heaps.size()) +
                    "個 / 1個あたり " + std::to_string(desc.SizeInBytes / (1024 * 1024)) + "MB)");
            return false;
        }

        const auto heapIndex = static_cast<uint32_t>(m_Heaps.size());
        m_Heaps.push_back(std::move(heap));

        // 新しいヒープのタイルをすべてフリーリストへ。後ろから取り出すので降順で積む
        m_FreeList.reserve(m_FreeList.size() + kTilesPerBlock);
        for (uint32_t i = kTilesPerBlock; i > 0; --i)
        {
            m_FreeList.push_back(Tile{ heapIndex, i - 1 });
        }

        Core::Logger::Info(
            "DX12",
            "タイルプールのヒープを追加しました(合計 " + std::to_string(m_Heaps.size()) + "個 / " +
                std::to_string(m_Heaps.size() * desc.SizeInBytes / (1024 * 1024)) + "MB)");
        return true;
    }

    bool DX12TilePool::Allocate(uint32_t count, std::vector<Tile>& outTiles)
    {
        if (count == 0)
        {
            return true;
        }

        std::lock_guard<std::mutex> lock(m_Mutex);

        // 足りないぶんだけヒープを増やす。増やせなければ何も確保せずに失敗させる
        // (中途半端に確保して返すと、呼び出し側がマッピングを組み立てられない)
        while (m_FreeList.size() < count)
        {
            if (!GrowLocked())
            {
                return false;
            }
        }

        outTiles.reserve(outTiles.size() + count);
        for (uint32_t i = 0; i < count; ++i)
        {
            outTiles.push_back(m_FreeList.back());
            m_FreeList.pop_back();
        }
        m_UsedTileCount += count;
        return true;
    }

    void DX12TilePool::Free(const std::vector<Tile>& tiles)
    {
        if (tiles.empty())
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_Mutex);
        m_FreeList.insert(m_FreeList.end(), tiles.begin(), tiles.end());
        const auto count = static_cast<uint32_t>(tiles.size());
        m_UsedTileCount -= (m_UsedTileCount >= count) ? count : m_UsedTileCount;
    }

    ID3D12Heap* DX12TilePool::GetHeap(uint32_t heapIndex) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return heapIndex < m_Heaps.size() ? m_Heaps[heapIndex].Get() : nullptr;
    }

    uint64_t DX12TilePool::GetReservedBytes() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return static_cast<uint64_t>(m_Heaps.size()) * kTileSizeBytes * kTilesPerBlock;
    }

    uint32_t DX12TilePool::GetUsedTileCount() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_UsedTileCount;
    }

    uint32_t DX12TilePool::GetHeapCount() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return static_cast<uint32_t>(m_Heaps.size());
    }
}
