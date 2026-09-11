#pragma once

#include <algorithm>

// 被覆率から求める全天の平均透過率(判断B)。
//
// IBL用キューブマップには雲を焼き込まない(Sky.hlsli の雲セクション、判断Aのコメント参照)
// ため、被覆率が上がってもキューブの明るさが晴天のまま据え置かれてしまう。
// これを補うため、キューブへ焼く天頂輝度にだけこの平均透過率を掛けて全体を暗くする。
//
// 【共有ヘッダーにする理由】値を決めるのはフレームの値を組み立てる側、
// 使うのは空のベイクと FrameConstants の両方。段階7.5の分割でこれらが
// 別の翻訳単位へ散るため、式を1か所へ置く。
namespace Kurenai::Rendering
{
    // 【物理的な導出ではない】実際の曇天は多重散乱・雲の厚みで複雑に減光するが、ここでは
    // 「被覆率0で1.0(無変化)、被覆率1でこの値まで直線的に落ちる」という単純な線形補間で
    // 済ませている。目的はIBLの明るさが被覆率に応じて定性的に下がることであり、
    // 精密な値は求めていない(実測で調整可能)
    inline constexpr float kCloudOvercastTransmittance = 0.35f;

    // 巻雲側の「全天が巻雲のときの透過率」。積雲の 0.35 より1に近い値にしてある。
    // 巻雲は光学的に薄く(CirrusDensityが積雲の1桁下)、全天を覆っても積雲ほど大きくは
    // 減光しないという定性的な近似であり、精密な値は求めていない(実測で調整可能)
    inline constexpr float kCirrusOvercastTransmittance = 0.75f;

    // 1層ぶんの「被覆率→平均透過率」の線形補間。
    // ComputeCloudAverageTransmittance が積雲・巻雲の両方でこの1つの式を共有する
    inline float ComputeCloudLayerTransmittance(bool layerEnabled, float coverage, float overcastTransmittance)
    {
        if (!layerEnabled)
        {
            return 1.0f;
        }
        const float clampedCoverage = std::clamp(coverage, 0.0f, 1.0f);
        // lerp(1.0f, overcastTransmittance, clampedCoverage)と同じ
        return 1.0f + (overcastTransmittance - 1.0f) * clampedCoverage;
    }

    // 巻雲(2層目)も加味した全天の平均透過率。
    // T = T_cumulus(積雲の被覆率) * T_cirrus(巻雲の被覆率) という2層の積で求める。
    // 巻雲を無効化・被覆率0にした場合は T_cirrus=1.0 になり、積雲だけの値になる
    inline float ComputeCloudAverageTransmittance(
        bool cloudEnabled, float coverage, bool cirrusEnabled, float cirrusCoverage)
    {
        const float cumulusTransmittance =
            ComputeCloudLayerTransmittance(cloudEnabled, coverage, kCloudOvercastTransmittance);
        const float cirrusTransmittance =
            ComputeCloudLayerTransmittance(cirrusEnabled, cirrusCoverage, kCirrusOvercastTransmittance);
        return cumulusTransmittance * cirrusTransmittance;
    }
}
