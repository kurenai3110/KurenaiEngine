// MegaLights の確率的サンプリング(初期RIS + 可視レイ + シェード)。
// ピクセルごとに候補プールから M 個を引いて寄与の大きさで重み付け直し(RIS)、
// 選ばれた1灯にだけ影レイを撃って、選ばれた確率で割り戻す。
//
// 【なぜこれで全灯評価と同じ答えになるのか】RIS は「粗い提案分布 p で M 個引き、
// 目標関数 p̂ に比例する重みで1つ選び、最後に 1/p̂ と Σw/M を掛け戻す」形の推定量で、
// **期待値が Σ_i f_i(全灯の合計)に一致する**。ノイズは乗るが偏りは無い、というのが要点で、
// 段階2の検証はまさにこれ ―― N枚平均が参照実装(MegaLightsReference.hlsl)へ
// 1/√N で寄っていくか、頭打ちになったらその値がバイアス、を測る。
//
// 【提案分布 p はどこから来るか】候補プール(MegaLightsTilePool.hlsl)がタイルごとに
// K 個のスロットを持ち、各スロットは p_i = w_i / SumW から**独立同分布に**引かれている。
// したがってスロットを一様に1つ選べば、それは p からの1サンプルになる。
// プールは w_i と SumW を別々に保存しているので、p_i はその場で厳密に再現できる。
//
// 【1フレームだけ見ると偏る】プールの K スロットは1タイル内の全ピクセルで共有され、
// 届いているのにどのスロットにも入らなかった灯はそのフレームでは絶対に選ばれない。
// これは**プールの実現値で条件付けたときの偏り**で、プールの種にフレーム番号を混ぜて
// 毎フレーム引き直しているため、時間方向の平均では消える。
// 逆に言うと、**フレーム番号を混ぜるのをやめると偏ったまま収束しなくなる**。
//
// 【1灯ぶんの式は共有する】幾何・early-out・寄与は PunctualLighting.hlsli の共有定義を使う。
// 参照実装と「どの灯が寄与0とみなされるか」がずれると定義域が変わり、期待値が一致しなくなる。
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
    // 【宣言はここで止めている】読むのは ShadowParams まで。途中を飛ばして末尾だけを宣言すると
    // 誤ったオフセットを読み、コンパイルは通り絵も「それらしく」出るため気付けない
};

cbuffer MegaLightsStochasticConstants : register(b1)
{
    // x=出力幅, y=出力高, z=1ピクセルあたりの初期候補数M, w=影レイを撃つか(0で撃たない)
    uint4 Params0;
    // x=タイル数X, y=タイルの1辺のピクセル数, z=1タイルあたりの候補数K, w=フレーム番号
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

// 候補プール。レイアウトは MegaLightsTilePool.hlsl 冒頭を参照
StructuredBuffer<uint> TilePool : register(t7);

RWTexture2D<float4> MegaLightsOutput : register(u0);

static const float kRayOriginBias = 0.01f;
static const float kRayOriginBiasSlope = 1e-4f;
static const float kMinSlopeScaleNdotL = 0.1f;
static const uint kInvalidLightIndex = 0xFFFFFFFFu;

float3 ReconstructWorldPos(float2 uv, float depth)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 worldPos = mul(float4(ndc, depth, 1.0f), InvViewProj);
    return worldPos.xyz / worldPos.w;
}

uint HashUint(uint x)
{
    x ^= x >> 17;
    x *= 0xed5ad4bbu;
    x ^= x >> 11;
    x *= 0xac4c1b51u;
    x ^= x >> 15;
    x *= 0x31848babu;
    x ^= x >> 14;
    return x;
}

// 呼ぶたびに状態を進める乱数。RIS はスロットの抽選と採用判定で2回引くので、
// 同じ種から独立した値を順に取り出せる形にしておく
float NextRandom(inout uint state)
{
    state = HashUint(state);
    return float(state) * 2.3283064365e-10f; // uintの最大値で割って[0,1)へ
}

float Luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
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

    const float2 uv = (float2(pixel) + 0.5f) / float2(outputSize);
    const float depth = DepthTexture.SampleLevel(DataSampler, uv, 0).r;
    if (depth <= 0.0f)
    {
        // 背景。【必ず書くこと】RHIにUAVのクリアが無く、書かずにreturnすると前フレームの残骸が残る
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

    // --- このピクセルが属するタイルの候補プールを引く ---
    const uint tileSize = max(Params1.y, 1u);
    const uint2 tileCoord = pixel / tileSize;
    const uint candidateCount = Params1.z;
    const uint tileBase = (tileCoord.y * Params1.x + tileCoord.x) * (4u + 2u * candidateCount);

    const float sumW = asfloat(TilePool[tileBase + 0u]);
    const uint validCandidates = TilePool[tileBase + 2u];
    if (sumW <= 0.0f || validCandidates == 0u)
    {
        // このタイルへ届く灯が無い。背景と同じく必ず書く
        MegaLightsOutput[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // --- RIS: 候補プールから M 個引いて、寄与の大きさに比例する重みで1つ残す ---
    const uint sampleCount = max(Params0.z, 1u);
    uint rngState = HashUint(pixel.x + pixel.y * outputSize.x + Params1.w * 0x9E3779B9u);

    float risWeightSum = 0.0f;
    uint selectedLightIndex = kInvalidLightIndex;
    float selectedTargetPdf = 0.0f;
    PunctualGeometry selectedGeometry = (PunctualGeometry)0;

    [loop]
    for (uint m = 0u; m < sampleCount; ++m)
    {
        // プールのスロットを一様に1つ引く。スロットは p_i = w_i / SumW から独立同分布に
        // 引かれているので、これは提案分布 p からの1サンプルになる
        const uint slot = min((uint)(NextRandom(rngState) * float(validCandidates)), validCandidates - 1u);
        const uint lightIndex = TilePool[tileBase + 4u + 2u * slot + 0u];
        const float candidateWeight = asfloat(TilePool[tileBase + 4u + 2u * slot + 1u]);
        // 採用判定の乱数は、候補が無効でも必ず引いて状態を進める
        // (引く回数がループの中身で変わると、ピクセルごとに乱数列の位相がずれる)
        const float acceptRandom = NextRandom(rngState);

        if (lightIndex == kInvalidLightIndex || candidateWeight <= 0.0f)
        {
            continue;
        }

        const GPULight light = Lights[lightIndex];
        const PunctualGeometry geometry = EvaluatePunctualGeometry(light, worldPos, N, translucency);
        if (!geometry.Contributes)
        {
            // 寄与0の灯は目標関数も0になるのでRISの重みも0。加算する必要が無い
            continue;
        }

        // 目標関数。遮蔽は含めない(含めるにはレイを撃つことになり、RISの意味が無くなる)
        const float3 unshadowed = EvaluatePunctualContribution(
            light, geometry, N, V, NdotV, albedo, metallic, roughness, translucency, energy, 1.0f);
        const float targetPdf = Luminance(unshadowed);
        if (targetPdf <= 0.0f)
        {
            continue;
        }

        // 提案分布の確率密度。候補プールが w_i と SumW を別々に持っているので厳密に再現できる
        const float sourcePdf = candidateWeight / sumW;
        const float risWeight = targetPdf / sourcePdf;

        risWeightSum += risWeight;
        // 重み付きリザーバサンプリング(1個)
        if (acceptRandom < risWeight / risWeightSum)
        {
            selectedLightIndex = lightIndex;
            selectedTargetPdf = targetPdf;
            selectedGeometry = geometry;
        }
    }

    // 【0除算のガードは必須】どの候補も寄与しないピクセル(屋外の大半)でここを割ると
    // NaN が出て、直接光→SceneColor→TAAの履歴まで壊れて復帰しなくなる
    if (selectedLightIndex == kInvalidLightIndex || selectedTargetPdf <= 0.0f || risWeightSum <= 0.0f)
    {
        MegaLightsOutput[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // 不偏寄与重み W = (1/p̂(y)) * (1/M) * Σw
    const float contributionWeight = risWeightSum / (float(sampleCount) * selectedTargetPdf);

    // --- 選ばれた1灯にだけ影レイを撃つ ---
    const GPULight selectedLight = Lights[selectedLightIndex];
    float shadow = 1.0f;
    if (Params0.w != 0u && selectedLight.Params.y > 0.5f)
    {
        const float slopeScale = 1.0f / max(dot(N, selectedGeometry.L), kMinSlopeScaleNdotL);
        const float originBias =
            (kRayOriginBias + length(worldPos - CameraPosition.xyz) * kRayOriginBiasSlope) * slopeScale;
        shadow = TraceLightVisibility(
            worldPos + N * originBias, selectedGeometry.L, originBias, selectedGeometry.Distance);
    }

    const float3 contribution = EvaluatePunctualContribution(
        selectedLight, selectedGeometry, N, V, NdotV, albedo, metallic, roughness, translucency, energy, shadow);

    MegaLightsOutput[pixel] = float4(contribution * contributionWeight, 1.0f);
}
