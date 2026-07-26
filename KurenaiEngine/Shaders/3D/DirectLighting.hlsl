// 直接光パス。G-Buffer(Albedo/Normal/Material/Depth)とシャドウマップから
// Cook-Torrance(GGX)のPBRで直接光(拡散+鏡面反射、シャドウ適用済み)だけを計算し、
// 専用のレンダーターゲットへ書き出す(環境光・間接光は含まない)。
// 太陽(平行光、b0、カスケードシャドウ付き)に加え、t8の構造化バッファに詰めたポイント/スポットライトを
// 影なしでループ加算する(詳細はdocs/Architecture.htmlの「複数ライト」「カスケードシャドウマップ」章を参照)。
// この結果はDeferredLightingパス(最終合成)とSSIL_VisibilityBitmask.hlsl(間接光サンプルの
// 簡易直接光の代わりに実際の直接光を使うことでシャドウも含めて正確にする)の両方からサンプルされる。
// レンダー解像度と同じ内部解像度で、HDR(トーンマップ前)の値をR32G32B32A32_Floatへ書き込む。
#include "NormalEncoding.hlsli"

static const float PI = 3.14159265359f;

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    // カスケードシャドウマップ(CSM)のカスケードごとのライト視点ビュー・プロジェクション行列
    float4x4 CascadeViewProj[4];
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
    float4x4 View;
    float4x4 Proj;
    float4 AmbientColor;
    // 各カスケードのView空間far距離(x=カスケード0, y=1, z=2, w=3)。ピクセルのView空間深度を
    // これと比較してどのカスケードのシャドウマップを使うか選ぶ
    float4 CascadeSplits;
    // x: PCSS(Percentage Closer Soft Shadows)のライトサイズ(シャドウマップUV空間の係数)。
    // ブロッカーサーチ・半影の広さの基準になる
    float4 ShadowParams;
};

// ポイント/スポットライト1灯ぶんのデータ。C++側 KurenaiEngine3D.cpp の GPULight と
// 並び・ストライド(64バイト)を一致させる必要がある。既存の SSAOConstants/SSILConstants と同様、
// パッキング規則の解釈揺れを避けるためメンバはすべて float4 単位で宣言する
struct GPULight
{
    float4 PositionType;   // xyz=ワールド座標, w=LightType(0=Directional, 1=Point, 2=Spot)
    // rgb = Color * Intensity[cd] * exposure(EV100)。カンデラ→露出済みの最終放射輝度で、
    // CPU側(MakeGPULight)で計算してあるためシェーダ側はそのまま乗算するだけでよい
    float4 ColorRange;     // rgb=露出済み放射輝度, w=Range
    float4 DirectionAngle; // xyz=向き(正規化済み), w=spotAngleScale
    float4 Params;         // x=spotAngleOffset, yzw=未使用(エリアライト用に予約)
};
StructuredBuffer<GPULight> Lights : register(t8);

// DirectLighting.hlsl側のこの宣言とC++側 KurenaiEngine3D.cpp の LightingConstants を一致させる必要がある
cbuffer LightingConstants : register(b1)
{
    uint4 LightCount; // x=有効ライト数, yzw=未使用
};

Texture2D AlbedoTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D MaterialTexture : register(t2);
Texture2D DepthTexture : register(t3);
Texture2D ShadowMap0 : register(t4);
Texture2D ShadowMap1 : register(t5);
Texture2D ShadowMap2 : register(t6);
Texture2D ShadowMap3 : register(t7);
SamplerState DefaultSampler : register(s0);

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

// 頂点バッファなしで画面全体を覆う三角形を1枚だけ生成する定番のテクニック
PSInput VSMain(uint vertexID : SV_VertexID)
{
    PSInput output;
    output.UV = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(output.UV.x * 2.0f - 1.0f, 1.0f - output.UV.y * 2.0f, 0.0f, 1.0f);
    return output;
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos = mul(clipPos, InvViewProj);
    return worldPos.xyz / worldPos.w;
}

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * d * d, 1e-6f);
}

float GeometrySchlickGGX(float NdotX, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdotX / (NdotX * (1.0f - k) + k);
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// PCSS(Percentage Closer Soft Shadows)。ライト視点のクリップ空間へ変換した上で、
// (1)近傍のブロッカー(受光点より手前=光源側にある遮蔽物)の平均深度を探し、
// (2)受光点との深度差から半影(ペナンブラ)の広さを推定し、
// (3)その広さでPCF(複数タップの深度比較平均)を行う。
// ブロッカーが見つからない場合は完全に光が当たるとみなしPCFをスキップする(コスト削減も兼ねる)。
// 戻り値は0(完全に影)〜1(完全に光が当たる)の連続値。シャドウマップの範囲外は影を落とさない。
//
// 本来のPCSS(Fernando 2005)は透視投影のライトを前提に「受光点までの距離」で半影の広さを
// スケールするが、このエンジンの平行光は正射影のシャドウマップを使うため、代わりに
// 正規化された深度値([0,1])同士の比をそのまま使う近似で代用している
float ComputeShadowFactor(Texture2D shadowMap, float4x4 cascadeViewProj, float3 worldPos, float NdotL)
{
    float4 lightClipPos = mul(float4(worldPos, 1.0f), cascadeViewProj);
    float3 lightNdc = lightClipPos.xyz / lightClipPos.w;

    if (abs(lightNdc.x) > 1.0f || abs(lightNdc.y) > 1.0f || lightNdc.z < 0.0f || lightNdc.z > 1.0f)
    {
        return 1.0f;
    }

    float2 shadowUV = float2(lightNdc.x * 0.5f + 0.5f, 1.0f - (lightNdc.y * 0.5f + 0.5f));
    float receiverDepth = lightNdc.z;

    // シャドウアクネ対策のバイアス。斜入射(NdotLが小さい)ほどアクネが出やすいため傾斜に応じて大きくする
    const float kShadowBiasMin = 0.0005f;
    const float kShadowBiasMax = 0.0025f;
    const float bias = lerp(kShadowBiasMax, kShadowBiasMin, NdotL);
    const float compareDepth = receiverDepth - bias;

    // シャドウマップの1テクセル分のUVサイズ(KurenaiEngine3D::kShadowMapSizeと合わせる)
    const float kTexelSize = 1.0f / 2048.0f;
    const float lightSize = max(ShadowParams.x, kTexelSize);

    // --- (1) ブロッカーサーチ: lightSizeの範囲を5x5タップでサンプルし、受光点より光源側にある
    //     (=深度がより小さい)テクセルの平均深度を求める ---
    const int kBlockerTaps = 5;
    const int kBlockerHalf = kBlockerTaps / 2;
    float blockerDepthSum = 0.0f;
    int blockerCount = 0;

    [unroll]
    for (int by = -kBlockerHalf; by <= kBlockerHalf; ++by)
    {
        [unroll]
        for (int bx = -kBlockerHalf; bx <= kBlockerHalf; ++bx)
        {
            const float2 offset = float2(bx, by) * (lightSize / float(kBlockerTaps));
            const float sampleDepth = shadowMap.Sample(DefaultSampler, shadowUV + offset).r;
            if (sampleDepth < compareDepth)
            {
                blockerDepthSum += sampleDepth;
                blockerCount += 1;
            }
        }
    }

    if (blockerCount == 0)
    {
        return 1.0f;
    }

    const float avgBlockerDepth = blockerDepthSum / float(blockerCount);

    // --- (2) 半影サイズの推定 ---
    const float penumbraRatio = (receiverDepth - avgBlockerDepth) / max(avgBlockerDepth, 1e-5f);
    const float filterRadius = clamp(penumbraRatio * lightSize, kTexelSize, lightSize);

    // --- (3) 推定した半径でPCF(5x5タップの深度比較平均) ---
    const int kPCFTaps = 5;
    const int kPCFHalf = kPCFTaps / 2;
    float shadowSum = 0.0f;

    [unroll]
    for (int py = -kPCFHalf; py <= kPCFHalf; ++py)
    {
        [unroll]
        for (int px = -kPCFHalf; px <= kPCFHalf; ++px)
        {
            const float2 offset = float2(px, py) * (filterRadius / float(kPCFTaps));
            const float sampleDepth = shadowMap.Sample(DefaultSampler, shadowUV + offset).r;
            shadowSum += (sampleDepth < compareDepth) ? 0.0f : 1.0f;
        }
    }

    return shadowSum / float(kPCFTaps * kPCFTaps);
}

// ピクセルのView空間深度からカスケード番号(0=カメラに近い方)を選び、対応するシャドウマップで
// ComputeShadowFactorを呼ぶ。HLSLはリソース(Texture2D)を動的添字の配列として扱えないため、
// 分岐でカスケードごとのテクスチャを選択する
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

// Cook-Torrance を1灯ぶん評価する(シャドウ・ライト色・減衰は呼び出し側で乗算する)。
// 太陽(b0)とポイント/スポットライト(t8)の両方から共通で呼ばれる
float3 EvaluateDirectBRDF(float3 N, float3 V, float3 L, float NdotV, float3 albedo, float metallic, float roughness)
{
    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float D = DistributionGGX(NdotH, roughness);
    float G = GeometrySmith(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(VdotH, F0);

    float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, 1e-4f);
    float3 kd = (1.0f - F) * (1.0f - metallic);
    float3 diffuse = kd * albedo / PI;

    return (diffuse + specular) * NdotL;
}

// Karis 2013 / Frostbite の windowed inverse-square。Range を超えると厳密に0になり、
// 打ち切り境界でのハードエッジが出ない
float DistanceAttenuation(float distSq, float range)
{
    float factor = distSq / max(range * range, 1e-4f); // (d/r)^2
    float window = saturate(1.0f - factor * factor);   // 1 - (d/r)^4
    // 光源に極端に近づいたときの発散を抑える。定数1.0を足す実装はシーンスケール依存になるため、
    // 最小距離二乗でのクランプにする
    return (window * window) / max(distSq, 0.0001f);
}

// Frostbite の lightAngleScale / lightAngleOffset。CPU側(MakeGPULight)で事前計算した値を
// GPULight.DirectionAngle.w / Params.x として受け取る
//   scale  = 1 / max(0.001, cos(inner) - cos(outer))
//   offset = -cos(outer) * scale
float SpotAttenuation(float3 spotDirection, float3 L, float angleScale, float angleOffset)
{
    float t = saturate(dot(spotDirection, -L) * angleScale + angleOffset);
    return t * t;
}

// t8のライトリストを1灯ぶん評価する(影なし)。early-outは効きの強い順(距離→減衰→スポット円錐→NdotL)に並べる
float3 EvaluateLight(GPULight light, float3 worldPos, float3 N, float3 V, float NdotV, float3 albedo, float metallic, float roughness)
{
    uint lightType = (uint)light.PositionType.w;
    float range = light.ColorRange.w;

    float3 L;
    float atten = 1.0f;

    if (lightType == 0u) // Directional
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

        if (lightType == 2u) // Spot
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

    return EvaluateDirectBRDF(N, V, L, NdotV, albedo, metallic, roughness) * light.ColorRange.rgb * atten;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float depth = DepthTexture.Sample(DefaultSampler, input.UV).r;
    if (depth <= 0.0f)
    {
        // 背景(スカイ)には直接光はない(スカイボックス自体はDeferredLightingパス側で表示する)
        // Reverse-Zのため遠平面(=背景)はNDC z=0.0付近になる
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float3 worldPos = ReconstructWorldPos(input.UV, depth);
    float3 albedo = AlbedoTexture.Sample(DefaultSampler, input.UV).rgb;
    float3 N = OctDecode(NormalTexture.Sample(DefaultSampler, input.UV).xy);
    float2 material = MaterialTexture.Sample(DefaultSampler, input.UV).rg;
    float metallic = material.r;
    float roughness = material.g;

    float3 V = normalize(CameraPosition.xyz - worldPos);
    float NdotV = saturate(dot(N, V)) + 1e-5f;

    float3 directLight = float3(0.0f, 0.0f, 0.0f);

    // --- 太陽(b0、カスケードシャドウ付き) ---
    // ここでNdotL<=0のとき関数全体を打ち切ってはいけない(以前の実装の落とし穴)。
    // 太陽の寄与だけをこのブロックに閉じ込め、その後は必ずライトリストのループへ進む
    float3 sunL = normalize(-LightDirection.xyz);
    float sunNdotL = saturate(dot(N, sunL));
    if (sunNdotL > 0.0f)
    {
        float viewDepth = mul(float4(worldPos, 1.0f), View).z;
        float shadow = ComputeCascadedShadowFactor(worldPos, viewDepth, sunNdotL);
        directLight += EvaluateDirectBRDF(N, V, sunL, NdotV, albedo, metallic, roughness) * LightColor.rgb * shadow;
    }

    // --- t8のライトリスト(影なし) ---
    [loop]
    for (uint i = 0; i < LightCount.x; ++i)
    {
        directLight += EvaluateLight(Lights[i], worldPos, N, V, NdotV, albedo, metallic, roughness);
    }

    return float4(directLight, 1.0f);
}
