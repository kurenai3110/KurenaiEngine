#pragma once

#include <array>
#include <cstdint>
#include <d3d12.h>
#include <string>
#include <vector>
#include <wrl/client.h>

#include "RHI/GPUProfilerCore.h"
#include "RHI/IRHIGPUProfiler.h"

namespace Kurenai::RHI
{
    class DX12Device;

    class DX12GPUProfiler : public IRHIGPUProfiler
    {
    public:
        explicit DX12GPUProfiler(DX12Device* device);

        void BeginFrame() override;
        void BeginScope(const std::string& name) override;
        void EndScope() override;
        void EndFrame() override;

        const std::vector<GPUTimingResult>& GetResults() const override { return m_Core.GetResults(); }
        float GetTotalFrameTimeMs() const override { return m_Core.GetTotalFrameTimeMs(); }

    private:
        // 1スロットあたりのタイムスタンプ数: フレーム開始+終了の2つ + 区間ごとの開始/終了2つ。
        // リングの段数・区間数の上限・区間名・結果の集計はGPUProfilerCoreが持ち、
        // ここはクエリヒープ上の添字の割り付けだけを決める
        static constexpr uint32_t kQueriesPerSlot = 2 + GPUProfilerCore::kMaxScopesPerFrame * 2;

        uint32_t QueryIndex(uint32_t slotIndex, uint32_t offsetInSlot) const;
        // いま記録中のスロット(GPUProfilerCore::GetWriteIndex)の結果を確定させる
        void ResolveWriteSlot();

        DX12Device* m_Device;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_QueryHeap;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_ReadbackBuffer;
        uint64_t m_TimestampFrequency = 0;

        GPUProfilerCore m_Core{ "DX12" };
    };
}
