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
// タイル錐台の組み立て・AABB・候補プールの重み w_j(y)。確率的バイリニア参照が
// 「隣のタイルならその灯をどの確率で提案したか」を再計算するのに使う。GPULightを使うのでこの順
#include "TileLightCulling.hlsli"
#include "MegaLightsCommon.hlsli"

// 候補プール。レイアウトは MegaLightsTilePool.hlsl 冒頭を参照
StructuredBuffer<uint> TilePool : register(t7);

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

// --- 候補プールの確率的バイリニア参照(Params5.z) ---
//
// 【何を直しているか】候補プールは16x16タイルごとに毎フレームK灯を抽選する。抽選の当たり外れは
// **そのタイルの256画素すべてに共有される**ので、タイル粒度の乱数がそのままタイル形の
// 相関ノイズになる。実測ではデノイズ前のタイル誤差の時間相関が0.113(毎フレーム独立)なのに
// デノイズ後は0.985(凍りつく)で、a-trous 5段のカーネルのうちタイルを跨げるのは
// 幾何的上限0.859に対して実際には0.020しか生き残らない(98%がエッジ停止で殺される)。
// **タイル内の256画素が同じ誤差を共有している以上、タイル内をいくら平均しても独立標本は
// 1つも増えない** ―― 空間フィルタでは原理的に取れない。輝度のエッジ停止に床を入れて
// 跨ぎを開ける対処は「ぼけただけ」で棄却された。
//
// 【どう直すか(UE5 MegaLights と同じ)】画素ごとに、最も近い4タイルの中から
// バイリニアの確率で1つを選ぶ。タイル境界の硬い割り当てを画素ごとの乱数へ溶かす。
// 出典: SIGGRAPH 2025 Advances "MegaLights: Stochastic Direct Lighting" p.14
// (advances.realtimerendering.com/s2025/content/MegaLights_Stochastic_Direct_Lighting_2025.pdf)
//
// 【格子ジッターでは直らない ―― 実測済み】ジッターは境界の位置を毎フレーム動かすだけで、
// 「1画素は1タイルに属する」という割り当ては残る。相関の単位がタイルのままなので効かない。
//
// 【不偏性がここの肝 ―― 選んだタイルの q_i(y) で割ってはいけない】q_i(y) = 0 の灯
// (そのタイルへ届かない灯)が存在するので、選んだタイルで割ると定義域が欠けてバイアスになる。
// 混合分布 q̄(y) = Σ_j b_j q_j(y) で割ること。q̄ > 0 は「4つのうち1つでも届く」で保証され、
// **画素自身のタイルは必ず4つに含まれる**(バイリニアの定義から、しかも重み0.25以上)ので、
// その画素に寄与しうる灯は必ず q̄ > 0 になる。
struct MegaLightsPoolTile
{
    // 候補プールの先頭添字
    uint Base;
    // そのタイルに届く全灯の重みの合計
    float SumW;
    // 届いた灯の数(混合抽出の一様枝の割り戻しに使う)
    uint ReachableCount;
    // 有効な候補数(0 か K)。0 なら背景タイルで、そこからは1灯も引けない
    uint ValidCandidates;
    // バイリニア重み b_j。4つの和は1
    float BilinearWeight;
    // w_j(y) の再計算に要る。側面はタイル座標から、深度スラブはヘッダから作る
    TileFrustum Frustum;
    float3 AabbMin;
    float3 AabbMax;
};

// タイル1つぶんの文脈をヘッダから組み立てる。
// 【錐台は候補プールを書いたときと同じ画素範囲で作ること】格子オフセット(Params6.xy)を
// 落とすと定義域がずれ、q_j(y) が実際の抽出確率と食い違う
MegaLightsPoolTile MegaLightsLoadPoolTile(
    uint2 tileCoord, uint2 outputSize, uint tileSize, uint candidateCount, float bilinearWeight)
{
    MegaLightsPoolTile tile;
    tile.Base = MegaLightsTilePoolBase(tileCoord, Params1.x, candidateCount);
    tile.SumW = asfloat(TilePool[tile.Base + 0u]);
    tile.ReachableCount = TilePool[tile.Base + 1u];
    tile.ValidCandidates = TilePool[tile.Base + 2u];
    tile.BilinearWeight = bilinearWeight;

    // 深度スラブは候補プールがヘッダへ書いている(そのタイルを走査しないと分からないため)
    const float nearestViewZ = asfloat(TilePool[tile.Base + 4u]);
    const float farthestViewZ = asfloat(TilePool[tile.Base + 5u]);
    const int2 tilePixelOrigin = int2(tileCoord * tileSize) - int2(Params6.xy);
    tile.Frustum = MakeTileFrustumFromPixelOrigin(
        tilePixelOrigin, outputSize, Params3.x, Params3.y, nearestViewZ, farthestViewZ);
    TileViewSpaceAABBFromPixelOrigin(
        tilePixelOrigin, outputSize, Params3.x, Params3.y, nearestViewZ, farthestViewZ,
        tile.AabbMin, tile.AabbMax);
    return tile;
}

// そのタイルが灯 y を1スロットぶん提案する確率 q_j(y)。届かなければ 0。
// 【重みは TileLightCulling.hlsli の関数で再計算する】書き手(MegaLightsTilePool.hlsl)と
// 同じ1本を呼ぶこと。式を写すと、ずれた瞬間に静かにバイアスが乗る
float MegaLightsTileSourcePdf(MegaLightsPoolTile tile, GPULight light, float3 viewCenter, float radius)
{
    if (tile.ValidCandidates == 0u || tile.SumW <= 0.0f)
    {
        return 0.0f;
    }
    const float w = TileLightCandidateWeight(light, viewCenter, radius, tile.Frustum, tile.AabbMin, tile.AabbMax);
    if (w <= 0.0f)
    {
        return 0.0f;
    }
    return kMegaLightsUniformMixFraction / float(max(tile.ReachableCount, 1u)) +
           (1.0f - kMegaLightsUniformMixFraction) * (w / tile.SumW);
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

    // 【ここで見るのは自分のタイルだけ】確率的バイリニア参照でも、この画素に寄与しうる灯は
    // 必ず自分のタイルの定義域に入る(光源の境界球が自分のタイルの錐台+深度スラブに
    // 触れないなら、その画素は光源の影響半径の外にいる)。自分のタイルが空なら
    // どの隣のタイルを引いても寄与0の灯しか出てこない
    const float sumW = asfloat(TilePool[tileBase + 0u]);
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

    // --- 参照する候補プールを決める(確率的バイリニア参照。既定は従来どおり自分のタイル固定) ---
    // 0=自分のタイル固定、1=2x2クアッドごとに1タイル、2=画素ごとに1タイル
    const uint bilinearMode = Params5.z;
    MegaLightsPoolTile poolTiles[4];
    uint poolTileCount = 1u;
    // 丸めで抽選が最後まで超えなかったときに落とす先。自分のタイルは重みが必ず0.25以上ある
    uint ownTileSlot = 0u;
    if (bilinearMode == 0u)
    {
        poolTiles[0] = MegaLightsLoadPoolTile(tileCoord, outputSize, tileSize, candidateCount, 1.0f);
        // 【ここを埋めないと未初期化の要素を読むことがある】使うのは [0] だけだが、
        // HLSL は配列の部分初期化を検出しない
        poolTiles[1] = poolTiles[0];
        poolTiles[2] = poolTiles[0];
        poolTiles[3] = poolTiles[0];
    }
    else
    {
        // 画素中心を「タイル中心の格子」へ移す。タイル t の中心は画素 t*S - offset + S/2 なので、
        // g = (pixel + 0.5 + offset)/S - 0.5 とすれば g の整数部が左上タイル、小数部が補間係数になる。
        // 【格子ジッターと同じオフセット済みの座標で作ること】書き手と読み手で格子がずれる
        const float2 grid = (float2(pixel) + 0.5f + float2(Params6.xy)) / float(tileSize) - 0.5f;
        const float2 gridFloor = floor(grid);
        const float2 frac2 = grid - gridFloor;
        const int2 baseTile = int2(gridFloor);

        float bilinearWeights[4];
        bilinearWeights[0] = (1.0f - frac2.x) * (1.0f - frac2.y);
        bilinearWeights[1] = frac2.x * (1.0f - frac2.y);
        bilinearWeights[2] = (1.0f - frac2.x) * frac2.y;
        bilinearWeights[3] = frac2.x * frac2.y;

        // 自分のタイルは、各軸で補間係数が0.5以上なら +1 側にいる(上の格子の作り方から)
        ownTileSlot = ((frac2.x >= 0.5f) ? 1u : 0u) | (((frac2.y >= 0.5f) ? 1u : 0u) << 1u);

        // タイル数はジッター有効時に +1 されている(Params1.x / Params5.y)
        const int2 maxTile = int2(int(max(Params1.x, 1u)) - 1, int(max(Params5.y, 1u)) - 1);

        [unroll]
        for (uint j = 0u; j < 4u; ++j)
        {
            // 【画面外へ出た分は端のタイルへ寄せる(クランプ)】切り捨てると Σb ≠ 1 になり
            // 割り戻しが狂う。クランプなら同じタイルが2回現れるだけで、
            // q̄ = Σ_j b_j q_j は「そのタイルの重みが合算された混合」として厳密に正しいまま。
            // しかも画面端では自分のタイル自身が端のタイルなので、寄せ先は自分になる
            const int2 coord = clamp(
                baseTile + int2(int(j & 1u), int(j >> 1u)), int2(0, 0), maxTile);
            poolTiles[j] = MegaLightsLoadPoolTile(
                uint2(coord), outputSize, tileSize, candidateCount, bilinearWeights[j]);
        }
        poolTileCount = 4u;
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

        // --- RIS: 候補プールから M 個引いて、寄与の大きさに比例する重みで1つ残す ---
        // 【スロットの抽選だけ低食い違い量列にする】画素ごとの位相をブルーノイズ的に配り、
        // 同じ画素の中では M 個が均等に散るようにする。周辺分布は一様のままなので
        // 割り戻しも期待値も変わらない(MegaLightsCommon.hlsli の説明を参照)。
        // 採用判定は白色のまま ―― あちらは M 回の判定の独立性を使っている。
        // 標本番号を位相の次元として渡し、N本が同じ列を引かないようにする
        const float slotPhase = MegaLightsPixelPhase(pixel, Params1.w, sampleSlot);
        uint rngState = HashUint(pixel.x + pixel.y * outputSize.x + Params1.w * 0x9E3779B9u +
                                 sampleSlot * 0xB5297A4Du);

        // --- 参照するタイルを b_j の確率で1つ選ぶ(確率的バイリニア参照) ---
        // 【白色乱数にしないこと】隣接画素で離れる配り方でないと、タイル境界を溶かした先が
        // また低周波になる。位相の次元は slotPhase(= sampleSlot)と衝突しないよう離す。
        // 【粒度】画素ごと(モード2)に選ぶとばらけるが、クアッド層化(Params4.w)は
        // タイルのスロットを2x2の4画素へ割り振るので、4人が別のプールを引くと層化が壊れる。
        // クアッドごと(モード1)なら層化は保たれ、相関の単位が16x16から2x2まで落ちる。
        // **既定はクアッドごと** ―― どちらが良いかは実測で決める
        // 【標本ごとに引き直す】sampleSlot を次元に渡すので、N本が別のタイルを引いてさらにばらける
        uint selectedTile = ownTileSlot;
        if (poolTileCount > 1u)
        {
            const uint2 phasePixel = (bilinearMode == 2u) ? pixel : (pixel >> 1u);
            const float tileRandom = MegaLightsPixelPhase(phasePixel, Params1.w, 64u + sampleSlot);
            float cdf = 0.0f;
            bool picked = false;
            [unroll]
            for (uint j = 0u; j < 4u; ++j)
            {
                cdf += poolTiles[j].BilinearWeight;
                if (!picked && tileRandom < cdf)
                {
                    selectedTile = j;
                    picked = true;
                }
            }
            // picked が false のまま(浮動小数の丸めで最後まで超えなかった)なら
            // ownTileSlot が残る。重みが必ず0.25以上あるので、確率0のタイルへは落ちない
        }
        const MegaLightsPoolTile pool = poolTiles[selectedTile];

        // --- クアッド層化(手法3。Params4.w) ---
        // 2x2クアッドの4画素へ候補スロットを1/4ずつ割り当て、**クアッド全体でK個のスロットを
        // 重複なく列挙させる**。手法3は4画素の標本を平均するので、4人が同じ灯を引いてしまうと
        // 実効的な標本数が減る。
        //
        // 【周辺分布は変わらないので割り戻しはそのまま厳密】プールのK個のスロットは
        // 混合分布(一様枝+重み枝)からの **i.i.d. 抽出** である(MegaLightsTilePool.hlsl)。
        // スロットの中身を見ずに番号だけで選ぶ限り、どのスロットを引いても得られる灯の分布は
        // 同じ混合分布のままで、下の sourcePdf の式は変わらない。
        // 【MegaLightsCommon.hlsli が禁じている層化とは別物】あちらが禁じているのは
        // 「1つのスロット列の中で (m + phase)/M と等間隔に取る」形で、周辺分布が層の中に
        // 閉じてしまうために提案と割り戻しが食い違う。こちらは層の中で一様に引いている。
        // 【レーンの割り当てはクアッドごと・フレームごとに回す】固定すると
        // 「左上の画素はいつも先頭8スロットから引く」形になり、2画素周期の模様が焼き付く。
        // 標本番号ぶんもずらして、同じ画素のN本が同じ層に固まらないようにする
        // 【層化は「選んだタイル」の有効候補数で行う】自分のタイルの数で割ると、
        // 背景タイルを選んだときに stratumCount が0になり、スロットの添字が確保外へ飛ぶ
        const uint poolCandidates = pool.ValidCandidates;
        const bool quadStratify = (Params4.w != 0u);
        uint stratumBase = 0u;
        uint stratumCount = poolCandidates;
        if (quadStratify && poolCandidates >= 4u)
        {
            const uint2 quad = pixel >> 1u;
            const uint lane = (pixel.x & 1u) | ((pixel.y & 1u) << 1u);
            const uint rotation = HashUint(quad.x + quad.y * 0x9E3779B9u + Params1.w * 0x85EBCA6Bu) & 3u;
            const uint stratum = (lane + rotation + sampleSlot) & 3u;
            const uint width = poolCandidates >> 2u;
            stratumBase = stratum * width;
            // 最後の層は端数を引き受ける(K=32なら割り切れるが、Kを変えても定義域が欠けないように)
            stratumCount = (stratum == 3u) ? (poolCandidates - stratumBase) : width;
        }

        float risWeightSum = 0.0f;
        uint selectedLightIndex = 0xFFFFFFFFu;
        float selectedTargetPdf = 0.0f;

        // 【空のタイルを選んだときは1本も引かない】バイリニア参照では背景タイルを
        // 引き当てうる。その標本は「M個の候補を検討して全部外した」のと同じ扱いになり、
        // 下の棄却の枝が M = sampleCount のリザーバを書く(混合分布 q̄ は
        // そのタイルの寄与を0として数えているので、期待値は変わらない)
        const bool poolUsable = (poolCandidates > 0u) && (pool.SumW > 0.0f);

        [loop]
        for (uint m = 0u; poolUsable && m < sampleCount; ++m)
        {
            const float slotRandom = MegaLightsLowDiscrepancy1D(m, slotPhase);
            // 層化しているときは自分の層の中だけを引く(層化していなければ全スロットが自分の層)
            const uint slot =
                stratumBase + min((uint)(slotRandom * float(stratumCount)), stratumCount - 1u);
            const uint lightIndex = TilePool[pool.Base + kMegaLightsTilePoolHeader + 2u * slot + 0u];
            const float candidateWeight = asfloat(TilePool[pool.Base + kMegaLightsTilePoolHeader + 2u * slot + 1u]);
            // 採用判定の乱数は候補が無効でも必ず引いて状態を進める
            // (引く回数がループの中身で変わると、ピクセルごとに乱数列の位相がずれる)
            const float acceptRandom = NextRandom(rngState);

            if (lightIndex == 0xFFFFFFFFu || candidateWeight <= 0.0f)
            {
                continue;
            }
            // 遮蔽が確定している灯は目標関数0として扱う(= 選ばない)。提案分布は
            // 変えていないので「引いたが目標0で外れた」という正当な棄却で、期待値は不変
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

            // 目標関数。遮蔽は含めない(含めるにはレイを撃つことになりRISの意味が無くなる)
            const float3 unshadowed = EvaluatePunctualContribution(
                light, geometry, N, V, NdotV, albedo, metallic, roughness, translucency, energy, 1.0f);
            const float targetPdf = Luminance(unshadowed);
            if (targetPdf <= 0.0f)
            {
                continue;
            }

            // 提案分布の確率密度。プールは「一様枝 + 重み枝」の混合で引いている
            // (MegaLightsTilePool.hlsl)ので、割り戻しも同じ混合式で行う。
            // プールが w_i / SumW / 届いた灯数 を別々に持っているので厳密に再現できる
            float sourcePdf;
            if (poolTileCount == 1u)
            {
                // 従来経路。自分のタイルの提案確率そのもの。
                // **重みはプールに書かれている値をそのまま使う**(再計算しない)ので、
                // バイリニア参照を切ったときの出力は変更前とビット同一になる
                sourcePdf = kMegaLightsUniformMixFraction / float(max(pool.ReachableCount, 1u)) +
                            (1.0f - kMegaLightsUniformMixFraction) * (candidateWeight / pool.SumW);
            }
            else
            {
                // 確率的バイリニア参照。**選んだタイルの q_i(y) で割ってはいけない** ――
                // q_i(y) = 0 の灯が存在するので定義域が欠けてバイアスになる。
                // 混合分布 q̄(y) = Σ_j b_j q_j(y) で割ること(理由は MegaLightsPoolTile の説明)。
                // 境界球はタイルに依らないので、4タイルぶんの前に1回だけ求める
                float3 viewCenter;
                float radius;
                ComputeLightBoundingSphere(light, View, viewCenter, radius);
                sourcePdf = 0.0f;
                [unroll]
                for (uint j = 0u; j < 4u; ++j)
                {
                    if (poolTiles[j].BilinearWeight > 0.0f)
                    {
                        sourcePdf += poolTiles[j].BilinearWeight *
                                     MegaLightsTileSourcePdf(poolTiles[j], light, viewCenter, radius);
                    }
                }
                // 【0除算のガード】選んだタイルから引けた灯なので q̄ > 0 のはずだが、
                // ここでNaNを出すと直接光→SceneColor→TAAの履歴まで壊れて復帰しない
                if (sourcePdf <= 0.0f)
                {
                    continue;
                }
            }
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
            // 【M=0 で書いてはいけない ―― 空間再利用の明るい側の系統誤差の原因だった】
            // 「M個引いて全部外した(全候補が背向き等)」は、遮蔽で殺した場合と同じく
            // 「M個の候補を検討して寄与0だった」という正当な結果である。ここを M=0 にすると、
            // 結合の分母(confidenceSum と、不偏化方式の Z)からこの画素の分だけが消える。
            // 全部外すのは背向き候補率の高い画素に集中して起きるため、その周囲だけ分母が
            // 系統的に過小になり、期待値が明るい側へ偏る(不偏性の条件は
            // 「分母 = 選ばれた灯を生成しえた候補の M の合計」であり、
            // 外した画素も生成しえた=確率が正だった以上、M ごと数えなければならない)
            MegaLightsReservoir rejected = MegaLightsMakeEmptyReservoir();
            rejected.M = float(sampleCount);
            Reservoirs[reservoirBase + sampleSlot] = rejected;
            if (ownsCache && Params4.x == 0u)
            {
                BlockedLights[reservoirIndex] = 0xFFFFFFFFu;
            }
            continue;
        }

        // --- 球光源: 狙う点を抽選してリザーバへ持たせる ---
        // 【選択ループの外で引くこと】ループ内で引くと、候補が無効だった回数で乱数列の位相が
        // ずれて画素ごとに相関が出る。半径0なら使われないが、引く回数は常に同じにしておく
        const float2 sampleUV = float2(NextRandom(rngState), NextRandom(rngState));

        // --- 初期可視レイ: 遮蔽されていたらここで殺す ---
        // 殺すと「遮蔽で真っ黒になる灯」が近傍へ配られなくなる(RTXDI系の標準の段)。
        // 【ただし空間再利用の不偏化(Z)とは両立しない】殺された画素の実効的な定義域は
        // p̂ から p̂・可視率 へ変わるが、Zはレイを撃たずに可視率を判定できないため、
        // 影の縁に暗い側の系統誤差が残る。Params2.w で切って測れるようにしてある。
        // 【レイはここで標本ごとに1本ずつ撃つ】1画素あたりの影レイの本数は
        // samplesPerPixel そのものになる
        bool visible = true;
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
                    // シェード側と同じ点へ撃つ(違う点を狙うと、殺す判断と影の階調が食い違う)
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
            // 【どの灯を殺したかを残す ―― 全部消して書いてはいけない】
            // W=0 なので結合の選択からは外れる(IsEmptyがtrue)が、
            // 「この画素はこの灯への可視レイが遮蔽された(V=0 が確定した)」という事実を
            // ライト番号と可視フラグで持ち回る。空間再利用の不偏化の分母(Z)は
            // 「その候補が選ばれた灯を生成しえたか」を数えるが、殺された灯は
            // その候補からは決して出て来られない。番号を消すと Z がそれを知れずに
            // M を数え、殺しの起きる画素の周囲だけ分母が太って**暗い側の系統誤差**になる
            // (実測 -3.6%。docs/ImplementationDetail.md 61.7f)。
            // M は残す ―― 「M個の候補を検討した」ことは事実で、他の灯の Z には数えるべき
            MegaLightsReservoir killed = MegaLightsMakeEmptyReservoir();
            killed.IndexAndFlags = MegaLightsPackLightAndFlags(selectedLightIndex, false);
            killed.SampleUV = MegaLightsPackSampleUV(sampleUV);
            killed.M = float(sampleCount);
            Reservoirs[reservoirBase + sampleSlot] = killed;
            // 【点光源だけキャッシュする】球光源の殺しは球面上の1点への判定で、
            // 灯そのものの遮蔽の証明にならない
            if (ownsCache)
            {
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

        // 可視レイを通った(または影を撃たない灯を選んだ)。キャッシュの灯と同じなら
        // 「遮蔽が解けた」ことの証明なので消す。違う灯ならキャッシュは維持
        if (ownsCache && (Params4.x == 0u || BlockedLights[reservoirIndex] == selectedLightIndex))
        {
            BlockedLights[reservoirIndex] = 0xFFFFFFFFu;
        }

        MegaLightsReservoir reservoir;
        // ライト番号は16bitへ詰める(kMaxLights = 1024 なので収まる)
        reservoir.IndexAndFlags = MegaLightsPackLightAndFlags(selectedLightIndex, true);
        // 球面上のどこを狙ったか。時空間再利用がこの点ごと持ち回るので、借りた側も同じ点へ撃つ
        // (半径0なら中心になり、点光源と完全に一致する)
        reservoir.SampleUV = MegaLightsPackSampleUV(sampleUV);
        // 不偏寄与重み W = (1/p̂(y)) * (1/M) * Σw
        reservoir.W = risWeightSum / (float(sampleCount) * selectedTargetPdf);
        reservoir.M = float(sampleCount);
        Reservoirs[reservoirBase + sampleSlot] = reservoir;
    }
}
