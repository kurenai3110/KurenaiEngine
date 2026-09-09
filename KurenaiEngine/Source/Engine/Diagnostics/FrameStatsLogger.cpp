#include "FrameStatsLogger.h"

// フレーム統計の集計。積算を代入にしてはいけない理由は FrameStatsLogger.h にある
namespace Kurenai::Diagnostics
{
    void FrameStatsLogger::AddFrame(std::chrono::steady_clock::time_point now, const FrameSample& sample)
    {
        if (m_FrameCount == 0)
        {
            m_WindowStart = now;
        }

        ++m_FrameCount;
        m_CPUTimeSumMs += sample.CPUFrameTimeMs;
        m_GPUTimeSumMs += sample.GPUTimeMs;
        m_GPUWaitSumMs += sample.GPUWaitMs;
        m_WorstFrameTimeMs = std::max(m_WorstFrameTimeMs, sample.FrameTimeMs);
        m_CullTestedSum += sample.FrustumCullTested;
        m_CullCulledSum += sample.FrustumCullCulled;
        m_LODSwitchSum += sample.LODSwitchCount;
        m_LODFadingSum += sample.LODFadingCount;
        m_MeshCullTestedSum += sample.MeshCullTested;
        m_MeshCullCulledSum += sample.MeshCullCulled;
        m_DrawCallsGBufferSum += sample.DrawCallsGBuffer;
        m_DrawCallsShadowSum += sample.DrawCallsShadow;
        m_DrawCallsDepthPrepassSum += sample.DrawCallsDepthPrepass;
        m_InstancedBatchSum += sample.InstancedBatchCount;
        m_InstancedInstanceSum += sample.InstancedInstanceCount;
    }

    void FrameStatsLogger::AddMeshletSample(uint32_t tested, uint32_t frustumCulled, uint32_t occlusionCulled)
    {
        m_MeshletTestedSum += tested;
        m_MeshletFrustumCulledSum += frustumCulled;
        m_MeshletOcclusionCulledSum += occlusionCulled;
        ++m_MeshletSampleCount;
    }

    void FrameStatsLogger::Reset()
    {
        m_FrameCount = 0;
        m_CPUTimeSumMs = 0.0;
        m_GPUTimeSumMs = 0.0;
        m_GPUWaitSumMs = 0.0;
        m_WorstFrameTimeMs = 0.0f;
        m_CullTestedSum = 0;
        m_CullCulledSum = 0;
        m_LODSwitchSum = 0;
        m_LODFadingSum = 0;
        m_MeshCullTestedSum = 0;
        m_MeshCullCulledSum = 0;
        m_DrawCallsGBufferSum = 0;
        m_DrawCallsShadowSum = 0;
        m_DrawCallsDepthPrepassSum = 0;
        m_InstancedBatchSum = 0;
        m_InstancedInstanceSum = 0;
        m_MeshletTestedSum = 0;
        m_MeshletFrustumCulledSum = 0;
        m_MeshletOcclusionCulledSum = 0;
        m_MeshletSampleCount = 0;
    }
}
