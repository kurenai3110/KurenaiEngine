#pragma once

#include <d3d12.h>

#include "RHI/IRHIDLSSContext.h"

// NGXのヘッダはD3D12の型を自前でtypedefするため、d3d12.hより後に読むこと
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

namespace Kurenai::RHI
{
    class DX12Device;

    // DLSS Super Resolution / DLAA の評価コンテキスト(DX12専用)。
    //
    // 【NGXの初期化はここではなくDX12Deviceが持つ】NVSDK_NGX_D3D12_Initはプロセス/デバイス単位の
    // 状態で、機能判定(DetectDLSSSupport)の時点で済んでいる必要がある。ここが持つのは
    // 「フィーチャ(解像度と品質モードごとの内部状態と履歴)」と、その評価に使うパラメータブロックだけ
    class DX12DLSSContext final : public IRHIDLSSContext
    {
    public:
        explicit DX12DLSSContext(DX12Device* device);
        ~DX12DLSSContext() override;

        bool QueryOptimalSettings(
            uint32_t outputWidth, uint32_t outputHeight, DLSSQuality quality,
            DLSSOptimalSettings& outSettings) override;
        bool EnsureFeature(IRHICommandList* commandList, const DLSSFeatureDesc& desc) override;
        bool Evaluate(IRHICommandList* commandList, const DLSSEvaluateDesc& desc) override;

    private:
        // フィーチャとパラメータブロックを解放する。GPUがまだ参照している可能性があるため、
        // 呼び出し側でWaitForGPUIdleを済ませてから呼ぶこと
        void ReleaseFeature();

        DX12Device* m_Device = nullptr;

        // NVSDK_NGX_D3D12_AllocateParametersで取ったフィーチャ用のパラメータブロック。
        // 能力問い合わせ用(GetCapabilityParameters)とは別物で、こちらは自分でDestroyする
        NVSDK_NGX_Parameter* m_Parameters = nullptr;
        NVSDK_NGX_Handle* m_Handle = nullptr;

        // 現在のフィーチャの生成条件。EnsureFeatureはこれと一致していれば何もしない
        DLSSFeatureDesc m_CurrentDesc{};
        bool m_HasFeature = false;
    };
}
