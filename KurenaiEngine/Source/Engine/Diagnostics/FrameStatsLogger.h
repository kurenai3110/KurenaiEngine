#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

// 一定時間ぶんのフレーム統計を溜めて、まとめてログへ出すための集計器。
//
// 【なぜ溜めるのか】Logger::Info は flush を伴うので、毎フレーム出すと計測のために
// 計測対象を遅くする。1秒に1回へ抑えつつ、実行ごとの記録は残す。
//
// 【積算を代入にしないこと】ここに並ぶのはすべて集計期間の**合計**で、平均を出すのに
// フレーム数で割る。うっかり `=` にすると、期間内で一度しか起きなかったことが
// 最後のフレームの値に置き換わって消える。**絵には一切出ないので採取では捕まらない**。
namespace Kurenai::Diagnostics
{
    class FrameStatsLogger
    {
    public:
        // 1フレームぶんの入力。呼ぶ側が集めた値をまとめて渡す
        struct FrameSample
        {
            float CPUFrameTimeMs = 0.0f;
            double GPUTimeMs = 0.0;
            double GPUWaitMs = 0.0;
            // 平均だけではスパイクが埋もれるため、期間中の最悪値も残す(合計ではなく最大)
            float FrameTimeMs = 0.0f;
            uint32_t FrustumCullTested = 0;
            uint32_t FrustumCullCulled = 0;
            uint32_t LODSwitchCount = 0;
            // 【瞬間値ではなく積算する】m_RenderStats.LODFadingCountをそのままログへ
            // 出していたときは、集計期間(1秒)の最終フレームの値だけを見ていた。
            // 既定のフェードは0.25秒なので構造的にほぼ必ず取りこぼし、「フェードが一度も
            // 実行されていない」のか「実行されたが見ていないだけ」なのかを区別できなかった
            // (実際に取りこぼした)。期間中の「フェード中インスタンス×フレーム」を
            // 足し込めば、0.25秒のフェードでも14フレームぶんとして必ず現れる
            uint32_t LODFadingCount = 0;
            uint32_t MeshCullTested = 0;
            uint32_t MeshCullCulled = 0;
            uint32_t DrawCallsGBuffer = 0;
            uint32_t DrawCallsShadow = 0;
            uint32_t DrawCallsDepthPrepass = 0;
            uint32_t InstancedBatchCount = 0;
            uint32_t InstancedInstanceCount = 0;
        };

        // 集計期間の先頭であれば起点を打ち直し、1フレームぶんを足す
        void AddFrame(std::chrono::steady_clock::time_point now, const FrameSample& sample);

        // メッシュレットカリングの統計だけは読み戻せたフレームでしか増えないので別口。
        // 【読めなかったフレームは足さない】0として足すと間引き率が実際より低く出る
        void AddMeshletSample(uint32_t tested, uint32_t frustumCulled, uint32_t occlusionCulled);

        // 集計期間の経過秒。呼ぶ側がしきい値と比べる
        float GetElapsedSeconds(std::chrono::steady_clock::time_point now) const
        {
            return std::chrono::duration<float>(now - m_WindowStart).count();
        }

        // 出し終えたら次の期間へ。**全部の合計を0へ戻すこと**
        void Reset();

        uint32_t GetFrameCount() const { return m_FrameCount; }
        double GetCPUTimeSumMs() const { return m_CPUTimeSumMs; }
        double GetGPUTimeSumMs() const { return m_GPUTimeSumMs; }
        double GetGPUWaitSumMs() const { return m_GPUWaitSumMs; }
        float GetWorstFrameTimeMs() const { return m_WorstFrameTimeMs; }
        uint64_t GetCullTestedSum() const { return m_CullTestedSum; }
        uint64_t GetCullCulledSum() const { return m_CullCulledSum; }
        uint64_t GetLODSwitchSum() const { return m_LODSwitchSum; }
        uint64_t GetLODFadingSum() const { return m_LODFadingSum; }
        uint64_t GetMeshCullTestedSum() const { return m_MeshCullTestedSum; }
        uint64_t GetMeshCullCulledSum() const { return m_MeshCullCulledSum; }
        uint64_t GetDrawCallsGBufferSum() const { return m_DrawCallsGBufferSum; }
        uint64_t GetDrawCallsShadowSum() const { return m_DrawCallsShadowSum; }
        uint64_t GetDrawCallsDepthPrepassSum() const { return m_DrawCallsDepthPrepassSum; }
        uint64_t GetInstancedBatchSum() const { return m_InstancedBatchSum; }
        uint64_t GetInstancedInstanceSum() const { return m_InstancedInstanceSum; }
        uint64_t GetMeshletTestedSum() const { return m_MeshletTestedSum; }
        uint64_t GetMeshletFrustumCulledSum() const { return m_MeshletFrustumCulledSum; }
        uint64_t GetMeshletOcclusionCulledSum() const { return m_MeshletOcclusionCulledSum; }
        uint32_t GetMeshletSampleCount() const { return m_MeshletSampleCount; }

    private:
        std::chrono::steady_clock::time_point m_WindowStart;
        uint32_t m_FrameCount = 0;

        double m_CPUTimeSumMs = 0.0;
        double m_GPUTimeSumMs = 0.0;
        double m_GPUWaitSumMs = 0.0;
        // これだけは合計ではなく期間中の最大
        float m_WorstFrameTimeMs = 0.0f;

        uint64_t m_CullTestedSum = 0;
        uint64_t m_CullCulledSum = 0;
        uint64_t m_LODSwitchSum = 0;
        uint64_t m_LODFadingSum = 0;
        uint64_t m_MeshCullTestedSum = 0;
        uint64_t m_MeshCullCulledSum = 0;
        uint64_t m_DrawCallsGBufferSum = 0;
        uint64_t m_DrawCallsShadowSum = 0;
        uint64_t m_DrawCallsDepthPrepassSum = 0;
        uint64_t m_InstancedBatchSum = 0;
        uint64_t m_InstancedInstanceSum = 0;

        uint64_t m_MeshletTestedSum = 0;
        uint64_t m_MeshletFrustumCulledSum = 0;
        uint64_t m_MeshletOcclusionCulledSum = 0;
        // 読み戻せたフレームだけを数える。上の3つの平均はこれで割る
        uint32_t m_MeshletSampleCount = 0;
    };
}
