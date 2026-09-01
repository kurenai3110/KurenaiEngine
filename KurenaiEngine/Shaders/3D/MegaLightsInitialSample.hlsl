// MegaLights の初期サンプリング。候補プールから M 個引いて RIS で1灯へ絞り、
// 結果を**リザーバとして**書き出す(色は作らない。シェードは MegaLightsShade.hlsl)。
//
// 【なぜ色ではなくリザーバを書くのか】時間・空間の再利用は「どの灯を選んだか」を
// 持ち回って現フレームで評価し直す形で行う。色を持ち回ると、遮蔽物が動いたときに
// 古い明るさが残り続ける。分けておけば、再利用の段が Initial と Shade の間に入るだけで済む。
//
// 【なぜこれで全灯評価と同じ答えになるのか】RIS は「粗い提案分布 p で M 個引き、
// 目標関数 p̂ に比例する重みで1つ選び、最後に 1/p̂ と Σw/M を掛け戻す」形の推定量で、
// 期待値が Σ_i f_i(全灯の合計)に一致する。ノイズは乗るが偏りは無い。
//
// 【提案分布 p はどこから来るか】候補プール(MegaLightsTilePool.hlsl)がタイルごとに
// K 個のスロットを持ち、各スロットは p_i = w_i / SumW から独立同分布に引かれている。
// スロットを一様に1つ選べば、それは p からの1サンプルになる。
//
// 【1フレームだけ見ると偏る】プールの K スロットは1タイル内の全ピクセルで共有され、
// 届いているのにどのスロットにも入らなかった灯はそのフレームでは選ばれない。
// プールの種にフレーム番号を混ぜて毎フレーム引き直しているため時間平均では消えるが、
// **混ぜるのをやめると偏ったまま収束しなくなる。**
//
// 【初期可視レイ】選び終わったサンプルへ影レイを1本撃ち、遮蔽されていたら W=0 で殺す。
// これはRTXDI系の標準の段で、**再利用を入れるなら必須**。
// 遮蔽されたサンプルを残したまま近傍へ配ると、「そこでは真っ黒になる灯」が周囲へ拡散し、
// 再利用が品質を悪化させる。実測でも、これを入れずに空間再利用を足したときは
// 球の |相対誤差| 中央値が 0.0309 → 0.0663 と倍以上に悪化した。
//
// レイは1本だけで、シェード側の影レイと合わせて1画素あたり2本になる。
// 【影を二重に掛けてはいけない】ここは「サンプルを殺す」だけで、影の階調は
// シェード側の1本が決める(殺されたサンプルはそもそもシェードへ渡らない)。
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
    // x=出力幅, y=出力高, z=1ピクセルあたりの初期候補数M, w=影レイを撃つか(このパスでは未使用)
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
#include "MegaLightsCommon.hlsli"

// 候補プール。レイアウトは MegaLightsTilePool.hlsl 冒頭を参照
StructuredBuffer<uint> TilePool : register(t7);

RWStructuredBuffer<MegaLightsReservoir> Reservoirs : register(u0);

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

// 呼ぶたびに状態を進める乱数。RIS はスロットの抽選と採用判定で2回引く
float NextRandom(inout uint state)
{
    state = HashUint(state);
    return float(state) * 2.3283064365e-10f; // uintの最大値で割って[0,1)へ
}

float Luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

static const float kRayOriginBias = 0.01f;
static const float kRayOriginBiasSlope = 1e-4f;
static const float kMinSlopeScaleNdotL = 0.1f;

float TraceLightVisibility(float3 rayOrigin, float3 L, float originBias, float distanceToLight)
{
    RayDesc ray;
    ray.Origin = rayOrigin;
    ray.Direction = L;
    ray.TMin = originBias;
    // 光源までの距離で打ち切る(省くと光源の向こう側のジオメトリが遮蔽物になる)
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

    const float2 uv = (float2(pixel) + 0.5f) / float2(outputSize);
    const float depth = DepthTexture.SampleLevel(DataSampler, uv, 0).r;
    if (depth <= 0.0f)
    {
        // 背景。【必ず書くこと】RHIにバッファのクリアが無く、書かずにreturnすると
        // 前フレームの残骸が残り、シェード側が存在しないサンプルを引く
        Reservoirs[reservoirIndex] = MegaLightsMakeEmptyReservoir();
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
    const uint tileBase = MegaLightsTilePoolBase(tileCoord, Params1.x, candidateCount);

    const float sumW = asfloat(TilePool[tileBase + 0u]);
    const uint validCandidates = TilePool[tileBase + 2u];
    if (sumW <= 0.0f || validCandidates == 0u)
    {
        Reservoirs[reservoirIndex] = MegaLightsMakeEmptyReservoir();
        return;
    }

    // --- RIS: 候補プールから M 個引いて、寄与の大きさに比例する重みで1つ残す ---
    const uint sampleCount = max(Params0.z, 1u);
    uint rngState = HashUint(pixel.x + pixel.y * outputSize.x + Params1.w * 0x9E3779B9u);

    float risWeightSum = 0.0f;
    uint selectedLightIndex = 0xFFFFFFFFu;
    float selectedTargetPdf = 0.0f;

    [loop]
    for (uint m = 0u; m < sampleCount; ++m)
    {
        const uint slot = min((uint)(NextRandom(rngState) * float(validCandidates)), validCandidates - 1u);
        const uint lightIndex = TilePool[tileBase + kMegaLightsTilePoolHeader + 2u * slot + 0u];
        const float candidateWeight = asfloat(TilePool[tileBase + kMegaLightsTilePoolHeader + 2u * slot + 1u]);
        // 採用判定の乱数は候補が無効でも必ず引いて状態を進める
        // (引く回数がループの中身で変わると、ピクセルごとに乱数列の位相がずれる)
        const float acceptRandom = NextRandom(rngState);

        if (lightIndex == 0xFFFFFFFFu || candidateWeight <= 0.0f)
        {
            continue;
        }

        const GPULight light = Lights[lightIndex];
        const PunctualGeometry geometry = EvaluatePunctualGeometry(light, worldPos, N, translucency);
        if (!geometry.Contributes)
        {
            continue;
        }

        // 目標関数。遮蔽は含めない(含めるにはレイを撃つことになりRISの意味が無くなる)
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
        if (acceptRandom < risWeight / risWeightSum)
        {
            selectedLightIndex = lightIndex;
            selectedTargetPdf = targetPdf;
        }
    }

    // 【0除算のガードは必須】どの候補も寄与しないピクセルでここを割るとNaNが出て、
    // 直接光→SceneColor→TAAの履歴まで壊れて復帰しなくなる
    if (selectedLightIndex == 0xFFFFFFFFu || selectedTargetPdf <= 0.0f || risWeightSum <= 0.0f)
    {
        Reservoirs[reservoirIndex] = MegaLightsMakeEmptyReservoir();
        return;
    }

    // --- 初期可視レイ: 遮蔽されていたらここで殺す ---
    // 再利用を入れるなら必須。遮蔽されたサンプルを近傍へ配ると、
    // 「そこでは真っ黒になる灯」が周囲へ拡散して品質を悪化させる
    bool visible = true;
    if (Params0.w != 0u)
    {
        const GPULight selectedLight = Lights[selectedLightIndex];
        if (selectedLight.Params.y > 0.5f)
        {
            const PunctualGeometry geometry =
                EvaluatePunctualGeometry(selectedLight, worldPos, N, translucency);
            if (geometry.Contributes)
            {
                const float slopeScale = 1.0f / max(dot(N, geometry.L), kMinSlopeScaleNdotL);
                const float originBias =
                    (kRayOriginBias + length(worldPos - CameraPosition.xyz) * kRayOriginBiasSlope) * slopeScale;
                visible = TraceLightVisibility(
                              worldPos + N * originBias, geometry.L, originBias, geometry.Distance) > 0.0f;
            }
        }
    }

    if (!visible)
    {
        // 【空にする ―― 重みを0にするだけでは足りない】W=0のリザーバは結合で
        // 選ばれなくなるが、M(confidence)は数えられる。それでよい:
        // 「M個の候補を見て、遮蔽で寄与0だった」という情報は正しい
        MegaLightsReservoir killed = MegaLightsMakeEmptyReservoir();
        killed.M = float(sampleCount);
        Reservoirs[reservoirIndex] = killed;
        return;
    }

    MegaLightsReservoir reservoir;
    // ライト番号は16bitへ詰める(kMaxLights = 1024 なので収まる)
    reservoir.LightAndFlags = MegaLightsPackLightAndFlags(selectedLightIndex, true);
    reservoir.SampleUV = 0u;
    // 不偏寄与重み W = (1/p̂(y)) * (1/M) * Σw
    reservoir.W = risWeightSum / (float(sampleCount) * selectedTargetPdf);
    reservoir.M = float(sampleCount);
    Reservoirs[reservoirIndex] = reservoir;
}
