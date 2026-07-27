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
Texture2D ShadowMap0 : register(t4);
Texture2D ShadowMap1 : register(t5);
Texture2D ShadowMap2 : register(t6);
Texture2D ShadowMap3 : register(t7);
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

// DirectLighting.hlsl/Transparent.hlslのPCSSと同じ式だが、こちらは128x128程度の低解像度な
// キューブ面へ書き込むためタップ数を落とした単純なPCF(3x3)にしている。プローブに映る影は
// 反射像としてさらに縮小・ぼかされるため、半影の正確さより焼き直しのコストを優先する
float ComputeShadowFactor(Texture2D shadowMap, float4x4 cascadeViewProj, float3 worldPos, float NdotL)
{
    float4 lightClipPos = mul(float4(worldPos, 1.0f), cascadeViewProj);
    float3 lightNdc = lightClipPos.xyz / lightClipPos.w;

    if (abs(lightNdc.x) > 1.0f || abs(lightNdc.y) > 1.0f || lightNdc.z < 0.0f || lightNdc.z > 1.0f)
    {
        return 1.0f;
    }

    float2 shadowUV = float2(lightNdc.x * 0.5f + 0.5f, 1.0f - (lightNdc.y * 0.5f + 0.5f));

    // バイアスはDirectLighting.hlslと同じ値に揃える(同じシャドウマップを読むため)
    const float kShadowBiasMin = 0.0005f;
    const float kShadowBiasMax = 0.0025f;
    const float bias = lerp(kShadowBiasMax, kShadowBiasMin, NdotL);
    const float compareDepth = lightNdc.z - bias;

    const float kTexelSize = 1.0f / 2048.0f;

    const int kPCFTaps = 3;
    const int kPCFHalf = kPCFTaps / 2;
    float shadowSum = 0.0f;

    [unroll]
    for (int py = -kPCFHalf; py <= kPCFHalf; ++py)
    {
        [unroll]
        for (int px = -kPCFHalf; px <= kPCFHalf; ++px)
        {
            const float2 offset = float2(px, py) * kTexelSize;
            const float sampleDepth = shadowMap.Sample(DataSampler, shadowUV + offset).r;
            shadowSum += (sampleDepth < compareDepth) ? 0.0f : 1.0f;
        }
    }

    return shadowSum / float(kPCFTaps * kPCFTaps);
}

// カスケード選択はカメラ基準(FrameConstants.Viewにカメラのビュー行列が入っている。
// ファイル冒頭の「定数バッファの与え方」参照)
float ComputeCascadedShadowFactor(float3 worldPos, float viewDepth, float NdotL)
{
    int cascadeIndex = 0;
    if (viewDepth > CascadeSplits.x) cascadeIndex = 1;
    if (viewDepth > CascadeSplits.y) cascadeIndex = 2;
    if (viewDepth > CascadeSplits.z) cascadeIndex = 3;

    if (cascadeIndex == 0)
    {
        return ComputeShadowFactor(ShadowMap0, CascadeViewProj[0], worldPos, NdotL);
    }
    else if (cascadeIndex == 1)
    {
        return ComputeShadowFactor(ShadowMap1, CascadeViewProj[1], worldPos, NdotL);
    }
    else if (cascadeIndex == 2)
    {
        return ComputeShadowFactor(ShadowMap2, CascadeViewProj[2], worldPos, NdotL);
    }
    else
    {
        return ComputeShadowFactor(ShadowMap3, CascadeViewProj[3], worldPos, NdotL);
    }
}

// DirectLighting.hlslのEvaluateDirectBRDFと同じ(拡散+鏡面を足した1つの値を返す)
float3 EvaluateDirectBRDF(
    float3 N, float3 V, float3 L, float NdotV, float3 albedo, float metallic, float roughness,
    float3 energyCompensation)
{
    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float D = DistributionGGX(NdotH, roughness);
    float G = GeometrySmith(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(VdotH, F0);

    float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, 1e-4f) * energyCompensation;
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
    float3 energyCompensation)
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

    return EvaluateDirectBRDF(N, V, L, NdotV, albedo, metallic, roughness, energyCompensation) * light.ColorRange.rgb * atten;
}

// スカイボックス由来のグローバルIBL(DeferredLighting.hlslのEvaluateIBLと同じ式。
// キャプチャ時にはAO/GIバッファが無いため常にao=1として扱い、スペキュラオクルージョンも省く)。
// 夜間減衰(AmbientColor.a)をここで掛けないのは、プローブを使う側のEvaluateIBLが実行時に
// 改めて掛けるため。焼き込み時にも掛けると二重に暗くなる
float3 EvaluateGlobalIBL(float3 N, float3 V, float3 albedo, float metallic, float roughness, float2 brdf, float3 energyCompensation)
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
    const float3 specularIBL = prefiltered * (F0 * brdf.x + brdf.y) * energyCompensation;

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
    const float2 brdf = BRDFLUTTexture.Sample(ColorSampler, float2(NdotV, roughness)).rg;
    const float3 energyCompensation = SpecularEnergyCompensation(F0, brdf, ShadowParams.w);

    float3 color = float3(0.0f, 0.0f, 0.0f);

    // --- 太陽(b0、カスケードシャドウ付き) ---
    float3 sunL = normalize(-LightDirection.xyz);
    float sunNdotL = saturate(dot(N, sunL));
    if (sunNdotL > 0.0f)
    {
        // カスケード選択はカメラ視錐台基準(FrameConstants.Viewはカメラのビュー行列)
        float viewDepth = mul(float4(input.WorldPos, 1.0f), View).z;
        float shadow = ComputeCascadedShadowFactor(input.WorldPos, viewDepth, sunNdotL);
        color += EvaluateDirectBRDF(N, V, sunL, NdotV, albedo, metallic, roughness, energyCompensation) * LightColor.rgb * shadow;
    }

    // --- t8のライトリスト(影なし) ---
    uint lightCount = (uint)ActiveLightCount.x;
    [loop]
    for (uint i = 0; i < lightCount; ++i)
    {
        color += EvaluateLight(Lights[i], input.WorldPos, N, V, NdotV, albedo, metallic, roughness, energyCompensation);
    }

    // --- スカイボックス由来のグローバルIBL(環境光) ---
    // ShadowParams.z = IBL強度倍率(Enable IBL無効なら0)。無効時はDeferredLighting.hlslと同じ
    // 定数色アンビエントへフォールバックし、プローブが真っ黒に焼けるのを防ぐ
    if (ShadowParams.z > 0.0f)
    {
        color += EvaluateGlobalIBL(N, V, albedo, metallic, roughness, brdf, energyCompensation) * ShadowParams.z;
    }
    else
    {
        color += (albedo * (1.0f - metallic) / PI) * AmbientColor.rgb;
    }

    color += emissive;

    return float4(color, 1.0f);
}
