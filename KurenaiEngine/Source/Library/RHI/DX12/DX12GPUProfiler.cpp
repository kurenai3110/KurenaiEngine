#include "DX12GPUProfiler.h"

#include <d3dx12.h>

#include "DX12Device.h"
#include "DX12Util.h"

namespace Kurenai::RHI
{
    DX12GPUProfiler::DX12GPUProfiler(DX12Device* device)
        : m_Device(device)
    {
        D3D12_QUERY_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        heapDesc.Count = GPUProfilerCore::kFrameLatency * kQueriesPerSlot;
        ThrowIfFailed(m_Device->GetDevice()->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_QueryHeap)), "GPUプロファイラのクエリヒープ作成に失敗しました");

        const uint64_t readbackSize =
            static_cast<uint64_t>(GPUProfilerCore::kFrameLatency) * kQueriesPerSlot * sizeof(UINT64);
        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_READBACK);
        const CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(readbackSize);
        ThrowIfFailed(
            m_Device->GetDevice()->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_ReadbackBuffer)),
            "GPUプロファイラのリードバックバッファ作成に失敗しました");

        ThrowIfFailed(m_Device->GetCommandQueue()->GetTimestampFrequency(&m_TimestampFrequency), "GPUタイムスタンプ周波数の取得に失敗しました");
    }

    uint32_t DX12GPUProfiler::QueryIndex(uint32_t slotIndex, uint32_t offsetInSlot) const
    {
        return slotIndex * kQueriesPerSlot + offsetInSlot;
    }

    void DX12GPUProfiler::BeginFrame()
    {
        if (m_Core.GetWriteSlot().Pending)
        {
            // このスロットを再利用する前に、前回計測分の結果を必ず確定させておく
            ResolveWriteSlot();
        }

        m_Core.ResetWriteSlotScopes();
        m_Device->GetCommandList()->EndQuery(
            m_QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(m_Core.GetWriteIndex(), 0));
    }

    void DX12GPUProfiler::BeginScope(const std::string& name)
    {
        uint32_t scopeIndex = 0;
        if (!m_Core.TryBeginScope(name, scopeIndex))
        {
            return;
        }
        const uint32_t offset = 2 + scopeIndex * 2;
        m_Device->GetCommandList()->EndQuery(
            m_QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(m_Core.GetWriteIndex(), offset));
    }

    void DX12GPUProfiler::EndScope()
    {
        uint32_t scopeIndex = 0;
        if (!m_Core.TryEndScope(scopeIndex))
        {
            return;
        }
        const uint32_t offset = 2 + scopeIndex * 2 + 1;
        m_Device->GetCommandList()->EndQuery(
            m_QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(m_Core.GetWriteIndex(), offset));
    }

    void DX12GPUProfiler::EndFrame()
    {
        const uint32_t slotIndex = m_Core.GetWriteIndex();
        const uint32_t scopeCount = m_Core.GetWriteSlot().ScopeCount;
        auto* cmdList = m_Device->GetCommandList();
        cmdList->EndQuery(m_QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(slotIndex, 1));

        // このフレームで実際にEndQueryを発行した範囲(フレーム開始/終了+使用した区間数ぶん)のみ解決する。
        // 未使用の区間分のクエリインデックスはEndQueryが一度も呼ばれておらず状態が不定なため、
        // 解決対象に含めるとデバッグレイヤーの警告や不定値の原因になる
        const uint32_t queriesToResolve = 2 + scopeCount * 2;
        const uint64_t resolveOffset = static_cast<uint64_t>(slotIndex) * kQueriesPerSlot * sizeof(UINT64);
        cmdList->ResolveQueryData(
            m_QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(slotIndex, 0), queriesToResolve, m_ReadbackBuffer.Get(), resolveOffset);

        m_Core.MarkFrameRecorded();
    }

    void DX12GPUProfiler::ResolveWriteSlot()
    {
        GPUProfilerCore::FrameSlot& slot = m_Core.GetWriteSlot();
        const uint32_t slotIndex = m_Core.GetWriteIndex();

        // DX12Device::AdvanceToNextFrame()は次フレームの記録を始める前に、kFrameCount
        // (=2)フレーム前のGPU実行完了をフェンスで保証している。このプロファイラのリング段数
        // kFrameLatency(=4)はkFrameCountより大きいため、次にこのスロットを使い回す時点
        // (kFrameLatencyフレーム後)では対応するフレームのGPU実行は必ず完了しており、
        // リードバックバッファの内容は確定している
        const D3D12_RANGE readRange{
            static_cast<SIZE_T>(slotIndex) * kQueriesPerSlot * sizeof(UINT64),
            static_cast<SIZE_T>(slotIndex + 1) * kQueriesPerSlot * sizeof(UINT64) };
        UINT64* mapped = nullptr;
        ThrowIfFailed(m_ReadbackBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mapped)), "GPUプロファイラのリードバック結果取得に失敗しました");

        const UINT64* slotData = mapped + static_cast<uint64_t>(slotIndex) * kQueriesPerSlot;

        m_Core.BeginResults(slot.ScopeCount);
        for (uint32_t i = 0; i < slot.ScopeCount; ++i)
        {
            const UINT64 begin = slotData[2 + i * 2];
            const UINT64 end = slotData[2 + i * 2 + 1];
            m_Core.AddScopeResult(slot.ScopeNames[i], begin, end, m_TimestampFrequency);
        }
        m_Core.EndResults();

        const D3D12_RANGE writtenRange{ 0, 0 };
        m_ReadbackBuffer->Unmap(0, &writtenRange);
        slot.Pending = false;
    }
}
