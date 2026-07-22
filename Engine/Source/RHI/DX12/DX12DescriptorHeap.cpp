#include "DX12DescriptorHeap.h"

#include <stdexcept>

#include "DX12Util.h"

namespace Kurenai::RHI
{
    DX12DescriptorHeap::DX12DescriptorHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, bool shaderVisible)
        : m_Capacity(capacity)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = type;
        desc.NumDescriptors = capacity;
        desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_Heap)), "ディスクリプタヒープの作成に失敗しました");
        m_DescriptorSize = device->GetDescriptorHandleIncrementSize(type);
    }

    uint32_t DX12DescriptorHeap::Allocate()
    {
        if (!m_FreeList.empty())
        {
            uint32_t index = m_FreeList.back();
            m_FreeList.pop_back();
            return index;
        }

        if (m_NextIndex >= m_Capacity)
        {
            throw std::runtime_error("ディスクリプタヒープの容量を超えました");
        }

        return m_NextIndex++;
    }

    void DX12DescriptorHeap::Free(uint32_t index)
    {
        m_FreeList.push_back(index);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12DescriptorHeap::GetCpuHandle(uint32_t index) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_Heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * m_DescriptorSize;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE DX12DescriptorHeap::GetGpuHandle(uint32_t index) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = m_Heap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * m_DescriptorSize;
        return handle;
    }
}
