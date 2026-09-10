#ifndef KURENAI_PUNCTUAL_EVALUATE_HLSLI
#define KURENAI_PUNCTUAL_EVALUATE_HLSLI

// 影を撃たないパスが使う punctual ライトの評価。
//
// 【なぜ1本へ寄せるか】同じ式が PlanarReflection.hlsl と ProbeShading.hlsli に
// 別々に置かれており、非コメント行で EvaluateDirectBRDF が28行、EvaluateLight が50行、
// どちらも完全に一致していた。片方だけ直すと、平面反射だけ・プローブだけ明るさが
// 変わるという形で出る。**平面反射はベースライン採取のどの構成でも描かれない**ので、
// 採取では捕まらない種類の食い違いになる。
//
// 【影を撃つ経路はここに無い】DirectLighting.hlsl の EvaluateLight はレイ予算を
// 持ち回るため引数も本体も違う。無理に1本にすると、影を撃たない側にまで
// 予算の受け渡しを持ち込むことになる。
//
// 【インクルードする側の責務】このヘッダーより前に次を用意すること。
//   - SpecularEnergy.hlsli (SpecularEnergyContext / GeometrySmith / DistributionGGX)
//   - LightAttenuation.hlsli (LightAttenuation / SpotAttenuation)
//   - ShaderInterop/GPULight.hlsli (struct GPULight)
//   - FresnelSchlick(cosTheta, F0)

// DirectLighting.hlsl/ProbeCapture.hlslのEvaluateDirectBRDFと同じ(拡散+鏡面を足した1つの値を返す)
float3 EvaluateDirectBRDF(
    float3 N, float3 V, float3 L, float NdotV, float3 albedo, float metallic, float roughness,
    SpecularEnergyContext energy)
{
    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float D = DistributionGGX(NdotH, roughness);
    float G = GeometrySmith(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(VdotH, F0);

    float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, 1e-4f) * energy.Compensation;

    if (energy.Mode == KURENAI_SPEC_COMP_KULLACONTY)
    {
        // 加算ローブはE(NdotL)を要る(DirectLighting.hlslの同じ箇所と同一の処理)
        const float2 brdfL = BRDFLUTTexture.SampleLevel(ColorSampler, float2(NdotL, energy.Roughness), 0).rg;
        specular += SpecularMultiScatterLobe(F0, energy.EssV, brdfL.x + brdfL.y, energy.Eavg, energy.Mode);
    }

    float3 kd = (1.0f - F) * (1.0f - metallic);
    float3 diffuse = kd * albedo / PI;

    return (diffuse + specular) * NdotL;
}

// 距離減衰。定義は LightAttenuation.hlsli にただ1つある
#include "LightAttenuation.hlsli"

float SpotAttenuation(float3 spotDirection, float3 L, float angleScale, float angleOffset)
{
    float t = saturate(dot(spotDirection, -L) * angleScale + angleOffset);
    return t * t;
}

// DirectLighting.hlsl/ProbeCapture.hlslのEvaluateLightと同じ(影なし)
float3 EvaluateLight(
    GPULight light, float3 worldPos, float3 N, float3 V, float NdotV, float3 albedo, float metallic, float roughness,
    SpecularEnergyContext energy)
{
    uint lightType = (uint)light.PositionType.w;
    float range = light.ColorRange.w;

    float3 L;
    float atten = 1.0f;

    if (lightType == 0u)
    {
        L = normalize(-light.DirectionAngle.xyz);
    }
    else
    {
        float3 toLight = light.PositionType.xyz - worldPos;
        float distSq = dot(toLight, toLight);
        if (distSq > range * range)
        {
            return float3(0.0f, 0.0f, 0.0f);
        }

        atten = LightAttenuation(
            lightType, toLight, distSq, range, light.Params.z, light.DirectionAngle.xyz, light.Params.w);
        if (atten <= 0.0f)
        {
            return float3(0.0f, 0.0f, 0.0f);
        }

        L = toLight * rsqrt(max(distSq, 1e-8f));

        if (lightType == 2u)
        {
            float spotAtten = SpotAttenuation(light.DirectionAngle.xyz, L, light.DirectionAngle.w, light.Params.x);
            if (spotAtten <= 0.0f)
            {
                return float3(0.0f, 0.0f, 0.0f);
            }
            atten *= spotAtten;
        }
    }

    if (dot(N, L) <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    return EvaluateDirectBRDF(N, V, L, NdotV, albedo, metallic, roughness, energy) * light.ColorRange.rgb * atten;
}

#endif // KURENAI_PUNCTUAL_EVALUATE_HLSLI
