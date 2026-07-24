#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace Kurenai::Core
{
    struct CPUTimingResult
    {
        std::string Name;
        float TimeMs = 0.0f;
    };

    // std::chronoによるCPU側の区間計測(RHI::IRHIGPUProfilerのCPU版)。
    // RHIに依存せずDX11/DX12共通のApplication::Render()から直接使えるため、
    // 各パスのコマンド記録にかかるCPU時間をバックエンド間で直接比較できる。
    // GPUProfiler同様、ネスト不可・シーケンシャルな区間のみをサポートする
    class CPUProfiler
    {
    public:
        // フレームの計測開始時に呼ぶ。前フレームの結果をクリアする
        void BeginFrame();

        // 計測したい区間の開始/終了を記録する
        void BeginScope(const std::string& name);
        void EndScope();

        // 指定した名前の直近の計測値からdeltaMsを差し引く(0未満にはならない)。
        // GPUの完了待ちなど、実際のCPU負荷ではない時間を含むスコープから、
        // その内訳を除外して表示したい場合に使う。該当スコープが無ければ何もしない
        void SubtractFromScope(const std::string& name, float deltaMs);

        const std::vector<CPUTimingResult>& GetResults() const { return m_Results; }

    private:
        std::vector<CPUTimingResult> m_Results;
        std::string m_CurrentScopeName;
        std::chrono::steady_clock::time_point m_ScopeStart;
        bool m_ScopeActive = false;
    };
}
