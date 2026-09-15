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
// 【初期可視レイは空間再利用と組で使う(どちらも既定で有効)】選んだサンプルへ影レイを
// 1本撃ち、遮蔽されていたらリザーバごと殺す(RTXDI系では標準の段)。
// 殺しの意味は「遮蔽で0になるサンプルを近傍へ配らない」ことなので、
// **空間再利用が無いと絵が1bitも変わらない**(殺されるサンプルはシェード側のレイでも
// どうせ0)。逆に空間再利用は殺しが無いと実測でほぼ効かない。
// 【殺すときはどの灯を殺したかをリザーバへ残す】殺された画素のストリームは
// 「可視な灯しか配れない」形に変わるため、空間再利用の不偏化の分母(Z)は可視性まで
// 含めて数える必要がある。番号を残せばZ側で確定情報として使え、不明な近傍にだけ
// バイアス補正レイを撃てば済む(詳細は MegaLightsSpatial.hlsl。
// 残さない実装は -3.6% 暗く偏った。docs/ImplementationDetail.md 61.7f)。
// 【影を二重に掛けてはいけない】撃つ場合もここは「サンプルを殺す」だけで、影の階調は
// シェード側の1本が決める(殺されたサンプルはそもそもシェードへ渡らない)。
//
// DX12 かつ DXR Tier 1.1 のときだけ生成される(RayQuery は SM 6.5 の機能)。
#include "NormalEncoding.hlsli"
#include "SpecularEnergy.hlsli"

#include "MathConstants.hlsli"

#include "ShaderInterop/FrameConstants.hlsli"

#include "ShaderInterop/MegaLightsStochasticConstants.hlsli"

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
// デノイザと同じ再投影に使うモーションベクターと、前フレームの幾何ガイド。
Texture2D VelocityTexture : register(t8);
StructuredBuffer<MegaLightsHistoryGuide> HistoryGuide : register(t9);

RWStructuredBuffer<MegaLightsReservoir> Reservoirs : register(u0);
// 画素ごとの「遮蔽が確定した灯」のキャッシュ(0xFFFFFFFFで無し)。
// 【なぜ持続させるのか】殺しの持ち回り(リザーバ)はそのフレームに殺しが起きた
// 画素にしか無い。初期RISが別の灯を引いたフレームには知識が消え、その隙に
// 空間再利用が遮蔽された支配光を近傍から借りて影レイを無駄にする ――
// 影の縁に暗い粒のフリンジが残る原因。キャッシュなら毎フレーム効く。
// 【新鮮さ】殺しで記録し、同じ灯が可視レイを通ったら即消す。支配的な灯は
// RISがほぼ毎フレーム引き直すので、遮蔽が解けた次のフレームには消える。
// 履歴が無効なフレーム(解像度変更直後など)は読まずに上書きだけする
RWStructuredBuffer<uint> BlockedLights : register(u1);
// ブースト標本の項と件数。ゲート外・背景・空タイルは全経路でゼロを書く。
RWTexture2D<float4> MegaLightsBoostTexture : register(u2);
RWStructuredBuffer<uint> MegaLightsBoostCount : register(u3);

#include "ShaderInterop/Common.hlsli"

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

// 1画素ぶんの標本すべてへ同じリザーバを書く(背景・候補なしの早期脱出用)。
// 【1本だけ書いて帰ってはいけない】RHIにバッファのクリアが無いので、
// 書かなかったスロットには前フレームの残骸が残り、Resolveがそれを平均に混ぜる
void WriteAllReservoirs(uint base, uint count, MegaLightsReservoir value)
{
    [loop]
    for (uint s = 0u; s < count; ++s)
    {
        Reservoirs[base + s] = value;
    }
}

// 1スロットぶんの RIS と初期可視レイを実行する。
// BlockedLights 自体は触らず、基底ループが読んだ値だけを受け取る。
MegaLightsReservoir DrawSample(
    uint sampleSlot, uint2 pixel, uint2 outputSize, uint tileBase, uint validCandidates,
    uint sampleCount, float sumW, uint reachableCount, uint blockedLight,
    float3 worldPos, float3 N, float3 V, float NdotV, float3 albedo,
    float metallic, float roughness, float translucency, SpecularEnergyContext energy,
    out uint selectedLightIndex, out bool visible)
{
    // --- RIS: 候補プールから M 個引いて、寄与の大きさに比例する重みで1つ残す ---
    // 【スロットの抽選だけ低食い違い量列にする】画素ごとの位相をブルーノイズ的に配り、
    // 同じ画素の中では M 個が均等に散るようにする。周辺分布は一様のままなので
    // 割り戻しも期待値も変わらない(MegaLightsCommon.hlsli の説明を参照)。
    // 採用判定は白色のまま ―― あちらは M 回の判定の独立性を使っている。
    // 標本番号を位相の次元として渡し、N本が同じ列を引かないようにする
    const float slotPhase = MegaLightsPixelPhase(pixel, Params1.w, sampleSlot);
    uint rngState = HashUint(pixel.x + pixel.y * outputSize.x + Params1.w * 0x9E3779B9u +
                             sampleSlot * 0xB5297A4Du);

    // --- クアッド層化(手法3。Params4.w) ---
    // 2x2クアッドの4画素へ候補スロットを1/4ずつ割り当て、クアッド全体で列挙させる。
    const bool quadStratify = (Params4.w != 0u);
    uint stratumBase = 0u;
    uint stratumCount = validCandidates;
    if (quadStratify && validCandidates >= 4u)
    {
        const uint2 quad = pixel >> 1u;
        const uint lane = (pixel.x & 1u) | ((pixel.y & 1u) << 1u);
        const uint rotation = HashUint(quad.x + quad.y * 0x9E3779B9u + Params1.w * 0x85EBCA6Bu) & 3u;
        const uint stratum = (lane + rotation + sampleSlot) & 3u;
        const uint width = validCandidates >> 2u;
        stratumBase = stratum * width;
        // 最後の層は端数を引き受け、候補の定義域を欠けさせない
        stratumCount = (stratum == 3u) ? (validCandidates - stratumBase) : width;
    }

    float risWeightSum = 0.0f;
    selectedLightIndex = 0xFFFFFFFFu;
    float selectedTargetPdf = 0.0f;
    visible = false;

    [loop]
    for (uint m = 0u; m < sampleCount; ++m)
    {
        const float slotRandom = MegaLightsLowDiscrepancy1D(m, slotPhase);
        const uint slot =
            stratumBase + min((uint)(slotRandom * float(stratumCount)), stratumCount - 1u);
        const uint lightIndex = TilePool[tileBase + kMegaLightsTilePoolHeader + 2u * slot + 0u];
        const float candidateWeight = asfloat(TilePool[tileBase + kMegaLightsTilePoolHeader + 2u * slot + 1u]);
        // 候補の中身にかかわらず採用判定の乱数を引き、画素ごとの列をずらさない
        const float acceptRandom = NextRandom(rngState);

        if (lightIndex == 0xFFFFFFFFu || candidateWeight <= 0.0f)
        {
            continue;
        }
        if (lightIndex == blockedLight)
        {
            continue;
        }
        const GPULight light = Lights[lightIndex];
        const PunctualGeometry geometry = EvaluatePunctualGeometry(light, worldPos, N, translucency);
        if (!geometry.Contributes)
        {
            continue;
        }

        // 目標関数。遮蔽は含めず、可視性は選択後の1本だけで求める
        const float3 unshadowed = EvaluatePunctualContribution(
            light, geometry, N, V, NdotV, albedo, metallic, roughness, translucency, energy, 1.0f);
        const float targetPdf = Luminance(unshadowed);
        if (targetPdf <= 0.0f)
        {
            continue;
        }

        const float sourcePdf = kMegaLightsUniformMixFraction / float(max(reachableCount, 1u)) +
                                (1.0f - kMegaLightsUniformMixFraction) * (candidateWeight / sumW);
        const float risWeight = targetPdf / sourcePdf;

        risWeightSum += risWeight;
        if (acceptRandom < risWeight / risWeightSum)
        {
            selectedLightIndex = lightIndex;
            selectedTargetPdf = targetPdf;
        }
    }

    if (selectedLightIndex == 0xFFFFFFFFu || selectedTargetPdf <= 0.0f || risWeightSum <= 0.0f)
    {
        MegaLightsReservoir rejected = MegaLightsMakeEmptyReservoir();
        rejected.M = float(sampleCount);
        selectedLightIndex = 0xFFFFFFFFu;
        return rejected;
    }

    // 球光源で狙う点は選択ループの外で必ず2次元ぶん引く
    const float2 sampleUV = float2(NextRandom(rngState), NextRandom(rngState));
    visible = true;
    if (Params0.w != 0u && Params2.w != 0u)
    {
        const GPULight selectedLight = Lights[selectedLightIndex];
        if (LightCastsRaytracedShadow(selectedLight.Params.y))
        {
            const PunctualGeometry geometry =
                EvaluatePunctualGeometry(selectedLight, worldPos, N, translucency);
            if (geometry.Contributes)
            {
                const float slopeScale = 1.0f / max(dot(N, geometry.L), kMinSlopeScaleNdotL);
                const float originBias =
                    (kRayOriginBias + length(worldPos - CameraPosition.xyz) * kRayOriginBiasSlope) * slopeScale;
                const float3 samplePos = MegaLightsLightSamplePosition(
                    selectedLight.PositionType.xyz, selectedLight.Params.z,
                    selectedLight.DirectionAngle.xyz, (uint)selectedLight.PositionType.w, sampleUV);
                const float3 toSample = samplePos - worldPos;
                const float sampleDist = length(toSample);
                if (sampleDist > originBias)
                {
                    visible = TraceLightVisibility(
                                  worldPos + N * originBias, toSample / sampleDist, originBias, sampleDist) > 0.0f;
                }
            }
        }
    }

    if (!visible)
    {
        MegaLightsReservoir killed = MegaLightsMakeEmptyReservoir();
        killed.IndexAndFlags = MegaLightsPackLightAndFlags(selectedLightIndex, false);
        killed.SampleUV = MegaLightsPackSampleUV(sampleUV);
        killed.M = float(sampleCount);
        return killed;
    }

    MegaLightsReservoir reservoir;
    reservoir.IndexAndFlags = MegaLightsPackLightAndFlags(selectedLightIndex, true);
    reservoir.SampleUV = MegaLightsPackSampleUV(sampleUV);
    reservoir.W = risWeightSum / (float(sampleCount) * selectedTargetPdf);
    reservoir.M = float(sampleCount);
    return reservoir;
}

// Initial からデノイザの履歴棄却を予測するため、同じ1タップ判定を使う。
bool MegaLightsInitialHistoryTapValid(
    int2 tapPixel, uint2 outputSize, float viewZ, float3 N, float2 material)
{
    const int2 clamped = clamp(tapPixel, int2(0, 0), int2(outputSize) - 1);
    float hViewZ;
    float3 hN;
    float2 hMaterial;
    bool hValid;
    if ((Params5.w & 2u) != 0u)
    {
        const MegaLightsHistoryGuide guide = HistoryGuide[clamped.y * outputSize.x + clamped.x];
        hViewZ = guide.ViewZ;
        hN = OctDecode(MegaLightsUnpackNormalOct(guide.NormalOct));
        MegaLightsUnpackMaterial(guide.Material, hMaterial.x, hMaterial.y);
        hValid = (guide.ViewZ != 0.0f);
    }
    else
    {
        const float2 tapUv = (float2(clamped) + 0.5f) / float2(outputSize);
        const float hDepth = DepthTexture.SampleLevel(DataSampler, tapUv, 0).r;
        const float3 hWorldPos = ReconstructWorldPos(tapUv, hDepth);
        hViewZ = (hDepth > 0.0f) ? mul(float4(hWorldPos, 1.0f), View).z : 0.0f;
        hN = OctDecode(NormalTexture.SampleLevel(DataSampler, tapUv, 0).xy);
        hMaterial = MaterialTexture.SampleLevel(DataSampler, tapUv, 0).rg;
        hValid = (hDepth > 0.0f);
    }
    return hValid && MegaLightsGuideMatchesSurface(hViewZ, hN, hMaterial, viewZ, N, material);
}

bool MegaLightsInitialPredictsDenoiseRejection(
    uint2 outputSize, float2 uv, float viewZ, float3 N, float2 material)
{
    if (Params5.y == 0u)
    {
        return false;
    }
    if (Params5.z == 3u)
    {
        return true;
    }
    // mode 2 の履歴長条件は Moments を Initial に束縛していないため、今回は mode 1 と同じ。
    if ((Params5.w & 1u) == 0u)
    {
        return true;
    }

    const float2 velocity = VelocityTexture.SampleLevel(DataSampler, uv, 0).rg;
    const float2 historyUv = uv - velocity;
    if (!all(historyUv >= 0.0f) || !all(historyUv <= 1.0f))
    {
        return true;
    }

    if ((Params5.w & 4u) != 0u)
    {
        const float2 historyPixelF = historyUv * float2(outputSize) - 0.5f;
        const float2 baseF = floor(historyPixelF);
        const float2 frac2 = historyPixelF - baseF;
        const int2 baseI = int2(baseF);
        const float tapWeights[4] = {
            (1.0f - frac2.x) * (1.0f - frac2.y),
            frac2.x * (1.0f - frac2.y),
            (1.0f - frac2.x) * frac2.y,
            frac2.x * frac2.y,
        };
        const int2 tapOffsets[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };
        [unroll]
        for (uint tap = 0u; tap < 4u; ++tap)
        {
            if (tapWeights[tap] > 0.0f &&
                MegaLightsInitialHistoryTapValid(baseI + tapOffsets[tap], outputSize, viewZ, N, material))
            {
                return false;
            }
        }
        return true;
    }

    const int2 historyPixel =
        clamp(int2(historyUv * float2(outputSize)), int2(0, 0), int2(outputSize) - 1);
    return !MegaLightsInitialHistoryTapValid(historyPixel, outputSize, viewZ, N, material);
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

    // 【リザーバは1画素にN本、遮蔽キャッシュは1画素に1つ】キャッシュは
    // 「この画素からこの灯は見えない」という画素の性質で、標本ごとには持たない
    const uint samplesPerPixel = max(Params5.x, 1u);
    const uint reservoirIndex = pixel.y * outputSize.x + pixel.x;
    const uint reservoirBase = reservoirIndex * samplesPerPixel;

    // RHIにUAVクリアが無いため、有効画素は背景や空タイルを含め必ず初期化する。
    MegaLightsBoostTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    MegaLightsBoostCount[reservoirIndex] = 0u;

    const float2 uv = (float2(pixel) + 0.5f) / float2(outputSize);
    const float depth = DepthTexture.SampleLevel(DataSampler, uv, 0).r;
    if (depth <= 0.0f)
    {
        // 背景。【必ず書くこと】RHIにバッファのクリアが無く、書かずにreturnすると
        // 前フレームの残骸が残り、シェード側が存在しないサンプルを引く
        WriteAllReservoirs(reservoirBase, samplesPerPixel, MegaLightsMakeEmptyReservoir());
        BlockedLights[reservoirIndex] = 0xFFFFFFFFu;
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
    const float viewZ = mul(float4(worldPos, 1.0f), View).z;

    // このフレームの乱数を1つも引く前に、速度・履歴ガイド・G-Bufferだけでゲートを確定する。
    const bool boostPixel = MegaLightsInitialPredictsDenoiseRejection(outputSize, uv, viewZ, N, material);

    const float3 V = normalize(CameraPosition.xyz - worldPos);
    const float NdotV = saturate(dot(N, V)) + 1e-5f;

    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    const float3 brdf = BRDFLUTTexture.SampleLevel(ColorSampler, float2(NdotV, roughness), 0).rgb;
    const SpecularEnergyContext energy = MakeSpecularEnergyContext(F0, brdf, roughness, ShadowParams.w);

    // --- このピクセルが属するタイルの候補プールを引く ---
    const uint tileSize = max(Params1.y, 1u);
    // 書き手の [tile*16-offset, tile*16-offset+16) と逆写像になる同じ格子規約
    const uint2 tileCoord = (pixel + Params6.xy) / tileSize;
    const uint candidateCount = Params1.z;
    const uint tileBase = MegaLightsTilePoolBase(tileCoord, Params1.x, candidateCount);

    const float sumW = asfloat(TilePool[tileBase + 0u]);
    // 混合抽出(一様枝 + 重み枝)の割り戻しに要る(MegaLightsTilePool.hlsl)
    const uint reachableCount = TilePool[tileBase + 1u];
    const uint validCandidates = TilePool[tileBase + 2u];
    const uint sampleCount = max(Params0.z, 1u);
    if (sumW <= 0.0f || validCandidates == 0u)
    {
        // 【空でも M は候補数を持たせる】M はこの画素が「何個の候補を検討したか」で、
        // 結果が空だったかどうかとは独立。ここを0にすると、空間再利用の分母
        // (confidenceSum と Z)から「引いたが外した」画素が消えて分母が過小になり、
        // **明るい側の系統誤差**になる(詳細は選択失敗側のコメント)
        MegaLightsReservoir empty = MegaLightsMakeEmptyReservoir();
        empty.M = float(sampleCount);
        WriteAllReservoirs(reservoirBase, samplesPerPixel, empty);
        // 何も分からなかったフレーム。キャッシュは維持(履歴が無効なら信用できないので消す)
        if (Params4.x == 0u)
        {
            BlockedLights[reservoirIndex] = 0xFFFFFFFFu;
        }
        return;
    }

    // --- 遮蔽が確定した灯を目標関数から外す(キャッシュ) ---
    // 影の縁では目標関数を支配する灯が自分からは遮蔽されていることがあり、RISは
    // 可視性を知らないのでその灯を毎フレーム選んでは殺される ―― デノイザ前の
    // 暗黒点の主因(実測で1フレームあたり点灯画素の3.6%)。BlockedLights は
    // その灯の遮蔽が可視レイで確定した画素にだけ入っているので、目標関数を0として
    // 扱えば抽選が「届く別の灯」へ向かう。
    // 【16フレームに1回だけ再検証を許す】除外し続けると殺しが起きなくなり、
    // 遮蔽が解けたことを知る機会(可視レイ)が失われる。位相は画素ごとにずらす。
    // 静止シーンではキャッシュは常に正しく、期待値は変わらない(目標関数の変更は
    // RISでは自由で、定義域の縮小は空間再利用の分母のレイ判定が正しく数える)。
    // 動的シーンでは解けた遮蔽に平均8フレームで気づく(その間はその灯を選ばない
    // だけで、他の灯の寄与は正しいまま)
    uint blockedLight = 0xFFFFFFFFu;
    if (Params4.x != 0u)
    {
        const bool retest = ((Params1.w + pixel.x * 3u + pixel.y * 7u) & 15u) == 0u;
        if (!retest)
        {
            blockedLight = BlockedLights[reservoirIndex];
        }
    }

    // --- 1画素あたり samplesPerPixel 本を独立に引く ---
    //
    // 【なぜ本数を増やせるようにしたか】クアッド共有は2x2の4本を平均するので、
    // 1画素1本でも実効的には4標本ある。それでも動いている間はノイズが目に見える。
    // 内訳は2つで、どちらも標本数を増やすことでしか減らない:
    //   1. どの灯を選ぶか(RISの分散)
    //   2. 選んだ灯の可視性(球光源の球面上の1点への1本。ここが大きい ――
    //      参照実装を1本と32本で比べると|相対誤差|のp90が0.37あった)
    // UE5の MegaLights も r.MegaLights.NumSamplesPerPixel を 2/4/16 から選ぶ形で、
    // **最小でも2**である。1本しか撃たないのはこちらの予算の決め方の問題だった。
    //
    // 【N本は互いに独立に引く ―― 層化はクアッドの中だけ】種も位相も標本番号で分ける。
    // 期待値は1本のときと同じで、平均の分散が 1/N になる。
    //
    // 【BlockedLights を触るのは0番の標本だけ】キャッシュは画素に1つしかないので、
    // N本が競って書くと「最後に書いた者勝ち」になり、どの標本の判断が残ったのか
    // 追えなくなる。0番の判断に固定しておけば N を変えても挙動が動かない
    [loop]
    for (uint sampleSlot = 0u; sampleSlot < samplesPerPixel; ++sampleSlot)
    {
        const bool ownsCache = (sampleSlot == 0u);
        uint selectedLightIndex;
        bool visible;
        const MegaLightsReservoir reservoir = DrawSample(
            sampleSlot, pixel, outputSize, tileBase, validCandidates, sampleCount, sumW, reachableCount,
            blockedLight, worldPos, N, V, NdotV, albedo, metallic, roughness, translucency, energy,
            selectedLightIndex, visible);

        if (selectedLightIndex == 0xFFFFFFFFu)
        {
            Reservoirs[reservoirBase + sampleSlot] = reservoir;
            if (ownsCache && Params4.x == 0u)
            {
                BlockedLights[reservoirIndex] = 0xFFFFFFFFu;
            }
            continue;
        }

        if (!visible)
        {
            // 殺された灯の番号はリザーバへ残す。キャッシュの所有者は基底のslot 0だけ。
            Reservoirs[reservoirBase + sampleSlot] = reservoir;
            if (ownsCache)
            {
                // 球光源の1点の遮蔽は灯全体の遮蔽を証明しないためキャッシュしない
                if (Lights[selectedLightIndex].Params.z <= 0.0f)
                {
                    BlockedLights[reservoirIndex] = selectedLightIndex;
                }
                else if (Params4.x == 0u)
                {
                    BlockedLights[reservoirIndex] = 0xFFFFFFFFu;
                }
            }
            continue;
        }

        // 可視だったキャッシュ対象の灯は遮蔽が解けたため消す。読み書きは基底側だけに置く。
        if (ownsCache && (Params4.x == 0u || BlockedLights[reservoirIndex] == selectedLightIndex))
        {
            BlockedLights[reservoirIndex] = 0xFFFFFFFFu;
        }
        Reservoirs[reservoirBase + sampleSlot] = reservoir;
    }

    if (boostPixel)
    {
        float3 boostSum = float3(0.0f, 0.0f, 0.0f);
        uint boostCount = 0u;
        const uint boostSamples = Params5.y;
        [loop]
        for (uint k = 0u; k < boostSamples; ++k)
        {
            uint selectedLightIndex;
            bool visible;
            // 基底N本と独立な列を使い、キャッシュは読まず書かず無効値を渡す。
            const MegaLightsReservoir boostReservoir = DrawSample(
                samplesPerPixel + k, pixel, outputSize, tileBase, validCandidates, sampleCount,
                sumW, reachableCount, 0xFFFFFFFFu, worldPos, N, V, NdotV, albedo, metallic,
                roughness, translucency, energy, selectedLightIndex, visible);

            // 分母は引いた回数で決まり、空・遮蔽・寄与0の標本も必ず数える。
            ++boostCount;
            float3 term = float3(0.0f, 0.0f, 0.0f);
            if (selectedLightIndex != 0xFFFFFFFFu && boostReservoir.W > 0.0f)
            {
                const GPULight light = Lights[selectedLightIndex];
                const PunctualGeometry geometry =
                    EvaluatePunctualGeometry(light, worldPos, N, translucency);
                if (geometry.Contributes)
                {
                    // Resolveの自面評価と同じ式。Vだけはこの画素で撃ったレイの結果を使う。
                    const float visibility = visible ? 1.0f : 0.0f;
                    term = EvaluatePunctualContribution(
                               light, geometry, N, V, NdotV, albedo, metallic, roughness,
                               translucency, energy, visibility) *
                           boostReservoir.W;
                }
            }
            boostSum += term;
        }

        // .a は輝度統計ではなく項数。真っ暗な項でも .a > 0 のゲート印を残す。
        MegaLightsBoostTexture[pixel] = float4(boostSum, (float)boostCount);
        MegaLightsBoostCount[reservoirIndex] = boostCount;
    }
}
