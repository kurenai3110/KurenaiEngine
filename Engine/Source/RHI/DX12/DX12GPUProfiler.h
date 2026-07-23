#pragma once

#include <array>
#include <cstdint>
#include <d3d12.h>
#include <string>
#include <vector>
#include <wrl/client.h>

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

        const std::vector<GPUTimingResult>& GetResults() const override { return m_Results; }
        float GetTotalFrameTimeMs() const override { return m_TotalFrameTimeMs; }

    private:
        // GPU実行がCPUの記録より数フレーム遅れてもクエリ結果を取りこぼさないためのリングバッファ段数
        static constexpr uint32_t kFrameLatency = 4;
        static constexpr uint32_t kMaxScopesPerFrame = 16;
        // 1スロットあたりのタイムスタンプ数: フレーム開始+終了の2つ + 区間ごとの開始/終了2つ
        static constexpr uint32_t kQueriesPerSlot = 2 + kMaxScopesPerFrame * 2;

        struct FrameSlot
        {
            std::array<std::string, kMaxScopesPerFrame> ScopeNames;
            uint32_t ScopeCount = 0;
            bool Pending = false; // EndFrame済みでリードバック結果の確定を待っている状態か
        };

        uint32_t QueryIndex(uint32_t slotIndex, uint32_t offsetInSlot) const;
        void ResolveSlot(FrameSlot& slot, uint32_t slotIndex);

        DX12Device* m_Device;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_QueryHeap;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_ReadbackBuffer;
        uint64_t m_TimestampFrequency = 0;

        std::array<FrameSlot, kFrameLatency> m_Slots;
        uint32_t m_WriteIndex = 0;

        std::vector<GPUTimingResult> m_Results;
        float m_TotalFrameTimeMs = 0.0f;
    };
}
