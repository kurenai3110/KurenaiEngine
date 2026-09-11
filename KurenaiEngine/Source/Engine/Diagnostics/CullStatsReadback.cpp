#include "CullStatsReadback.h"

#include <algorithm>
#include <iterator>

// カリング統計の読み戻し。設計の意図とリングの段数の理由は CullStatsReadback.h にある
namespace Kurenai::Diagnostics
{
    namespace
    {
        // 読み戻し用のバッファ1本。大きさはGPU側のカウンタバッファと同じにする
        RHI::BufferDesc MakeReadbackDesc(uint32_t counterCount)
        {
            RHI::BufferDesc desc;
            desc.Usage = RHI::BufferUsage::Readback;
            desc.SizeInBytes = static_cast<uint32_t>(sizeof(uint32_t)) * counterCount;
            desc.StrideInBytes = static_cast<uint32_t>(sizeof(uint32_t));
            return desc;
        }
    }

    void CullStatsReadback::CreateMeshletRing(RHI::IRHIDevice& device, uint32_t counterCount)
    {
        const RHI::BufferDesc desc = MakeReadbackDesc(counterCount);
        for (uint32_t i = 0; i < kRingSize; ++i)
        {
            m_MeshletRing[i] = device.CreateBuffer(desc);
        }
    }

    void CullStatsReadback::ResetMeshletRing()
    {
        for (auto& slot : m_MeshletRing)
        {
            slot.reset();
        }
        m_MeshletBindlessIndex = RHI::kInvalidBindlessIndex;
    }

    bool CullStatsReadback::ResolveMeshlet(uint32_t* outCounters, size_t byteCount)
    {
        const uint32_t oldestIndex = (m_MeshletIndex + 1) % kRingSize;
        const bool read =
            m_MeshletRing[oldestIndex] && m_MeshletRing[oldestIndex]->ReadbackData(outCounters, byteCount);
        m_MeshletIndex = (m_MeshletIndex + 1) % kRingSize;
        return read;
    }

    void CullStatsReadback::CreateModelRing(RHI::IRHIDevice& device, uint32_t counterCount)
    {
        const RHI::BufferDesc desc = MakeReadbackDesc(counterCount);
        for (uint32_t i = 0; i < kRingSize; ++i)
        {
            m_ModelRing[i] = device.CreateBuffer(desc);
        }
    }

    void CullStatsReadback::ResetModelRing()
    {
        for (auto& slot : m_ModelRing)
        {
            slot.reset();
        }
    }

    void CullStatsReadback::ResolveModel(uint32_t cpuFrustumCulled, uint32_t candidateCount)
    {
        // 今フレームのCPU側の結果を、GPUのコピーとまったく同じ位置へ積む。
        // 読むときに同じ位置から取れば、比べるのは同じフレームのもの同士になる
        m_ModelCpuFrustumHistory[m_ModelIndex] = cpuFrustumCulled;
        m_ModelCandidateHistory[m_ModelIndex] = candidateCount;

        const uint32_t oldest = (m_ModelIndex + 1) % kRingSize;
        uint32_t counters[Passes::kModelCullCounterCount] = {};
        if (m_ModelRing[oldest] && m_ModelRing[oldest]->ReadbackData(counters, sizeof(counters)))
        {
            m_ModelTested = counters[0];
            m_ModelFrustumCulled = counters[1];
            m_ModelOcclusionCulled = counters[2];
            m_ModelSurvived = counters[3];
            for (uint32_t region = 0; region < Passes::kModelCullRegionCount; ++region)
            {
                m_ModelRegionIssued[region] = counters[4 + region];
            }
            m_ModelComparedCpuFrustumCulled = m_ModelCpuFrustumHistory[oldest];
            m_ModelComparedCandidateCount = m_ModelCandidateHistory[oldest];
        }
        m_ModelIndex = (m_ModelIndex + 1) % kRingSize;
    }

    void CullStatsReadback::ClearModelStats()
    {
        m_ModelTested = 0;
        m_ModelFrustumCulled = 0;
        m_ModelOcclusionCulled = 0;
        m_ModelSurvived = 0;
        std::fill(std::begin(m_ModelRegionIssued), std::end(m_ModelRegionIssued), 0u);
    }
}
