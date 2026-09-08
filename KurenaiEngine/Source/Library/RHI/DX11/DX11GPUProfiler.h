#pragma once

#include <array>
#include <cstdint>
#include <d3d11.h>
#include <string>
#include <vector>
#include <wrl/client.h>

#include "RHI/GPUProfilerCore.h"
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

        const std::vector<GPUTimingResult>& GetResults() const override { return m_Core.GetResults(); }
        float GetTotalFrameTimeMs() const override { return m_Core.GetTotalFrameTimeMs(); }

    private:
        // 1スロットぶんのクエリ。リングの段数・区間数の上限・区間名・結果の集計は
        // GPUProfilerCoreが持ち、ここはID3D11Queryのオブジェクトだけを同じ添字で並べる。
        //
        // 【DX12より確保が重い】あちらはタイムスタンプ1本ぶんの添字で済むが、こちらは
        // 1区間につき2個、リングの段数だけ前もって作る(96区間なら776個)。生成は起動時の1回きり
        struct QuerySlot
        {
            Microsoft::WRL::ComPtr<ID3D11Query> DisjointQuery;
            Microsoft::WRL::ComPtr<ID3D11Query> FrameStartQuery;
            Microsoft::WRL::ComPtr<ID3D11Query> FrameEndQuery;
            std::array<Microsoft::WRL::ComPtr<ID3D11Query>, GPUProfilerCore::kMaxScopesPerFrame> BeginQueries;
            std::array<Microsoft::WRL::ComPtr<ID3D11Query>, GPUProfilerCore::kMaxScopesPerFrame> EndQueries;
        };

        Microsoft::WRL::ComPtr<ID3D11Query> CreateTimestampQuery() const;
        // いま記録中のスロット(GPUProfilerCore::GetWriteIndex)の結果を確定させる
        void ResolveWriteSlot();

        Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;
        GPUProfilerCore m_Core{ "DX11" };
        std::array<QuerySlot, GPUProfilerCore::kFrameLatency> m_QuerySlots;
    };
}
