#pragma once

#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

#include "RHI/IRHIBuffer.h"
#include "RHI/RHIEnums.h"

namespace Kurenai::RHI
{
    // 常にUPLOADヒープに作成し、CPUから直接書き込めるようマップしたままにする。
    // 毎フレーム完全同期のためGPUとの多重バッファリングは不要で、頂点/インデックス/定数バッファすべてこの1種類で扱う。
    //
    // 定数バッファ(Usage==Constant)は、1フレームぶんのコマンドをすべて記録してから1回だけ実行する設計上、
    // 単純に同じ領域へ複数回UpdateBufferすると(例: メッシュごとに material 定数を書き換える場合)、
    // GPU実行時にはそのフレーム最後に書き込んだ内容へ全描画が上書きされてしまう。これを避けるため、
    // 定数バッファは内部でリング状に複数コピーを持ち、UpdateBufferのたびに次のスロットへ書き込みを進める
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

        D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const;
        // UpdateBuffer用: リング上の次のスロットへ書き込み位置を進め、そのCPUマップ済みポインタを返す
        void* AdvanceRingAndGetWritePtr();
        const D3D12_VERTEX_BUFFER_VIEW& GetVertexBufferView() const { return m_VertexBufferView; }
        const D3D12_INDEX_BUFFER_VIEW& GetIndexBufferView() const { return m_IndexBufferView; }

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> m_Resource;
        void* m_MappedPtr;
        uint32_t m_SlotSizeInBytes;
        uint32_t m_RingCapacity;
        uint32_t m_CurrentRingIndex = 0;
        D3D12_VERTEX_BUFFER_VIEW m_VertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW m_IndexBufferView{};
    };
}
