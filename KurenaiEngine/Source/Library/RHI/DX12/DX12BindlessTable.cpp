#include "DX12BindlessTable.h"

#include <string>

#include "Core/Logger.h"
#include "DX12DescriptorHeap.h"
#include "RHI/RHIBindless.h"

namespace Kurenai::RHI
{
    DX12BindlessTable::DX12BindlessTable(ID3D12Device* device, DX12DescriptorHeap* heap, uint32_t baseIndex, uint32_t capacity)
        : m_Device(device), m_Heap(heap), m_BaseIndex(baseIndex), m_Capacity(capacity)
    {
    }

    uint32_t DX12BindlessTable::Register(D3D12_CPU_DESCRIPTOR_HANDLE sourceCpuHandle)
    {
        if (!m_Device || !m_Heap)
        {
            return kInvalidBindlessIndex;
        }

        // 無効なハンドル(ビューを持たないリソースのGet*CpuHandle)をコピーすると
        // でたらめなディスクリプタが区画へ入り、シェーダーが読んだ瞬間にデバイス削除へ至る。
        // DX12Texture::HasSrvと同じ考え方でここでも弾いておく
        if (sourceCpuHandle.ptr == 0)
        {
            Core::Logger::Error("DX12", "bindlessへ無効なディスクリプタハンドルが渡されました");
            return kInvalidBindlessIndex;
        }

        uint32_t offset = 0;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!m_FreeList.empty())
            {
                offset = m_FreeList.back();
                m_FreeList.pop_back();
            }
            else if (m_NextOffset < m_Capacity)
            {
                offset = m_NextOffset++;
            }
            else
            {
                Core::Logger::Error(
                    "DX12",
                    "bindless区画の容量を超えました(容量: " + std::to_string(m_Capacity) +
                        ")。このリソースはbindlessで引けません");
                return kInvalidBindlessIndex;
            }
        }

        const uint32_t heapIndex = m_BaseIndex + offset;
        m_Device->CopyDescriptorsSimple(
            1, m_Heap->GetCpuHandle(heapIndex), sourceCpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return heapIndex;
    }

    void DX12BindlessTable::Unregister(uint32_t heapIndex)
    {
        if (heapIndex == kInvalidBindlessIndex)
        {
            return;
        }

        if (heapIndex < m_BaseIndex || heapIndex >= m_BaseIndex + m_Capacity)
        {
            Core::Logger::Error("DX12", "bindless区画の範囲外の番号が返却されました: " + std::to_string(heapIndex));
            return;
        }

        std::lock_guard<std::mutex> lock(m_Mutex);
        m_FreeList.push_back(heapIndex - m_BaseIndex);
    }

    uint32_t DX12BindlessTable::GetUsedCount() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_NextOffset - static_cast<uint32_t>(m_FreeList.size());
    }
}
