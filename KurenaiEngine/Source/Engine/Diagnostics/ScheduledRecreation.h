#pragma once

#include <cstdint>
#include <string>

#include "../Settings/SystemSettings.h"

namespace Kurenai
{
    // 【検証専用】ベースライン採取だけでは通らないGPUリソースの作り直し経路を、
    // 指定フレームで無人採取スクリプトから踏ませるための予約。
    enum class ScheduledRecreationKind
    {
        RenderResolution,  // 内部レンダー解像度の変更(超解像は無効にする)
        UpscaleOutput,     // 超解像の出力解像度の変更(超解像を有効にする)
        BufferPrecision,   // 中間バッファ精度の切り替え(精度依存PSOも作り直される)
        SceneLoad,         // シーンの切り替え(GPUリソースの破棄と再作成)
    };

    struct ScheduledRecreation
    {
        uint32_t Frame = 0; // m_TAAFrameIndex がこの値以上になった最初のフレームで発火
        ScheduledRecreationKind Kind = ScheduledRecreationKind::RenderResolution;
        uint32_t Width = 0; // RenderResolution / UpscaleOutput
        uint32_t Height = 0;
        BufferPrecision Precision = BufferPrecision::HDR; // BufferPrecisionのときだけ使う
        std::wstring SceneName; // SceneLoad(.ksceneのファイル名から拡張子を除いたもの)
    };
}
