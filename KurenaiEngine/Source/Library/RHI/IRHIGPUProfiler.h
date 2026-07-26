#pragma once

#include <string>
#include <vector>

#include "KurenaiTypes.h"

namespace Kurenai::RHI
{
    struct GPUTimingResult
    {
        std::string Name;
        float TimeMs = 0.0f;
    };

    // GPUタイムスタンプクエリによる区間計測。DX11(ID3D11Query)とDX12(ID3D12QueryHeap)で
    // 実装機構が大きく異なるため、他のRHIリソースと同様にIRHIDevice経由でバックエンド実装を生成する。
    // ネスト不可・シーケンシャルな区間のみをサポートする(Shadow→GBuffer→...のような直列パスの計測用)
    class KURENAI_LIB_API IRHIGPUProfiler
    {
    public:
        virtual ~IRHIGPUProfiler() = default;

        // フレームの描画コマンド記録の最初に呼ぶ。内部で数フレーム前の計測結果を確定させてから、
        // このフレーム用の計測を開始する
        virtual void BeginFrame() = 0;

        // 計測したい区間の開始/終了を記録する
        virtual void BeginScope(const std::string& name) = 0;
        virtual void EndScope() = 0;

        // フレームの描画コマンド記録の最後(Present呼び出しより前)に呼ぶ
        virtual void EndFrame() = 0;

        // 直近に確定した(数フレーム遅れの)各区間の計測結果
        virtual const std::vector<GPUTimingResult>& GetResults() const = 0;
        // 直近に確定したフレーム全体のGPU実行時間
        virtual float GetTotalFrameTimeMs() const = 0;
    };
}
