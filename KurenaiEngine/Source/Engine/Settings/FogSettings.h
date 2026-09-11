#pragma once

#include "../EngineDefaults.h"

namespace Kurenai
{
    struct FogSettings
    {
        bool Enabled = Defaults::FogEnabled;
        // 基準高度(m_FogRefHeight)での消散係数[1/m]。AerialPerspective.hlsl/PlanarReflection.hlslの
        // FogParams0.xへ渡る
        float Density = Defaults::FogDensity;
        // スケールハイト[m]。大きいほど霞が高くまで及ぶ(HeightFog.hlsli参照)
        float ScaleHeight = Defaults::FogScaleHeight;
        // 基準高度[m](ワールドY)。既定は水面の高さに合わせている
        float RefHeight = Defaults::FogRefHeight;
        // 不透明度の上限(1.0で遠方が完全に空の色まで行く)
        float MaxOpacity = Defaults::FogMaxOpacity;
    };
}
