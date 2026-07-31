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
        uint32_t ringCapacity)
        : m_Device(device)
        , m_Resource(std::move(resource))
        , m_MappedPtr(mappedPtr)
        , m_SlotSizeInBytes(sizeInBytes)
        , m_RingCapacity(ringCapacity)
        , m_Usage(usage)
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

    DX12Buffer::DX12Buffer(DX12Device* device, Microsoft::WRL::ComPtr<ID3D12Resource> resource, uint32_t uavIndex, uint32_t sizeInBytes, uint32_t strideInBytes)
        : m_Device(device)
        , m_Resource(std::move(resource))
        , m_MappedPtr(nullptr)
        , m_SlotSizeInBytes(sizeInBytes)
        , m_RingCapacity(1)
        , m_UavIndex(uavIndex)
        , m_Usage(BufferUsage::Structured)
        // BufferUsage::Structuredのリソースは作成時点でUNORDERED_ACCESS状態になっている
        // (DX12Device::CreateBuffer参照)。TransitionToが余計なバリアを積まないよう実態に合わせる
        , m_CurrentState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        (void)strideInBytes;
    }

    DX12Buffer::DX12Buffer(
        DX12Device* device,
        Microsoft::WRL::ComPtr<ID3D12Resource> resource,
        uint32_t uavIndex,
        uint32_t srvIndex,
        uint32_t sizeInBytes,
        uint32_t strideInBytes)
        : m_Device(device)
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
        Microsoft::WRL::ComPtr<ID3D12Resource> resource,
        D3D12_RESOURCE_STATES initialState,
        uint32_t srvIndex,
        Microsoft::WRL::ComPtr<ID3D12Resource> uploadResource,
        void* uploadMappedPtr,
        uint32_t sizeInBytes,
        uint32_t strideInBytes,
        uint32_t uploadRingCapacity)
        : m_Device(device)
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
        Microsoft::WRL::ComPtr<ID3D12Resource> resource,
        uint32_t srvIndex,
        uint32_t sizeInBytes,
        uint32_t strideInBytes,
        D3D12_RESOURCE_STATES initialState)
        : m_Device(device)
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
        if (m_UavIndex != kInvalid)
        {
            m_Device->GetSrvCpuHeap()->Free(m_UavIndex);
        }
        if (m_SrvIndex != kInvalid)
        {
            m_Device->GetSrvCpuHeap()->Free(m_SrvIndex);
        }
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
        return m_Device->GetSrvCpuHeap()->GetCpuHandle(m_UavIndex);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Buffer::GetSrvCpuHandle() const
    {
        return m_Device->GetSrvCpuHeap()->GetCpuHandle(m_SrvIndex);
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
