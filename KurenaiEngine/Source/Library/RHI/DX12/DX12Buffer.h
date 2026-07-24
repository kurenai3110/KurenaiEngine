#pragma once

#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

#include "RHI/IRHIBuffer.h"
#include "RHI/RHIEnums.h"

namespace Kurenai::RHI
{
    class DX12Device;

    // 頂点/インデックス/定数バッファすべてこの1種類で扱うが、ヒープの配置はUsageによって異なる
    // (DX12Device::CreateBuffer参照)。
    //
    // 定数バッファ(Usage==Constant)はCPUから毎フレームUpdateBufferで書き込むため、UPLOADヒープに
    // 常駐させマップしたままにする。1フレームぶんのコマンドをすべて記録してから1回だけ実行する設計上、
    // 単純に同じ領域へ複数回UpdateBufferすると(例: メッシュごとに material 定数を書き換える場合)、
    // GPU実行時にはそのフレーム最後に書き込んだ内容へ全描画が上書きされてしまう。これを避けるため、
    // 定数バッファは内部でリング状に複数コピーを持ち、UpdateBufferのたびに次のスロットへ書き込みを進める。
    // CPUはGPU完了を待たずに次フレームの記録を始める(DX12Device::kFrameCount)ため、リング容量は
    // 直近数フレームぶんのUpdateBuffer回数を十分上回る値にしてある(DX12Device::kConstantBufferRingCapacity)。
    //
    // 頂点/インデックスバッファは初回アップロード後書き換えないため、GPUからの読み取りが高速な
    // DEFAULTヒープに作成する(m_MappedPtrはnullptr、ringCapacity=1でAdvanceRingAndGetWritePtrは呼ばれない)
    class DX12Buffer : public IRHIBuffer
    {
    public:
        DX12Buffer(
            Microsoft::WRL::ComPtr<ID3D12Resource> resource,
            void* mappedPtr,
            uint32_t sizeInBytes,
            uint32_t strideInBytes,
            BufferUsage usage,
            uint32_t ringCapacity = 1);

        // BufferUsage::Structured(RWStructuredBuffer)用: UAVディスクリプタのインデックスを保持し、
        // 破棄時にDX12Deviceのディスクリプタヒープへ返却する
        static constexpr uint32_t kInvalid = 0xFFFFFFFFu;
        DX12Buffer(DX12Device* device, Microsoft::WRL::ComPtr<ID3D12Resource> resource, uint32_t uavIndex, uint32_t sizeInBytes, uint32_t strideInBytes);
        ~DX12Buffer() override;

        ID3D12Resource* GetResource() const { return m_Resource.Get(); }
        D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const;
        // UpdateBuffer用: リング上の次のスロットへ書き込み位置を進め、そのCPUマップ済みポインタを返す
        void* AdvanceRingAndGetWritePtr();
        const D3D12_VERTEX_BUFFER_VIEW& GetVertexBufferView() const { return m_VertexBufferView; }
        const D3D12_INDEX_BUFFER_VIEW& GetIndexBufferView() const { return m_IndexBufferView; }
        // BufferUsage::Structuredで作成した場合のみ有効なUAVハンドル(コンピュートシェーダーからのRW用)
        D3D12_CPU_DESCRIPTOR_HANDLE GetUavCpuHandle() const;

    private:
        DX12Device* m_Device = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_Resource;
        void* m_MappedPtr;
        uint32_t m_SlotSizeInBytes;
        uint32_t m_RingCapacity;
        uint32_t m_CurrentRingIndex = 0;
        uint32_t m_UavIndex = kInvalid;
        D3D12_VERTEX_BUFFER_VIEW m_VertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW m_IndexBufferView{};
    };
}
