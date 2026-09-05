#pragma once

#include "../EngineDefaults.h"

namespace Kurenai
{
    struct StarsSettings
    {
        // --- 星空 ---
        // 夜空の星。Sky.hlsliのSkyColorが方向ハッシュで解析的に描く(テクスチャは使わない)。
        // **IBLキューブ(SkyGenerate.hlsl)へは焼かない**ので、これらを変えても空の焼き直しは要らない
        // (雲の風と同じ扱い。m_SkyBakeDirtyを立てないこと)
        bool Enabled = Defaults::StarsEnabled;
        float Density = Defaults::StarsDensity;
        float Brightness = Defaults::StarsBrightness;
        // またたきの強さ。既定0。上げるとTAAがちらつきとして拾い、A/B比較の再現性も落ちる
        float Twinkle = Defaults::StarsTwinkle;
    };
}
