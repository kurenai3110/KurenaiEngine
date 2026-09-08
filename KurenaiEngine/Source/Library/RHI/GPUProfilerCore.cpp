#include "GPUProfilerCore.h"

#include "Core/Logger.h"

namespace Kurenai::RHI
{
    bool GPUProfilerCore::TryBeginScope(const std::string& name, uint32_t& outScopeIndex)
    {
        FrameSlot& slot = GetWriteSlot();
        if (slot.ScopeCount >= kMaxScopesPerFrame)
        {
            // 計測のみスキップする(描画自体には影響しない)。ただしGPU Frame Timeは
            // 各区間の合計なので、この状態では表示値が実際より小さくなる。黙って捨てると
            // 最適化の効果測定を誤らせるため一度だけ警告する
            if (!m_ScopeOverflowLogged)
            {
                m_ScopeOverflowLogged = true;
                Core::Logger::Warning(
                    m_BackendTag,
                    "GPUプロファイラの計測区間が上限(" + std::to_string(kMaxScopesPerFrame) + ")を超えました。'" + name +
                        "'以降は計測されず、GPU Frame Timeも過小表示になります。kMaxScopesPerFrameを増やしてください");
            }
            return false;
        }

        outScopeIndex = slot.ScopeCount;
        slot.ScopeNames[slot.ScopeCount] = name;
        return true;
    }

    bool GPUProfilerCore::TryEndScope(uint32_t& outScopeIndex)
    {
        FrameSlot& slot = GetWriteSlot();
        if (slot.ScopeCount >= kMaxScopesPerFrame)
        {
            return false;
        }

        outScopeIndex = slot.ScopeCount;
        ++slot.ScopeCount;
        return true;
    }

    void GPUProfilerCore::MarkFrameRecorded()
    {
        GetWriteSlot().Pending = true;
        m_WriteIndex = (m_WriteIndex + 1) % kFrameLatency;
    }

    void GPUProfilerCore::BeginResults(uint32_t scopeCount)
    {
        m_Results.clear();
        m_Results.reserve(scopeCount);
        m_PendingTotalMs = 0.0f;
    }

    void GPUProfilerCore::AddScopeResult(
        const std::string& name, uint64_t beginTicks, uint64_t endTicks, uint64_t frequency)
    {
        if (frequency == 0)
        {
            return;
        }

        const float timeMs = static_cast<float>(endTicks - beginTicks) * 1000.0f / static_cast<float>(frequency);
        m_Results.push_back({ name, timeMs });
        m_PendingTotalMs += timeMs;
    }

    void GPUProfilerCore::EndResults()
    {
        // GPU Frame Timeは各パスの計測値の合計として算出する(FrameStart〜FrameEndの全区間ではない)。
        //
        // DX11はSetRenderTarget(swapChain)でバックバッファに触れる際、vsyncによる暗黙のバッファ確保待ちが
        // 同一コマンドストリーム内でGPU側の待ちとして発生しうるが、この待ちはどのスコープにも属さない
        // (DX11SwapChain::Present()の実測でGPU Waitとして別途報告される)。全区間で計算すると
        // この待ちが計上されてしまいDX12(フェンス待ちが完全に計測区間外で発生する)と数値の意味が
        // 揃わなくなるため、両バックエンドとも「各パスの合計」に統一する
        m_TotalFrameTimeMs = m_PendingTotalMs;
    }
}
