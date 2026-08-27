#include "DX12Buffer.h"

#include <string>
#include <utility>

#include "Core/Logger.h"

#include "DX12Device.h"

namespace Kurenai::RHI
{
    DX12Buffer::DX12Buffer(
        DX12Device* device,
        Microsoft::WRL::ComPtr<ID3D12Resource> resource,
        void* mappedPtr,
        uint32_t sizeInBytes,
        uint32_t strideInBytes,
        BufferUsage usage,
        uint32_t ringCapacity,
        DX12DescriptorHeap* srvUavHeap,
        uint32_t srvIndex)
        : m_Device(device)
        , m_SrvUavHeap(srvUavHeap)
        , m_Resource(std::move(resource))
        , m_MappedPtr(mappedPtr)
        , m_SlotSizeInBytes(sizeInBytes)
        , m_RingCapacity(ringCapacity)
        , m_Usage(usage)
        // 頂点バッファはDEFAULTヒープへ作られ、初期データのアップロード後
        // VERTEX_AND_CONSTANT_BUFFER状態で置かれる(DX12Device::CreateBuffer)。
        // ShaderReadableでSRVも張る場合はメッシュシェーダーが非ピクセルシェーダーリソースとして
        // 読むが、この2状態は共存できるためTransitionToを呼ぶ必要はない
        , m_SrvIndex(srvIndex)
    {
        const D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = m_Resource->GetGPUVirtualAddress();

        if (usage == BufferUsage::Vertex)
        {
            m_VertexBufferView.BufferLocation = gpuAddress;
            m_VertexBufferView.SizeInBytes = sizeInBytes;
            m_VertexBufferView.StrideInBytes = strideInBytes;
        }
        else if (usage == BufferUsage::Index)
        {
            m_IndexBufferView.BufferLocation = gpuAddress;
            m_IndexBufferView.SizeInBytes = sizeInBytes;
            m_IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
        }
    }

    DX12Buffer::DX12Buffer(
        DX12Device* device,
        DX12DescriptorHeap* srvUavHeap,
        Microsoft::WRL::ComPtr<ID3D12Resource> resource,
        uint32_t uavIndex,
        uint32_t sizeInBytes,
        uint32_t strideInBytes,
        BufferUsage usage)
        : m_Device(device)
        , m_SrvUavHeap(srvUavHeap)
        , m_Resource(std::move(resource))
        , m_MappedPtr(nullptr)
        , m_SlotSizeInBytes(sizeInBytes)
        , m_RingCapacity(1)
        , m_UavIndex(uavIndex)
        , m_Usage(usage)
        // BufferUsage::Structured / IndirectArgsのリソースは作成時点でUNORDERED_ACCESS状態に
        // なっている(DX12Device::CreateBuffer参照)。
        // TransitionToが余計なバリアを積まないよう実態に合わせる
        , m_CurrentState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        (void)strideInBytes;
    }

    DX12Buffer::DX12Buffer(
        DX12Device* device,
        DX12DescriptorHeap* srvUavHeap,
        Microsoft::WRL::ComPtr<ID3D12Resource> resource,
        uint32_t uavIndex,
        uint32_t srvIndex,
        uint32_t sizeInBytes,
        uint32_t strideInBytes)
        : m_Device(device)
        , m_SrvUavHeap(srvUavHeap)
        , m_Resource(std::move(resource))
        , m_MappedPtr(nullptr)
        , m_SlotSizeInBytes(sizeInBytes)
        , m_RingCapacity(1)
        , m_UavIndex(uavIndex)
        , m_Usage(BufferUsage::StructuredRW)
        // BufferUsage::StructuredRWのリソースもStructuredと同じく作成時点でUNORDERED_ACCESS状態
        // (DX12Device::CreateBuffer参照)。以降はSetComputeUnorderedAccessBuffer /
        // SetShaderResourceBufferの呼び出しでUNORDERED_ACCESS↔PIXEL_SHADER_RESOURCEを往復する
        , m_CurrentState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        , m_SrvIndex(srvIndex)
    {
        (void)strideInBytes;
    }

    DX12Buffer::DX12Buffer(
        DX12Device* device,
        DX12DescriptorHeap* srvUavHeap,
        Microsoft::WRL::ComPtr<ID3D12Resource> resource,
        D3D12_RESOURCE_STATES initialState,
        uint32_t srvIndex,
        Microsoft::WRL::ComPtr<ID3D12Resource> uploadResource,
        void* uploadMappedPtr,
        uint32_t sizeInBytes,
        uint32_t strideInBytes,
        uint32_t uploadRingCapacity)
        : m_Device(device)
        , m_SrvUavHeap(srvUavHeap)
        , m_Resource(std::move(resource))
        , m_MappedPtr(nullptr)
        , m_SlotSizeInBytes(sizeInBytes)
        , m_RingCapacity(1)
        , m_Usage(BufferUsage::StructuredReadOnly)
        , m_CurrentState(initialState)
        , m_SrvIndex(srvIndex)
        , m_UploadResource(std::move(uploadResource))
        , m_UploadMappedPtr(uploadMappedPtr)
        , m_UploadRingCapacity(uploadRingCapacity)
    {
        (void)strideInBytes;
    }

    DX12Buffer::DX12Buffer(
        DX12Device* device,
        DX12DescriptorHeap* srvUavHeap,
        Microsoft::WRL::ComPtr<ID3D12Resource> resource,
        uint32_t srvIndex,
        uint32_t sizeInBytes,
        uint32_t strideInBytes,
        D3D12_RESOURCE_STATES initialState)
        : m_Device(device)
        , m_SrvUavHeap(srvUavHeap)
        , m_Resource(std::move(resource))
        , m_MappedPtr(nullptr)
        , m_SlotSizeInBytes(sizeInBytes)
        , m_RingCapacity(1)
        , m_Usage(BufferUsage::StructuredImmutable)
        , m_CurrentState(initialState)
        , m_SrvIndex(srvIndex)
    {
        (void)strideInBytes;
    }

    DX12Buffer::~DX12Buffer()
    {
        // bindless区画への登録があれば返却する。UnregisterはkInvalidBindlessIndexを
        // 渡された場合に何もしないため、登録の有無で分岐する必要はない
        if (m_Device)
        {
            if (DX12BindlessTable* table = m_Device->GetBindlessTable())
            {
                table->Unregister(m_BindlessIndex);
            }
        }

        if (m_UavIndex != kInvalid)
        {
            m_SrvUavHeap->Free(m_UavIndex);
        }
        if (m_SrvIndex != kInvalid)
        {
            m_SrvUavHeap->Free(m_SrvIndex);
        }
        if (m_ClearUavIndex != kInvalid)
        {
            m_SrvUavHeap->Free(m_ClearUavIndex);
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Buffer::GetOrCreateClearUavCpuHandle()
    {
        if (!HasUav())
        {
            return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        }

        // 元からrawなUsage(IndirectArgs)は通常のUAVがそのままクリアに使える
        if (m_Usage == BufferUsage::IndirectArgs)
        {
            return GetUavCpuHandle();
        }

        if (m_ClearUavIndex != kInvalid)
        {
            return m_SrvUavHeap->GetCpuHandle(m_ClearUavIndex);
        }

        // rawビューは4バイト単位でアドレスを刻むため、サイズが4の倍数でないと末尾を消せない。
        // 構造化バッファのストライドは4の倍数なので通常ここへは来ない
        if (m_SlotSizeInBytes == 0 || (m_SlotSizeInBytes % 4) != 0)
        {
            Core::Logger::Error(
                "DX12",
                "クリア用のraw UAVを作れません(サイズが4の倍数ではありません: " + std::to_string(m_SlotSizeInBytes) + ")");
            return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        }

        m_ClearUavIndex = m_SrvUavHeap->Allocate();

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.Buffer.NumElements = m_SlotSizeInBytes / 4;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        m_Device->GetDevice()->CreateUnorderedAccessView(
            m_Resource.Get(), nullptr, &uavDesc, m_SrvUavHeap->GetCpuHandle(m_ClearUavIndex));

        return m_SrvUavHeap->GetCpuHandle(m_ClearUavIndex);
    }

    D3D12_GPU_VIRTUAL_ADDRESS DX12Buffer::GetGPUVirtualAddress() const
    {
        return m_Resource->GetGPUVirtualAddress() + static_cast<UINT64>(m_CurrentRingIndex) * m_SlotSizeInBytes;
    }

    void* DX12Buffer::AdvanceRingAndGetWritePtr()
    {
        CheckRingOverflow(m_RingCapacity, m_RingWritesThisFrame, m_RingOverflowReported, "定数バッファ");

        m_CurrentRingIndex = (m_CurrentRingIndex + 1) % m_RingCapacity;
        return static_cast<uint8_t*>(m_MappedPtr) + static_cast<size_t>(m_CurrentRingIndex) * m_SlotSizeInBytes;
    }

    void DX12Buffer::CheckRingOverflow(
        uint32_t ringCapacity, uint32_t& writesThisFrame, bool& reported, const char* bufferKindName)
    {
        if (!m_Device)
        {
            return;
        }

        const uint64_t frameStamp = m_Device->GetFrameStamp();
        if (frameStamp != m_LastWriteFrameStamp)
        {
            m_LastWriteFrameStamp = frameStamp;
            m_RingWritesThisFrame = 0;
            m_UploadRingWritesThisFrame = 0;
        }

        ++writesThisFrame;

        // CPUはkFrameCountフレームぶん先行して記録するため、1フレームで安全に使えるのは
        // リング容量のkFrameCount分の1まで。それを超えるとGPUがまだ読んでいるスロットを上書きする
        const uint32_t safeWritesPerFrame = ringCapacity / DX12Device::kFrameCount;
        if (writesThisFrame > safeWritesPerFrame && !reported)
        {
            // 毎フレーム大量に出続けるのを避けるため、バッファごとに1回だけ報告する
            reported = true;
            Core::Logger::Error(
                "DX12",
                std::string(bufferKindName) + "のリングが1フレームで使い切られました(容量: " + std::to_string(ringCapacity) +
                    ", 1フレームあたりの安全な書き込み回数: " + std::to_string(safeWritesPerFrame) + ", 実際: " +
                    std::to_string(writesThisFrame) + ")。GPUが読み取り中のスロットを上書きするため描画結果が壊れます");
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Buffer::GetUavCpuHandle() const
    {
        // UAVを持たないUsageで呼ばれた場合にでたらめなハンドルを返さないよう、
        // GetSrvCpuHandleと同じくポインタ0を返して呼び出し側が判定できるようにする
        if (!HasUav())
        {
            return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        }
        return m_SrvUavHeap->GetCpuHandle(m_UavIndex);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Buffer::GetSrvCpuHandle() const
    {
        // SRVを持たないUsage(Vertex/Index/Constant、およびShaderReadableを指定しなかった頂点バッファ)は
        // m_SrvUavHeapがnullptrのまま、m_SrvIndexもkInvalidになる。そのまま計算すると
        // nullptr参照、あるいは「ヒープ先頭 + 0xFFFFFFFF × ディスクリプタサイズ」という
        // でたらめなハンドルになり、D3D12へ渡した時点でデバイス削除に至る
        // (DX12Texture::HasSrvがガードしているのと同じ問題)。
        // 呼び出し側が無効を判定できるよう、ここではポインタ0を返す
        if (!m_SrvUavHeap || m_SrvIndex == kInvalid)
        {
            return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        }
        return m_SrvUavHeap->GetCpuHandle(m_SrvIndex);
    }

    void DX12Buffer::TransitionTo(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES newState)
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

    void* DX12Buffer::AdvanceUploadRingAndGetWritePtr()
    {
        CheckRingOverflow(
            m_UploadRingCapacity, m_UploadRingWritesThisFrame, m_UploadRingOverflowReported, "読み取り専用構造化バッファのステージングリング");

        m_UploadRingIndex = (m_UploadRingIndex + 1) % m_UploadRingCapacity;
        return static_cast<uint8_t*>(m_UploadMappedPtr) + static_cast<size_t>(m_UploadRingIndex) * m_SlotSizeInBytes;
    }
}
