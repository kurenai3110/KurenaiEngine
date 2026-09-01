// MegaLights のシェード。リザーバに残った1灯へ影レイを撃ち、選ばれた確率で割り戻して
// 直接光をHDRで書き出す。灯を選ぶところは MegaLightsInitialSample.hlsl(と、その後ろに入る再利用)。
//
// 【なぜ選ぶ側と分けてあるのか】時間・空間の再利用は「どの灯を選んだか」を持ち回って
// 現フレームで評価し直す形でしか書けない。選択とシェードが1パスに混ざっていると、
// 再利用の段を差し込む場所が無い。分けておけば Initial と Shade の間に挟むだけで済む。
//
// 【影レイはここでだけ撃つ】選択の段では遮蔽を見ない(見るにはレイが要り、
// 「レイを1本に抑える」という手法の目的が崩れる)。したがって影は最後に1本だけ。
//
// DX12 かつ DXR Tier 1.1 のときだけ生成される(RayQuery は SM 6.5 の機能)。
#include "NormalEncoding.hlsli"
#include "SpecularEnergy.hlsli"

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
    // w にスペキュラのエネルギー補正のモードが入っている
    float4 ShadowParams;
    // 【宣言はここで止めている】読むのは ShadowParams まで。途中を飛ばして末尾だけを
    // 宣言すると誤ったオフセットを読み、コンパイルは通り絵も「それらしく」出るため気付けない
};

cbuffer MegaLightsStochasticConstants : register(b1)
{
    // x=出力幅, y=出力高, z=初期候補数M(このパスでは未使用), w=影レイを撃つか(0で撃たない)
    uint4 Params0;
    // x=タイル数X, y=タイルの1辺のピクセル数, z=1タイルあたりの候補数K, w=フレーム番号
    // (このパスでは未使用。b1を2パスで共有しているため並びは合わせてある)
    uint4 Params1;
};

RaytracingAccelerationStructure SceneTLAS : register(t0);

Texture2D NormalTexture : register(t1);
Texture2D DepthTexture : register(t2);
Texture2D AlbedoTexture : register(t3);
Texture2D MaterialTexture : register(t4);
Texture2D BRDFLUTTexture : register(t5);

#define KURENAI_PUNCTUAL_LIGHT_REGISTER t6
#define KURENAI_PUNCTUAL_LIGHTING_BRDF
#include "PunctualLighting.hlsli"
#include "MegaLightsCommon.hlsli"

StructuredBuffer<MegaLightsReservoir> Reservoirs : register(t7);

RWTexture2D<float4> MegaLightsOutput : register(u0);

static const float kRayOriginBias = 0.01f;
static const float kRayOriginBiasSlope = 1e-4f;
static const float kMinSlopeScaleNdotL = 0.1f;

float3 ReconstructWorldPos(float2 uv, float depth)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 worldPos = mul(float4(ndc, depth, 1.0f), InvViewProj);
    return worldPos.xyz / worldPos.w;
}

float TraceLightVisibility(float3 rayOrigin, float3 L, float originBias, float distanceToLight)
{
    RayDesc ray;
    ray.Origin = rayOrigin;
    ray.Direction = L;
    ray.TMin = originBias;
    // 【光源までの距離で打ち切る】太陽と違い、これを省くと光源の向こう側のジオメトリが
    // 遮蔽物になり、壁際・天井際のライトが常に真っ暗になる
    ray.TMax = max(distanceToLight - originBias, originBias);

    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(SceneTLAS, RAY_FLAG_NONE, 0xFFu, ray);
    query.Proceed();

    return (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0f : 1.0f;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    const uint2 outputSize = Params0.xy;
    if (pixel.x >= outputSize.x || pixel.y >= outputSize.y)
    {
        return;
    }

    const uint reservoirIndex = pixel.y * outputSize.x + pixel.x;
    const MegaLightsReservoir reservoir = Reservoirs[reservoirIndex];

    // 【背景も空のリザーバも必ず書くこと】RHIにUAVのクリアが無く、書かずにreturnすると
    // 前フレームの残骸が残る
    if (MegaLightsReservoirIsEmpty(reservoir))
    {
        MegaLightsOutput[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float2 uv = (float2(pixel) + 0.5f) / float2(outputSize);
    const float depth = DepthTexture.SampleLevel(DataSampler, uv, 0).r;
    if (depth <= 0.0f)
    {
        MegaLightsOutput[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float3 worldPos = ReconstructWorldPos(uv, depth);
    const float4 albedoSample = AlbedoTexture.SampleLevel(ColorSampler, uv, 0);
    const float3 albedo = albedoSample.rgb;
    const float translucency = albedoSample.a;
    const float3 N = OctDecode(NormalTexture.SampleLevel(DataSampler, uv, 0).xy);
    const float2 material = MaterialTexture.SampleLevel(DataSampler, uv, 0).rg;
    const float metallic = material.r;
    const float roughness = material.g;

    const float3 V = normalize(CameraPosition.xyz - worldPos);
    const float NdotV = saturate(dot(N, V)) + 1e-5f;

    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    const float3 brdf = BRDFLUTTexture.SampleLevel(ColorSampler, float2(NdotV, roughness), 0).rgb;
    const SpecularEnergyContext energy = MakeSpecularEnergyContext(F0, brdf, roughness, ShadowParams.w);

    // 【幾何は保存せず引き直す】リザーバに入れて持ち回ることもできるが、再利用で
    // 別の画素から来たサンプルは、その画素の位置で評価し直さなければ意味が無い。
    // 常に「いまの画素で引き直す」形にしておけば、再利用が入っても同じコードで済む
    const uint lightIndex = MegaLightsUnpackLight(reservoir.LightAndFlags);
    const GPULight light = Lights[lightIndex];
    const PunctualGeometry geometry = EvaluatePunctualGeometry(light, worldPos, N, translucency);
    if (!geometry.Contributes)
    {
        MegaLightsOutput[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float shadow = 1.0f;
    if (Params0.w != 0u && light.Params.y > 0.5f)
    {
        const float slopeScale = 1.0f / max(dot(N, geometry.L), kMinSlopeScaleNdotL);
        const float originBias =
            (kRayOriginBias + length(worldPos - CameraPosition.xyz) * kRayOriginBiasSlope) * slopeScale;
        shadow = TraceLightVisibility(
            worldPos + N * originBias, geometry.L, originBias, geometry.Distance);
    }

    const float3 contribution = EvaluatePunctualContribution(
        light, geometry, N, V, NdotV, albedo, metallic, roughness, translucency, energy, shadow);

    MegaLightsOutput[pixel] = float4(contribution * reservoir.W, 1.0f);
}
