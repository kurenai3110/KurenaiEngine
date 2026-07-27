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
            DX12Device* device,
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

        // BufferUsage::StructuredReadOnly(StructuredBuffer<T>、読み取り専用)用:
        // ピクセルシェーダが読むSRV本体(DEFAULTヒープ、resource/initialState/srvIndex)と、
        // CPUが毎フレームUpdateBufferで書き込むUPLOADヒープのステージングリング
        // (uploadResource/uploadMappedPtr/uploadRingCapacity)の両方を保持する。
        // ピクセルごとに読まれるためUPLOADヒープに本体を置くことはできないが、UPLOADヒープに
        // 直接書き込めるConstantバッファのリングとは異なり、DEFAULTヒープへは
        // CopyBufferRegion(コマンドリスト経由)でしか書けないため、ステージング用の中間バッファが要る
        DX12Buffer(
            DX12Device* device,
            Microsoft::WRL::ComPtr<ID3D12Resource> resource,
            D3D12_RESOURCE_STATES initialState,
            uint32_t srvIndex,
            Microsoft::WRL::ComPtr<ID3D12Resource> uploadResource,
            void* uploadMappedPtr,
            uint32_t sizeInBytes,
            uint32_t strideInBytes,
            uint32_t uploadRingCapacity);

        ~DX12Buffer() override;

        ID3D12Resource* GetResource() const { return m_Resource.Get(); }
        D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const;
        // UpdateBuffer用: リング上の次のスロットへ書き込み位置を進め、そのCPUマップ済みポインタを返す
        void* AdvanceRingAndGetWritePtr();
        const D3D12_VERTEX_BUFFER_VIEW& GetVertexBufferView() const { return m_VertexBufferView; }
        const D3D12_INDEX_BUFFER_VIEW& GetIndexBufferView() const { return m_IndexBufferView; }
        // BufferUsage::Structuredで作成した場合のみ有効なUAVハンドル(コンピュートシェーダーからのRW用)
        D3D12_CPU_DESCRIPTOR_HANDLE GetUavCpuHandle() const;
        // BufferUsage::StructuredReadOnlyで作成した場合のみ有効なSRVハンドル(ピクセルシェーダからの読み取り用)
        D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpuHandle() const;
        // BufferUsage::StructuredReadOnlyで作成されたか(UpdateBufferの分岐に使う)。
        // このバッファ種別だけがm_SrvIndexを有効値で持つことを利用して判定する
        bool IsStructuredReadOnly() const { return m_SrvIndex != kInvalid; }
        // 現在のリソース状態と異なる場合のみバリアを発行して遷移する(DX12Texture::TransitionToと同じパターン)。
        // BufferUsage::StructuredReadOnlyでのみ使う
        void TransitionTo(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES newState);
        // UpdateBuffer用: ステージングリングの次のスロットへ書き込み位置を進め、そのCPUマップ済みポインタを返す。
        // BufferUsage::StructuredReadOnlyでのみ使う
        void* AdvanceUploadRingAndGetWritePtr();
        ID3D12Resource* GetUploadResource() const { return m_UploadResource.Get(); }
        uint64_t GetUploadRingOffset() const { return static_cast<uint64_t>(m_UploadRingIndex) * m_SlotSizeInBytes; }

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

        // BufferUsage::StructuredReadOnly専用のメンバ
        D3D12_RESOURCE_STATES m_CurrentState = D3D12_RESOURCE_STATE_COMMON;
        uint32_t m_SrvIndex = kInvalid;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_UploadResource;
        void* m_UploadMappedPtr = nullptr;
        uint32_t m_UploadRingCapacity = 1;
        uint32_t m_UploadRingIndex = 0;

        // リング周回の検出用。1フレーム内に「リング容量 ÷ kFrameCount」を超えて書き込むと、
        // まだGPUが読んでいる可能性のある直近フレームのスロットを上書きしてしまい、
        // 描画結果が静かに壊れる(定数バッファならメッシュの変換行列、構造化バッファならライトリスト)。
        // 例外を投げるとシーンが大きいだけでアプリが落ちてしまうため、検出したらログを出して継続する
        uint64_t m_LastWriteFrameStamp = 0;
        uint32_t m_RingWritesThisFrame = 0;
        uint32_t m_UploadRingWritesThisFrame = 0;
        bool m_RingOverflowReported = false;
        bool m_UploadRingOverflowReported = false;
        // リングへの書き込み回数をフレーム単位で数え直し、1フレームあたりの上限を超えていたらログを出す。
        // 戻り値は使わず、副作用(カウント・ログ)のみが目的
        void CheckRingOverflow(
            uint32_t ringCapacity, uint32_t& writesThisFrame, bool& reported, const char* bufferKindName);
    };
}
