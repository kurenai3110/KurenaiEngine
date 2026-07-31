// 反射プローブのキャプチャパス(フォワード)。プローブ位置から6方向を1面ずつ2Dレンダーターゲットへ
// 描画し、その結果をIBLConvolve.hlslのCSCopyCaptureToCubeFaceがキューブマップの該当面へ書き写す。
// キューブマップへ直接描画(面ごとのRTV)はRHIが持っていないため、この「2Dへ描いてUAVでコピー」
// という経路を採っている(既に実績のある面ごとUAV書き込みの仕組みをそのまま再利用できる)。
//
// ライティングはTransparent.hlsl(半透明フォワードパス)と同じ式を使う。プローブに映るのは
// 「直接光 + スカイボックス由来のグローバルIBL」までで、SSAO/SSIL/SSRのスクリーンスペース手法や
// 他のプローブの寄与は含まない(含めるとプローブ同士が相互参照して発散するため、
// 反射の中の反射は1バウンスで打ち切るのが定石)。
//
// 【定数バッファの与え方】b0はFrameConstantsをそのまま使うが、エンジン側(KurenaiEngine3D::Render)は
// このパス専用のバッファへ次の値を詰めて渡す:
//   ViewProj       … プローブのその面のビュー・プロジェクション(ラスタライズに使う)
//   View           … 「カメラ」のビュー行列。カスケード選択の深度(CascadeSplits)がカメラ視錐台
//                     基準で求められているため、ここだけはプローブではなくカメラのものを渡す
//   CameraPosition … プローブのワールド座標(視線ベクトルVの起点。プローブから見た放射輝度を
//                     捉えるのが目的なので実際のカメラ位置ではない)
// これによりシェーダー側はFrameConstantsの宣言を一切変えずに済む。
//
// 既知の制約: カスケードシャドウマップはカメラ視錐台に合わせて分割・フィットされているため、
// カメラから遠く離れた位置のプローブを焼くとシャドウマップの範囲外になり影が落ちない
// (ComputeShadowFactorが範囲外を「影なし」として返す)。プローブは基本的に視界内で焼く前提とする。
#include "SpecularEnergy.hlsli"
#include "Samplers.hlsli"

static const float PI = 3.14159265359f;

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 CascadeViewProj[4];
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
    float4x4 View;
    float4x4 Proj;
    float4 AmbientColor;
    float4 CascadeSplits;
    float4 ShadowParams;
    // x=t8のライトリストの有効数(Transparent.hlslと同じくFrameConstants末尾で受け取る。
    // b1はObjectConstantsが占有していてLightingConstantsを置けないため)
    float4 ActiveLightCount;
};

// GBuffer.hlsl/Transparent.hlslのObjectConstantsと同じレイアウト
cbuffer ObjectConstants : register(b1)
{
    float4x4 World;
    float4x4 NormalMatrix;
    float MetallicFactor;
    float RoughnessFactor;
    float TangentSignFlip;
    float AlphaCutoff;
    float3 EmissiveFactor;
    float ObjectPadding;
    float4 BaseColorFactor;
};

// DirectLighting.hlsl側のstruct GPULightと並び・ストライド(64バイト)を一致させる必要がある
struct GPULight
{
    float4 PositionType;
    float4 ColorRange;
    float4 DirectionAngle;
    float4 Params;
};
StructuredBuffer<GPULight> Lights : register(t8);

Texture2D BaseColorTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D MetallicRoughnessTexture : register(t2);
Texture2D EmissiveTexture : register(t3);
// カスケードシャドウマップ(t4のTexture2DArray)とそのPCSSサンプリング。
// DirectLighting.hlsl/Transparent.hlslと同じ実装を共有しているため、プローブに焼かれる影と
// 本編の影が食い違うことはない。FrameConstants(CascadeViewProj/CascadeSplits/ShadowParams)と
// DataSamplerを参照するため、それらの宣言より後でインクルードする必要がある
#include "ShadowSampling.hlsli"
// スカイボックス由来のグローバルIBL。プローブに映る面の環境光として使う
TextureCube IrradianceTexture : register(t9);
TextureCube PrefilteredEnvTexture : register(t10);
Texture2D BRDFLUTTexture : register(t11);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
    float4 Tangent : TANGENT;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float3 Normal : NORMAL;
    float3 WorldPos : TEXCOORD1;
    float2 UV : TEXCOORD0;
    float4 Tangent : TANGENT;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float3 worldPos = mul(float4(input.Position, 1.0f), World).xyz;
    output.Position = mul(float4(worldPos, 1.0f), ViewProj);
    output.Normal = mul(input.Normal, (float3x3)NormalMatrix);
    output.WorldPos = worldPos;
    output.UV = input.UV;
    output.Tangent = float4(mul(input.Tangent.xyz, (float3x3)World), input.Tangent.w * TangentSignFlip);
    return output;
}

// GBuffer.hlslのComputeTangentFrameと同じ(ピクセル単位でGram-Schmidt再直交化する)
float3x3 ComputeTangentFrame(float3 N, float4 tangent)
{
    float3 T = normalize(tangent.xyz - N * dot(N, tangent.xyz));
    float3 B = cross(N, T) * tangent.w;
    return float3x3(T, B, N);
}

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * d * d, 1e-6f);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// DirectLighting.hlslのEvaluateDirectBRDFと同じ(拡散+鏡面を足した1つの値を返す)
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

float DistanceAttenuation(float distSq, float range)
{
    float factor = distSq / max(range * range, 1e-4f);
    float window = saturate(1.0f - factor * factor);
    return (window * window) / max(distSq, 0.0001f);
}

float SpotAttenuation(float3 spotDirection, float3 L, float angleScale, float angleOffset)
{
    float t = saturate(dot(spotDirection, -L) * angleScale + angleOffset);
    return t * t;
}

// DirectLighting.hlslのEvaluateLightと同じ(影なし)
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

        atten = DistanceAttenuation(distSq, range);
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

// スカイボックス由来のグローバルIBL(DeferredLighting.hlslのEvaluateIBLと同じ式。
// キャプチャ時にはAO/GIバッファが無いため常にao=1として扱い、スペキュラオクルージョンも省く)。
// 昼度(AmbientColor.a)による夜間減衰は、手続き空の導入でどこでも掛けなくなった(21.4節)。
// 空のキューブマップ自体が太陽高度に応じて暗くなるため、焼き込み時にも使用時にも不要
float3 EvaluateGlobalIBL(float3 N, float3 V, float3 albedo, float metallic, float roughness, float3 brdf, int compensationMode)
{
    const float NdotV = saturate(dot(N, V));
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    const float3 irradiance = IrradianceTexture.Sample(MaterialSampler, N).rgb;
    const float3 fresnelRoughness =
        F0 + (max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0) - F0) * pow(saturate(1.0f - NdotV), 5.0f);
    const float3 kd = (1.0f - fresnelRoughness) * (1.0f - metallic);
    const float3 diffuseIBL = kd * albedo * irradiance;

    const float3 R = reflect(-V, N);
    const float mipLevel = roughness * ShadowParams.y;
    const float3 prefiltered = PrefilteredEnvTexture.SampleLevel(MaterialSampler, R, mipLevel).rgb;
    // 乗算型(モード1・2)は単一散乱項へ倍率として掛かり、Kulla-Conty(3)は加算ローブを足す
    // (DeferredLighting.hlslのEvaluateIBLと同じ形。加算ぶんは拡散イラディアンスに掛かる)
    const float3 FssEss = F0 * brdf.x + brdf.y;
    const float Ess = brdf.x + brdf.y;
    const float3 specularIBL =
        prefiltered * FssEss * SpecularEnergyCompensation(F0, brdf, compensationMode)
        + SpecularMultiScatterIBL(F0, FssEss, Ess, compensationMode) * irradiance;

    return diffuseIBL + specularIBL;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 baseColorSample = BaseColorTexture.Sample(MaterialSampler, input.UV) * BaseColorFactor;

    // 不透明パスと同じアルファカットアウト(葉・フェンス等をプローブでも正しく抜く)
    clip(baseColorSample.a - AlphaCutoff);

    float3 geometricNormal = normalize(input.Normal);
    float2 normalXY = NormalTexture.Sample(MaterialSampler, input.UV).xy * 2.0f - 1.0f;
    float normalZ = sqrt(saturate(1.0f - dot(normalXY, normalXY)));
    float3 normalSample = float3(normalXY, normalZ);
    float3x3 tbn = ComputeTangentFrame(geometricNormal, input.Tangent);
    float3 N = normalize(mul(normalSample, tbn));

    float3 metallicRoughnessSample = MetallicRoughnessTexture.Sample(MaterialSampler, input.UV).rgb;
    float metallic = saturate(MetallicFactor * metallicRoughnessSample.b);
    // RoughnessFactorが負の場合はソースデータにラフネス係数が無かったことを表す
    // (Assets::kInvalidMaterialFactor)。GBuffer.hlslと同じく係数1.0として扱う
    float roughnessFactor = (RoughnessFactor < 0.0f) ? 1.0f : RoughnessFactor;
    float roughness = clamp(roughnessFactor * metallicRoughnessSample.g, 0.045f, 1.0f);

    float3 emissive = EmissiveTexture.Sample(MaterialSampler, input.UV).rgb * EmissiveFactor;

    float3 albedo = baseColorSample.rgb;
    // CameraPositionにはプローブのワールド座標が入っている(ファイル冒頭参照)
    float3 V = normalize(CameraPosition.xyz - input.WorldPos);
    float NdotV = saturate(dot(N, V)) + 1e-5f;

    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    // LUTの第3成分(Eavg)はKulla-Conty方式だけが使う(14.9.2.1節)
    const float3 brdf = BRDFLUTTexture.Sample(ColorSampler, float2(NdotV, roughness)).rgb;
    const SpecularEnergyContext energy = MakeSpecularEnergyContext(F0, brdf, roughness, ShadowParams.w);

    float3 color = float3(0.0f, 0.0f, 0.0f);

    // --- 太陽(b0、カスケードシャドウ付き) ---
    float3 sunL = normalize(-LightDirection.xyz);
    float sunNdotL = saturate(dot(N, sunL));
    if (sunNdotL > 0.0f)
    {
        // カスケード選択はカメラ視錐台基準(FrameConstants.Viewはカメラのビュー行列)
        float viewDepth = mul(float4(input.WorldPos, 1.0f), View).z;
        float shadow = ComputeCascadedShadowFactor(input.WorldPos, viewDepth, sunNdotL);
        color += EvaluateDirectBRDF(N, V, sunL, NdotV, albedo, metallic, roughness, energy) * LightColor.rgb * shadow;
    }

    // --- t8のライトリスト(影なし) ---
    uint lightCount = (uint)ActiveLightCount.x;
    [loop]
    for (uint i = 0; i < lightCount; ++i)
    {
        color += EvaluateLight(Lights[i], input.WorldPos, N, V, NdotV, albedo, metallic, roughness, energy);
    }

    // --- スカイボックス由来のグローバルIBL(環境光) ---
    // ShadowParams.z = IBL強度倍率(Enable IBL無効なら0)。無効時はDeferredLighting.hlslと同じ
    // 定数色アンビエントへフォールバックし、プローブが真っ黒に焼けるのを防ぐ
    if (ShadowParams.z > 0.0f)
    {
        color += EvaluateGlobalIBL(N, V, albedo, metallic, roughness, brdf, energy.Mode) * ShadowParams.z;
    }
    else
    {
        color += (albedo * (1.0f - metallic) / PI) * AmbientColor.rgb;
    }

    color += emissive;

    return float4(color, 1.0f);
}
