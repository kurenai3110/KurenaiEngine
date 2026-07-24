#pragma once

#include <array>
#include <cstdint>
#include <d3d11.h>
#include <string>
#include <vector>
#include <wrl/client.h>

#include "RHI/IRHIGPUProfiler.h"

namespace Kurenai::RHI
{
    class DX11GPUProfiler : public IRHIGPUProfiler
    {
    public:
        DX11GPUProfiler(Microsoft::WRL::ComPtr<ID3D11Device> device, Microsoft::WRL::ComPtr<ID3D11DeviceContext> context);

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

        struct FrameSlot
        {
            Microsoft::WRL::ComPtr<ID3D11Query> DisjointQuery;
            Microsoft::WRL::ComPtr<ID3D11Query> FrameStartQuery;
            Microsoft::WRL::ComPtr<ID3D11Query> FrameEndQuery;
            std::array<Microsoft::WRL::ComPtr<ID3D11Query>, kMaxScopesPerFrame> BeginQueries;
            std::array<Microsoft::WRL::ComPtr<ID3D11Query>, kMaxScopesPerFrame> EndQueries;
            std::array<std::string, kMaxScopesPerFrame> ScopeNames;
            uint32_t ScopeCount = 0;
            bool Pending = false; // EndFrame済みでGetDataによる結果確定を待っている状態か
        };

        Microsoft::WRL::ComPtr<ID3D11Query> CreateTimestampQuery() const;
        void ResolveSlot(FrameSlot& slot);

        Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;
        std::array<FrameSlot, kFrameLatency> m_Slots;
        uint32_t m_WriteIndex = 0;

        std::vector<GPUTimingResult> m_Results;
        float m_TotalFrameTimeMs = 0.0f;
    };
}
