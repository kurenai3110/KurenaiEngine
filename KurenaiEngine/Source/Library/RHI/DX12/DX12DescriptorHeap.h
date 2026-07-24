#pragma once

#include <cstdint>
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

namespace Kurenai::RHI
{
    // 固定容量のディスクリプタヒープをフリーリストで管理するヘルパー。
    // RTV/DSV/SRV用のCPU専用ヒープ、およびシェーダ可視のCBV_SRV_UAV/Samplerヒープの両方に使う
    class DX12DescriptorHeap
    {
    public:
        DX12DescriptorHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity, bool shaderVisible);

        uint32_t Allocate();
        void Free(uint32_t index);

        D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle(uint32_t index) const;
        D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(uint32_t index) const;

        ID3D12DescriptorHeap* GetHeap() const { return m_Heap.Get(); }
        uint32_t GetDescriptorSize() const { return m_DescriptorSize; }

    private:
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_Heap;
        uint32_t m_DescriptorSize = 0;
        uint32_t m_Capacity = 0;
        uint32_t m_NextIndex = 0;
        std::vector<uint32_t> m_FreeList;
    };
}
