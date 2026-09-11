#include "DX11GPUProfiler.h"

#include <utility>

#include "DX11Util.h"

namespace Kurenai::RHI
{
    DX11GPUProfiler::DX11GPUProfiler(Microsoft::WRL::ComPtr<ID3D11Device> device, Microsoft::WRL::ComPtr<ID3D11DeviceContext> context)
        : m_Device(std::move(device))
        , m_Context(std::move(context))
    {
        for (auto& slot : m_QuerySlots)
        {
            D3D11_QUERY_DESC disjointDesc{};
            disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            ThrowIfFailed(m_Device->CreateQuery(&disjointDesc, &slot.DisjointQuery), "GPUプロファイラのDisjointクエリ作成に失敗しました");

            slot.FrameStartQuery = CreateTimestampQuery();
            slot.FrameEndQuery = CreateTimestampQuery();
            for (uint32_t i = 0; i < GPUProfilerCore::kMaxScopesPerFrame; ++i)
            {
                slot.BeginQueries[i] = CreateTimestampQuery();
                slot.EndQueries[i] = CreateTimestampQuery();
            }
        }
    }

    Microsoft::WRL::ComPtr<ID3D11Query> DX11GPUProfiler::CreateTimestampQuery() const
    {
        D3D11_QUERY_DESC desc{};
        desc.Query = D3D11_QUERY_TIMESTAMP;
        Microsoft::WRL::ComPtr<ID3D11Query> query;
        ThrowIfFailed(m_Device->CreateQuery(&desc, &query), "GPUプロファイラのTimestampクエリ作成に失敗しました");
        return query;
    }

    void DX11GPUProfiler::BeginFrame()
    {
        if (m_Core.GetWriteSlot().Pending)
        {
            // このスロットを再利用する前に、前回計測分の結果を必ず確定させておく
            // (確定させないままBegin/Endし直すとクエリの内容が上書きされ結果を取りこぼす)
            ResolveWriteSlot();
        }

        m_Core.ResetWriteSlotScopes();
        QuerySlot& queries = m_QuerySlots[m_Core.GetWriteIndex()];
        m_Context->Begin(queries.DisjointQuery.Get());
        m_Context->End(queries.FrameStartQuery.Get());
    }

    void DX11GPUProfiler::BeginScope(const std::string& name)
    {
        uint32_t scopeIndex = 0;
        if (!m_Core.TryBeginScope(name, scopeIndex))
        {
            return;
        }
        m_Context->End(m_QuerySlots[m_Core.GetWriteIndex()].BeginQueries[scopeIndex].Get());
    }

    void DX11GPUProfiler::EndScope()
    {
        uint32_t scopeIndex = 0;
        if (!m_Core.TryEndScope(scopeIndex))
        {
            return;
        }
        m_Context->End(m_QuerySlots[m_Core.GetWriteIndex()].EndQueries[scopeIndex].Get());
    }

    void DX11GPUProfiler::EndFrame()
    {
        QuerySlot& queries = m_QuerySlots[m_Core.GetWriteIndex()];
        m_Context->End(queries.FrameEndQuery.Get());
        m_Context->End(queries.DisjointQuery.Get());

        m_Core.MarkFrameRecorded();
    }

    void DX11GPUProfiler::ResolveWriteSlot()
    {
        GPUProfilerCore::FrameSlot& slot = m_Core.GetWriteSlot();
        QuerySlot& queries = m_QuerySlots[m_Core.GetWriteIndex()];

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
        HRESULT hr = S_FALSE;
        // このスロットのGPU実行はkFrameLatency分前に発行済みのため通常は即座に完了しているが、
        // 念のため上限回数まで待つ。それでも完了しない場合は今回の結果確定を諦め、前回の値を表示し続ける
        for (int attempt = 0; attempt < 1000 && hr != S_OK; ++attempt)
        {
            hr = m_Context->GetData(queries.DisjointQuery.Get(), &disjointData, sizeof(disjointData), 0);
        }

        slot.Pending = false;

        if (hr != S_OK || disjointData.Disjoint || disjointData.Frequency == 0)
        {
            return;
        }

        m_Core.BeginResults(slot.ScopeCount);
        for (uint32_t i = 0; i < slot.ScopeCount; ++i)
        {
            UINT64 begin = 0;
            UINT64 end = 0;
            m_Context->GetData(queries.BeginQueries[i].Get(), &begin, sizeof(begin), 0);
            m_Context->GetData(queries.EndQueries[i].Get(), &end, sizeof(end), 0);
            m_Core.AddScopeResult(slot.ScopeNames[i], begin, end, disjointData.Frequency);
        }
        m_Core.EndResults();
    }
}
