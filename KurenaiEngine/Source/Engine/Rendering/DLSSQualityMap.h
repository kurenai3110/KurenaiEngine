#pragma once

#include <string>

#include "Core/Logger.h"
#include "RHI/IRHIDLSSContext.h"
#include "../Settings/PostProcessSettings.h"

namespace Kurenai::Rendering
{
    // エンジンの品質モード(UpscaleQualityMode)をRHI側のDLSS品質モード(RHI::DLSSQuality)へ写す。
    //
    // 【変換はここ1箇所だけにする】2つの列挙を直接キャストで行き来させると、
    // どちらかに段を足した瞬間に静かにずれる(RHI::DLSSQualityはさらにNGXの
    // NVSDK_NGX_PerfQuality_Valueへ写され、そちらの並びはまた別。DX12DLSSContext.cpp参照)。
    //
    // ヘッダーに置いてあるのは、解像度を決めるKurenaiEngine3D.cppと、
    // フレームの値を組み立てるRenderFrameBuild.cppの2つの翻訳単位から要るため
    inline RHI::DLSSQuality ToRHIDLSSQuality(UpscaleQualityMode mode)
    {
        switch (mode)
        {
        case UpscaleQualityMode::DLAA:             return RHI::DLSSQuality::DLAA;
        case UpscaleQualityMode::UltraQuality:     return RHI::DLSSQuality::UltraQuality;
        case UpscaleQualityMode::Quality:          return RHI::DLSSQuality::Quality;
        case UpscaleQualityMode::Balanced:         return RHI::DLSSQuality::Balanced;
        case UpscaleQualityMode::Performance:      return RHI::DLSSQuality::Performance;
        case UpscaleQualityMode::UltraPerformance: return RHI::DLSSQuality::UltraPerformance;
        default:
            // ここへ来るのは品質モードを足して対応表を直し忘れたときだけ。
            // UIのコンボは段数をstatic_assertで縛っているので通常は先に気づけるが、
            // 黙ってQualityへ落とすと「選んだ段と違う倍率で描かれている」形でしか現れない
            Core::Logger::Error(
                "DLSS",
                "ToRHIDLSSQuality: 未知の品質モード(" + std::to_string(static_cast<int>(mode)) +
                    ")です。対応表を直してください。今回はQualityとして扱います");
            return RHI::DLSSQuality::Quality;
        }
    }
}
