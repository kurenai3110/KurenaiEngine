#include "DX11GPUProfiler.h"

#include <utility>

#include "DX11Util.h"

namespace Kurenai::RHI
{
    DX11GPUProfiler::DX11GPUProfiler(Microsoft::WRL::ComPtr<ID3D11Device> device, Microsoft::WRL::ComPtr<ID3D11DeviceContext> context)
        : m_Device(std::move(device))
        , m_Context(std::move(context))
    {
        for (auto& slot : m_Slots)
        {
            D3D11_QUERY_DESC disjointDesc{};
            disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            ThrowIfFailed(m_Device->CreateQuery(&disjointDesc, &slot.DisjointQuery), "GPUプロファイラのDisjointクエリ作成に失敗しました");

            slot.FrameStartQuery = CreateTimestampQuery();
            slot.FrameEndQuery = CreateTimestampQuery();
            for (uint32_t i = 0; i < kMaxScopesPerFrame; ++i)
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
        FrameSlot& slot = m_Slots[m_WriteIndex];
        if (slot.Pending)
        {
            // このスロットを再利用する前に、前回計測分の結果を必ず確定させておく
            // (確定させないままBegin/Endし直すとクエリの内容が上書きされ結果を取りこぼす)
            ResolveSlot(slot);
        }

        slot.ScopeCount = 0;
        m_Context->Begin(slot.DisjointQuery.Get());
        m_Context->End(slot.FrameStartQuery.Get());
    }

    void DX11GPUProfiler::BeginScope(const std::string& name)
    {
        FrameSlot& slot = m_Slots[m_WriteIndex];
        if (slot.ScopeCount >= kMaxScopesPerFrame)
        {
            return; // 想定外に計測区間が増えた場合は計測のみスキップする(描画自体には影響しない)
        }
        slot.ScopeNames[slot.ScopeCount] = name;
        m_Context->End(slot.BeginQueries[slot.ScopeCount].Get());
    }

    void DX11GPUProfiler::EndScope()
    {
        FrameSlot& slot = m_Slots[m_WriteIndex];
        if (slot.ScopeCount >= kMaxScopesPerFrame)
        {
            return;
        }
        m_Context->End(slot.EndQueries[slot.ScopeCount].Get());
        ++slot.ScopeCount;
    }

    void DX11GPUProfiler::EndFrame()
    {
        FrameSlot& slot = m_Slots[m_WriteIndex];
        m_Context->End(slot.FrameEndQuery.Get());
        m_Context->End(slot.DisjointQuery.Get());
        slot.Pending = true;

        m_WriteIndex = (m_WriteIndex + 1) % kFrameLatency;
    }

    void DX11GPUProfiler::ResolveSlot(FrameSlot& slot)
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
        HRESULT hr = S_FALSE;
        // このスロットのGPU実行はkFrameLatency分前に発行済みのため通常は即座に完了しているが、
        // 念のため上限回数まで待つ。それでも完了しない場合は今回の結果確定を諦め、前回の値を表示し続ける
        for (int attempt = 0; attempt < 1000 && hr != S_OK; ++attempt)
        {
            hr = m_Context->GetData(slot.DisjointQuery.Get(), &disjointData, sizeof(disjointData), 0);
        }

        slot.Pending = false;

        if (hr != S_OK || disjointData.Disjoint || disjointData.Frequency == 0)
        {
            return;
        }

        UINT64 frameStart = 0;
        UINT64 frameEnd = 0;
        m_Context->GetData(slot.FrameStartQuery.Get(), &frameStart, sizeof(frameStart), 0);
        m_Context->GetData(slot.FrameEndQuery.Get(), &frameEnd, sizeof(frameEnd), 0);
        m_TotalFrameTimeMs = static_cast<float>(frameEnd - frameStart) * 1000.0f / static_cast<float>(disjointData.Frequency);

        m_Results.clear();
        m_Results.reserve(slot.ScopeCount);
        for (uint32_t i = 0; i < slot.ScopeCount; ++i)
        {
            UINT64 begin = 0;
            UINT64 end = 0;
            m_Context->GetData(slot.BeginQueries[i].Get(), &begin, sizeof(begin), 0);
            m_Context->GetData(slot.EndQueries[i].Get(), &end, sizeof(end), 0);
            const float timeMs = static_cast<float>(end - begin) * 1000.0f / static_cast<float>(disjointData.Frequency);
            m_Results.push_back({ slot.ScopeNames[i], timeMs });
        }
    }
}
