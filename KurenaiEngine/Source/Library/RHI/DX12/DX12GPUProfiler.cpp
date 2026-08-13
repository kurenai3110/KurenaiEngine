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
        heapDesc.Count = kFrameLatency * kQueriesPerSlot;
        ThrowIfFailed(m_Device->GetDevice()->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_QueryHeap)), "GPUプロファイラのクエリヒープ作成に失敗しました");

        const uint64_t readbackSize = static_cast<uint64_t>(kFrameLatency) * kQueriesPerSlot * sizeof(UINT64);
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
        FrameSlot& slot = m_Slots[m_WriteIndex];
        if (slot.Pending)
        {
            // このスロットを再利用する前に、前回計測分の結果を必ず確定させておく
            ResolveSlot(slot, m_WriteIndex);
        }

        slot.ScopeCount = 0;
        m_Device->GetCommandList()->EndQuery(m_QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(m_WriteIndex, 0));
    }

    void DX12GPUProfiler::BeginScope(const std::string& name)
    {
        FrameSlot& slot = m_Slots[m_WriteIndex];
        if (slot.ScopeCount >= kMaxScopesPerFrame)
        {
            // 計測のみスキップする(描画自体には影響しない)。ただしGPU Frame Timeは
            // 各区間の合計なので、この状態では表示値が実際より小さくなる。黙って捨てると
            // 最適化の効果測定を誤らせるため一度だけ警告する
            if (!m_ScopeOverflowLogged)
            {
                m_ScopeOverflowLogged = true;
                Core::Logger::Warning(
                    "DX12",
                    "GPUプロファイラの計測区間が上限(" + std::to_string(kMaxScopesPerFrame) + ")を超えました。'" + name +
                        "'以降は計測されず、GPU Frame Timeも過小表示になります。kMaxScopesPerFrameを増やしてください");
            }
            return;
        }
        slot.ScopeNames[slot.ScopeCount] = name;
        const uint32_t offset = 2 + slot.ScopeCount * 2;
        m_Device->GetCommandList()->EndQuery(m_QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(m_WriteIndex, offset));
    }

    void DX12GPUProfiler::EndScope()
    {
        FrameSlot& slot = m_Slots[m_WriteIndex];
        if (slot.ScopeCount >= kMaxScopesPerFrame)
        {
            return;
        }
        const uint32_t offset = 2 + slot.ScopeCount * 2 + 1;
        m_Device->GetCommandList()->EndQuery(m_QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(m_WriteIndex, offset));
        ++slot.ScopeCount;
    }

    void DX12GPUProfiler::EndFrame()
    {
        FrameSlot& slot = m_Slots[m_WriteIndex];
        auto* cmdList = m_Device->GetCommandList();
        cmdList->EndQuery(m_QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(m_WriteIndex, 1));

        // このフレームで実際にEndQueryを発行した範囲(フレーム開始/終了+使用した区間数ぶん)のみ解決する。
        // 未使用の区間分のクエリインデックスはEndQueryが一度も呼ばれておらず状態が不定なため、
        // 解決対象に含めるとデバッグレイヤーの警告や不定値の原因になる
        const uint32_t queriesToResolve = 2 + slot.ScopeCount * 2;
        const uint64_t resolveOffset = static_cast<uint64_t>(m_WriteIndex) * kQueriesPerSlot * sizeof(UINT64);
        cmdList->ResolveQueryData(
            m_QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex(m_WriteIndex, 0), queriesToResolve, m_ReadbackBuffer.Get(), resolveOffset);

        slot.Pending = true;
        m_WriteIndex = (m_WriteIndex + 1) % kFrameLatency;
    }

    void DX12GPUProfiler::ResolveSlot(FrameSlot& slot, uint32_t slotIndex)
    {
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

        m_Results.clear();
        m_Results.reserve(slot.ScopeCount);
        // GPU Frame Timeは各パスの計測値の合計として算出する(FrameStart~FrameEndの全区間ではない)。
        // DX11GPUProfiler::ResolveSlot()と算出方法を揃え、両バックエンドで同じ意味の値になるようにする
        float totalFrameTimeMs = 0.0f;
        for (uint32_t i = 0; i < slot.ScopeCount; ++i)
        {
            const UINT64 begin = slotData[2 + i * 2];
            const UINT64 end = slotData[2 + i * 2 + 1];
            const float timeMs = static_cast<float>(end - begin) * 1000.0f / static_cast<float>(m_TimestampFrequency);
            m_Results.push_back({ slot.ScopeNames[i], timeMs });
            totalFrameTimeMs += timeMs;
        }
        m_TotalFrameTimeMs = totalFrameTimeMs;

        const D3D12_RANGE writtenRange{ 0, 0 };
        m_ReadbackBuffer->Unmap(0, &writtenRange);
        slot.Pending = false;
    }
}
