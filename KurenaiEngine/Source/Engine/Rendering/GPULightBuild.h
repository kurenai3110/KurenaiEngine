#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Assets/Model.h"
#include "Rendering/ExposureMath.h"
#include "Rendering/GPULight.h"

// GPULight を**組み立てる**側。レイアウトは Rendering/GPULight.h が持つ。
//
// 【なぜ GPULight.h へ入れないのか】あちらは <DirectXMath.h> しか引かない軽いヘッダーで、
// GPUライトのレコードを読むだけの側(パス群)も含めて広くインクルードされている。
// 組み立てには Assets::Light と ComputeExposure が要るので、そこを分けてある。
//
// **「レイアウト」= GPULight.h /「組み立て」= このヘッダー。**
namespace Kurenai::Rendering
{
    // t5の構造化バッファに詰めるライトの最大数。実データ(BistroInterior.fbxで4灯)に対しては
    // 十分すぎる余裕を持たせてあるが、構造化バッファなのでこの容量自体がGPU時間へ影響することはない
    // (シェーダはLightCount.xまでしかループしないため)
    inline constexpr uint32_t kMaxLights = 1024;

    // MegaLightsTilePool.hlsl の kMegaLightsMaxLights と同じ値。あちらはライトごとの重みを
    // groupshared配列に置くためコンパイル時定数である必要があり、C++からの受け渡しでは代用できない。
    //
    // 【なぜ静的検査で縛るのか】候補プールは走査するライト数をこの値で頭打ちにするが、
    // タイルライトカリング(LightCulling.hlsl)は頭打ちしない。kMaxLightsをこれより大きくすると、
    // **判定を共有しているのに定義域だけが黙ってずれる**(あぶれた灯はカリングには入るが
    // 候補プールには入らない)。到達判定の共有では防げない食い違いなので、ここで止める
    inline constexpr uint32_t kMegaLightsTilePoolMaxLights = 1024;
    static_assert(
        kMaxLights <= kMegaLightsTilePoolMaxLights,
        "kMaxLightsを増やすなら MegaLightsTilePool.hlsl の kMegaLightsMaxLights も同じ値へ上げること"
        "(候補プールが走査するライト数の上限。超えるとタイルライトカリングと定義域がずれる)");

    // Assets::LightをGPU側のGPULightへ変換する。カンデラ/ルクスの測光量にEV100露出を直接掛けて
    // 表示レンジへ変換する(設計判断は「強度の単位」節を参照)。Frostbiteのスポット角度減衰用
    // lightAngleScale/lightAngleOffsetもここでCPU事前計算する
    inline GPULight MakeGPULight(const Assets::Light& light, float exposureEV100)
    {
        const float exposure = ComputeExposure(exposureEV100);
        const float radiance = light.Intensity * exposure;

        GPULight gpuLight{};
        gpuLight.PositionType = { light.Position[0], light.Position[1], light.Position[2], static_cast<float>(light.Type) };
        gpuLight.ColorRange = { light.Color[0] * radiance, light.Color[1] * radiance, light.Color[2] * radiance, light.Range };

        float angleScale = 0.0f;
        float angleOffset = 0.0f;
        if (light.Type == Assets::LightType::Spot)
        {
            // Frostbiteのスポット減衰式: t = saturate(dot(spotDir,-L)*scale + offset), atten = t*t
            const float cosOuter = std::cos(light.SpotOuterConeAngle);
            const float cosInner = std::cos(light.SpotInnerConeAngle);
            angleScale = 1.0f / std::max(0.001f, cosInner - cosOuter);
            angleOffset = -cosOuter * angleScale;
        }
        gpuLight.DirectionAngle = { light.Direction[0], light.Direction[1], light.Direction[2], angleScale };
        // Params.y = このライトが影を落とすか。ライトごとに切れるようにしてあるのは、
        // ピクセルあたりのシャドウレイ数に上限(Passes::LightingConstants.LightCount.y)があり、
        // 「影を出したいライト」に予算を回せるようにするため
        // Params.z = 光源そのものの半径[m]。0なら点光源。予約枠だった zw のうち z を使う。
        // 【平行光には入れない】太陽は MegaLights の対象外で、円盤サンプリングは
        // RTShadow.hlsl が別に持っている
        const float sourceRadius =
            (light.Type == Assets::LightType::Directional) ? 0.0f : std::max(0.0f, light.SourceRadius);
        // Params.y は影のフラグ。**bit0 = スクリーンスペースシャドウ / bit1 = レイトレース影レイ**
        // (Shaders/3D/LightAttenuation.hlsli と一致させること)。作者が置いたライトは
        // 両方を立てる ―― 1つの真偽値だった頃と挙動が変わらない。
        // 【リテラルで 3.0f と書かない】ビットの定義を変えたときに追随しない
        const float shadowFlags = light.CastShadow
                                      ? static_cast<float>(kLightShadowScreenSpace | kLightShadowRaytraced)
                                      : 0.0f;
        gpuLight.Params = { angleOffset, shadowFlags, sourceRadius, 0.0f };
        return gpuLight;
    }
}
