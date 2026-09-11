#pragma once

#include "../EngineDefaults.h"

namespace Kurenai
{
    struct EmissiveLightSettings
    {
        // シーン全体の自発光(エミッシブ)の強度倍率。MakeObjectConstantsがmesh.EmissiveFactorへ
        // 乗算する。glTFのemissiveFactorは通常1.0以下に収まるため、G-Bufferのエミッシブを
        // HDR化(R11G11B10_Float)しただけでは照明器具の輝度が1.0を超えず、ブルームが効かない。
        // アセットを再オーサリングせずにHDRな自発光を得るための倍率
        // (KHR_materials_emissive_strengthをインポータが読むようになれば本来はそちらが正しい)
        float Intensity = Defaults::EmissiveIntensity;

        bool LightsEnabled = Defaults::EmissiveLightsEnabled;
        // DDGIにも自発光を加算したままにするか(=二重に数えるか)。既定は抑止する
        bool LightsDoubleCountGI = Defaults::EmissiveLightsDoubleCountGI;
        float LightsCutoffIrradiance = Defaults::EmissiveLightsCutoffIrradiance;
        int LightsMaxCount = Defaults::EmissiveLightsMaxCount;
    };
}
