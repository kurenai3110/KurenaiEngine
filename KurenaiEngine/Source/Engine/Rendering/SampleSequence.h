#pragma once

#include <cstdint>

// 低食い違い量列(Halton列)と、その周期。
//
// TAAのサブピクセルジッターが使う値をフレーム組み立て側と共有する。
namespace Kurenai::Rendering
{
    // 基数baseのradical inverse、すなわちindexを基数base表記にして
    // 小数点の左右を反転した値を返す([0,1)に収まる)。
    //
    // 乱数と違い、少ない点数でも区間内へ均等に散らばるのが要点で、
    // 8フレームぶん取ればピクセル内に8点が偏りなく配置される
    inline float RadicalInverse(uint32_t index, uint32_t base)
    {
        float result = 0.0f;
        float fraction = 1.0f / static_cast<float>(base);
        while (index > 0)
        {
            result += static_cast<float>(index % base) * fraction;
            index /= base;
            fraction /= static_cast<float>(base);
        }
        return result;
    }

    // TAAのジッター周期(フレーム数)。長いほど多くのサンプル位置を踏めるが、
    // その分だけ収束に時間がかかり、カメラが動いている間の見た目が不安定になる。
    // 8はUnreal Engine等でも使われる実用的な妥協点
    inline constexpr uint32_t kTAAJitterSampleCount = 8;
}
