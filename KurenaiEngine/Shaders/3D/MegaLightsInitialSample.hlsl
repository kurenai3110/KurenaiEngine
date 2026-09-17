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
// 前フレームの可視灯リスト(提案分布の第3成分)。**候補プールが実際に引いたのと
// 同じリストを見ること** ―― どのリストを使ったかはプールのヘッダ[base+3]に書いてある。
StructuredBuffer<uint> VisibleLights : register(t10);


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

// --- 候補プールの確率的バイリニア参照(Params6.w) ---
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
// 【不偏性がここの肝 ―― 選んだタイルの q_i(y) で割ってはいけない】q_i(y) = 0 の灯
// (そのタイルへ届かない灯)が存在するので、選んだタイルで割ると定義域が欠けてバイアスになる。
// 混合分布 q̄(y) = Σ_j b_j q_j(y) で割ること。q̄ > 0 は「4つのうち1つでも届く」で保証され、
// **画素自身のタイルは必ず4つに含まれる**(バイリニアの定義から、しかも重み0.25以上)ので、
// その画素に寄与しうる灯は必ず q̄ > 0 になる。
// 1タイルぶんの可視灯リストのうち、レジスタへ載せる本数。
// **kMegaLightsVisibleListCapacityMax 以下であること。** これを超える分は
// 構造化バッファから読み直す(既定の容量8では溢れない)
static const uint kMegaLightsPoolTileListCache = 8u;

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
    // 前フレームの可視灯リスト。**そのタイルのヘッダ[base+3]が指す先**を読む。
    // 再投影の式をこちらに書かないのは、どのリストを引いたかを決めたのが候補プールだから
    uint ListLength;
    // 可視灯リストの先頭 kMegaLightsPoolTileListCache 本だけをレジスタへ載せる。
    // 【全部載せてはいけない】この構造体は poolTiles[4] で4つ抱えるので、
    // 上限いっぱい(16)をキャッシュすると 64 uint がレジスタを占める。
    // 実測でそれを 8 本へ半減させると MegaLightsInitial が -1.2ms 動いた
    // (根拠は docs/ImplementationDetail.md 61.7aa)。
    // **上限そのものは下げない** ―― 容量は CLI で 16 まで振れる余地として
    // 意図的に残されている(EngineDefaults.h の MegaLightsVisibleListCapacity)。
    // 溢れた分はバッファから読む。既定の容量8では1回も起きない
    uint ListCache[kMegaLightsPoolTileListCache];
    // 溢れた分を読みに行くための先頭添字(0xFFFFFFFF でリスト無し)
    uint ListBase;
    // w_j(y) の再計算に要る。側面はタイル座標から、深度スラブはヘッダから作る
    TileFrustum Frustum;
    float3 AabbMin;
    float3 AabbMax;
};

// タイル1つぶんの文脈をヘッダから組み立てる。
// 【錐台は候補プールを書いたときと同じ画素範囲で作ること】定義域がずれると、
// q_j(y) が実際の抽出確率と食い違う
MegaLightsPoolTile MegaLightsLoadPoolTile(
    uint2 tileCoord, uint2 outputSize, uint tileSize, uint candidateCount, float bilinearWeight)
{
    MegaLightsPoolTile tile;
    tile.Base = MegaLightsTilePoolBase(tileCoord, Params1.x, candidateCount);
    tile.SumW = asfloat(TilePool[tile.Base + 0u]);
    tile.ReachableCount = TilePool[tile.Base + 1u];
    tile.ValidCandidates = TilePool[tile.Base + 2u];
    tile.BilinearWeight = bilinearWeight;

    // 【リストはここで登録へ載せる】RIS の M 回の抽選のたびにバッファを読み直すと
    // 1画素あたり M*L 回の読み出しになる。載せ替えは1タイル1回で済む
    const uint listBase = TilePool[tile.Base + 3u];
    tile.ListLength = 0u;
    tile.ListBase = listBase;
    [unroll]
    for (uint li = 0u; li < kMegaLightsPoolTileListCache; ++li)
    {
        tile.ListCache[li] = kMegaLightsInvalidLight;
    }
    if (listBase != 0xFFFFFFFFu && asfloat(Params7.x) > 0.0f)
    {
        // 書き手(候補プール)と同じ1つの関数を通す。実行時の容量でクランプしない
        tile.ListLength = MegaLightsVisibleListLength(VisibleLights, listBase);
        [unroll]
        for (uint lj = 0u; lj < kMegaLightsPoolTileListCache; ++lj)
        {
            if (lj < tile.ListLength)
            {
                tile.ListCache[lj] = VisibleLights[listBase + kMegaLightsVisibleListHeader + lj];
            }
        }
    }

    // 深度スラブは候補プールがヘッダへ書いている(そのタイルを走査しないと分からないため)
    const float nearestViewZ = asfloat(TilePool[tile.Base + 4u]);
    const float farthestViewZ = asfloat(TilePool[tile.Base + 5u]);
    const int2 tilePixelOrigin = int2(tileCoord * tileSize);
    tile.Frustum = MakeTileFrustumFromPixelOrigin(
        tilePixelOrigin, outputSize, Params3.x, Params3.y, nearestViewZ, farthestViewZ);
    TileViewSpaceAABBFromPixelOrigin(
        tilePixelOrigin, outputSize, Params3.x, Params3.y, nearestViewZ, farthestViewZ,
        tile.AabbMin, tile.AabbMax);
    return tile;
}

// そのタイルの可視灯リストに灯 y が何回現れるか。
// 【出現回数で数える】リストは重複を許す(書き手の競合を正しさの問題にしないため)。
// 数え方は MegaLightsVisibleLights.hlsl の書き手と1つの規約で揃えてある
float MegaLightsTileListCount(MegaLightsPoolTile tile, uint lightIndex)
{
    float count = 0.0f;
    // レジスタに載っている先頭ぶん
    [unroll]
    for (uint k = 0u; k < kMegaLightsPoolTileListCache; ++k)
    {
        if (k < tile.ListLength && tile.ListCache[k] == lightIndex)
        {
            count += 1.0f;
        }
    }
    // 載りきらなかった分だけバッファから読む。**既定の容量8では1回も回らない。**
    // 【足す順序を変えないこと】先頭から昇順に足しているので、
    // 全部キャッシュしていた頃と浮動小数の加算順序が一致する
    [loop]
    for (uint k2 = kMegaLightsPoolTileListCache; k2 < tile.ListLength; ++k2)
    {
        if (VisibleLights[tile.ListBase + kMegaLightsVisibleListHeader + k2] == lightIndex)
        {
            count += 1.0f;
        }
    }
    return count;
}

// そのタイルが灯 y を1スロットぶん提案する確率 q_j(y)。届かなければ 0。
// 【重みは TileLightCulling.hlsli の関数で再計算する】書き手(MegaLightsTilePool.hlsl)と
// 同じ1本を呼ぶこと。式を写すと、ずれた瞬間に静かにバイアスが乗る
float MegaLightsTileSourcePdf(
    MegaLightsPoolTile tile, uint lightIndex, GPULight light, float3 viewCenter, float radius)
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
    // 【3成分の混合。a>0 が不偏性の担保】
    //   q_j(y) = a/R_j + (1-a) * [ (1-c) * w_j(y)/SumW_j + c * count_j(y)/L_j ]
    // c をどれだけ上げても一様枝 a は削らないので、そのタイルへ届くどの灯にも
    // 正の下限確率が残る。**リストに載っている灯は3成分すべてを足すこと** ――
    // リスト枝で引けた灯は重み枝からも一様枝からも出て来られる
    const float listMix = (tile.ListLength > 0u) ? asfloat(Params7.x) : 0.0f;
    const float listTerm =
        (tile.ListLength > 0u)
            ? (listMix * MegaLightsTileListCount(tile, lightIndex) / float(tile.ListLength))
            : 0.0f;
    return kMegaLightsUniformMixFraction / float(max(tile.ReachableCount, 1u)) +
           (1.0f - kMegaLightsUniformMixFraction) * ((1.0f - listMix) * (w / tile.SumW) + listTerm);
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
    uint sampleSlot, uint2 pixel, uint2 outputSize, MegaLightsPoolTile poolTiles[4],
    uint poolTileCount, uint ownTileSlot, uint bilinearMode,
    // 共有ブロックの1辺の log2(1 = 2x2 / 2 = 4x4)。層化とタイル選択の粒度を決める
    uint blockShift,
    uint sampleCount, uint blockedLight,
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
    const float slotPhase = MegaLightsPixelPhaseMode(pixel, Params1.w, sampleSlot, Params7.z);
    uint rngState = HashUint(pixel.x + pixel.y * outputSize.x + Params1.w * 0x9E3779B9u +
                             sampleSlot * 0xB5297A4Du);

    // --- 参照するタイルを b_j の確率で1つ選ぶ(確率的バイリニア参照) ---
    // 【白色乱数にしないこと】隣接画素で離れる配り方でないと、タイル境界を溶かした先が
    // また低周波になる。位相の次元は slotPhase(= sampleSlot)と衝突しないよう離す。
    // 【粒度】画素ごと(モード2)に選ぶとばらけるが、クアッド層化(Params4.w)は
    // タイルのスロットを共有ブロックの画素へ割り振るので、ブロックの住人が別のプールを
    // 引くと層化が壊れる。ブロックごと(モード1)なら層化は保たれ、
    // 相関の単位が16x16からブロックの大きさまで落ちる。
    // **既定はブロックごと** ―― どちらが良いかは実測で決める
    // 【共有半径に追随させること】半径2(4x4)で >>1 のままにすると、
    // 同じブロックの中で2種類のプールが引かれて層化が半分壊れる
    // 【標本ごとに引き直す】sampleSlot を次元に渡すので、N本が別のタイルを引いてさらにばらける
    uint selectedTile = ownTileSlot;
    if (poolTileCount > 1u)
    {
        const uint2 phasePixel = (bilinearMode == 2u) ? pixel : (pixel >> blockShift);
        const float tileRandom =
            MegaLightsPixelPhaseMode(phasePixel, Params1.w, 64u + sampleSlot, Params7.z);
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
    // --- 選んだタイルから「両方の経路で要るスカラ」だけを取り出す ---
    // 【構造体ごと動的添字で複製してはいけない】`poolTiles[selectedTile]` と書くと、
    // DXC は配列を動的に添字できる形へ落とすため、**poolTiles 全体が
    // スクラッチメモリへ追い出される**。中には ListCache[16] が4タイルぶん = 64 uint
    // 入っており、実測で alloca [64 x i32] と [4 x *] x5、getelementptr 250 /
    // store 168 / load 87 が立っていた(根拠は docs/ImplementationDetail.md 61.7aa)。
    // 定数添字だけで書けば配列はレジスタに留まる。
    // 【ListCache と ReachableCount と ListLength はここでは選ばない】
    // それらを使うのは下の `poolTileCount == 1u` の枝だけで、その枝では
    // poolTiles[1..3] は poolTiles[0] の複製かつ selectedTile == 0 なので
    // poolTiles[0] を直接読めばよい(値は厳密に同じ)
    uint poolBase = poolTiles[0].Base;
    uint poolValidCandidates = poolTiles[0].ValidCandidates;
    float poolSumW = poolTiles[0].SumW;
    [unroll]
    for (uint pj = 1u; pj < 4u; ++pj)
    {
        const bool hit = (pj == selectedTile);
        poolBase = hit ? poolTiles[pj].Base : poolBase;
        poolValidCandidates = hit ? poolTiles[pj].ValidCandidates : poolValidCandidates;
        poolSumW = hit ? poolTiles[pj].SumW : poolSumW;
    }

    // --- クアッド層化(手法3。Params4.w) ---
    // 2x2クアッドの4画素へ候補スロットを1/4ずつ割り当て、クアッド全体で列挙させる。
    // 【層化は「選んだタイル」の有効候補数で行う】自分のタイルの数で割ると、
    // 背景タイルを選んだときに stratumCount が0になり、スロットの添字が確保外へ飛ぶ
    const uint poolCandidates = poolValidCandidates;
    const bool quadStratify = (Params4.w != 0u);
    // 層の数は共有ブロックの画素数。半径1なら4層、半径2なら16層
    const uint blockPixels = 1u << (2u * blockShift);
    uint stratumBase = 0u;
    uint stratumCount = poolCandidates;
    if (quadStratify && poolCandidates >= blockPixels)
    {
        const uint2 quad = pixel >> blockShift;
        const uint mask = (1u << blockShift) - 1u;
        const uint lane = (pixel.x & mask) | ((pixel.y & mask) << blockShift);
        const uint rotation =
            HashUint(quad.x + quad.y * 0x9E3779B9u + Params1.w * 0x85EBCA6Bu) & (blockPixels - 1u);
        const uint stratum = (lane + rotation + sampleSlot) & (blockPixels - 1u);
        const uint width = poolCandidates / blockPixels;
        stratumBase = stratum * width;
        // 最後の層は端数を引き受け、候補の定義域を欠けさせない
        stratumCount = (stratum == blockPixels - 1u) ? (poolCandidates - stratumBase) : width;
    }

    float risWeightSum = 0.0f;
    selectedLightIndex = 0xFFFFFFFFu;
    float selectedTargetPdf = 0.0f;
    visible = false;

    // 【空のタイルを選んだときは1本も引かない】バイリニア参照では背景タイルを
    // 引き当てうる。その標本は「M個の候補を検討して全部外した」のと同じ扱いになり、
    // 下の棄却の枝が M = sampleCount のリザーバを書く(混合分布 q̄ は
    // そのタイルの寄与を0として数えているので、期待値は変わらない)
    const bool poolUsable = (poolCandidates > 0u) && (poolSumW > 0.0f);

    [loop]
    for (uint m = 0u; poolUsable && m < sampleCount; ++m)
    {
        const float slotRandom = MegaLightsLowDiscrepancy1D(m, slotPhase);
        const uint slot =
            stratumBase + min((uint)(slotRandom * float(stratumCount)), stratumCount - 1u);
        const uint lightIndex = TilePool[poolBase + kMegaLightsTilePoolHeader + 2u * slot + 0u];
        const float candidateWeight = asfloat(TilePool[poolBase + kMegaLightsTilePoolHeader + 2u * slot + 1u]);
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

        // 提案分布の確率密度。プールは「一様枝 + 重み枝」の混合で引いている
        // (MegaLightsTilePool.hlsl)ので、割り戻しも同じ混合式で行う
        float sourcePdf;
        if (poolTileCount == 1u)
        {
            // 従来経路。自分のタイルの提案確率そのもの。
            // **重みはプールに書かれている値をそのまま使う**(再計算しない)ので、
            // バイリニア参照を切ったときの出力は変更前とビット同一になる
            // 可視灯リストの枝(c)も同じ形で足す。**重みはプールに書かれている値を
            // そのまま使う**(再計算しない)ので、c=0 かつバイリニア参照を切ったときの
            // 出力は従来とビット同一になる
            const float listMix = (poolTiles[0].ListLength > 0u) ? asfloat(Params7.x) : 0.0f;
            const float listTerm =
                (poolTiles[0].ListLength > 0u)
                    ? (listMix * MegaLightsTileListCount(poolTiles[0], lightIndex) / float(poolTiles[0].ListLength))
                    : 0.0f;
            sourcePdf = kMegaLightsUniformMixFraction / float(max(poolTiles[0].ReachableCount, 1u)) +
                        (1.0f - kMegaLightsUniformMixFraction) *
                            ((1.0f - listMix) * (candidateWeight / poolTiles[0].SumW) + listTerm);
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
                                 MegaLightsTileSourcePdf(poolTiles[j], lightIndex, light, viewCenter, radius);
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
    // クアッド共有で標本を借りる範囲の半径(1=2x2 / 2=4x4)を、ブロック1辺の log2 にする。
    // **Resolve の収集範囲と必ず同じ値を見ること**(片方だけ広げると層化が静かに壊れる)
    const uint blockShift = clamp(Params5.y, 1u, 2u);
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
    const uint2 tileCoord = pixel / tileSize;
    const uint candidateCount = Params1.z;
    const uint tileBase = MegaLightsTilePoolBase(tileCoord, Params1.x, candidateCount);

    const float sumW = asfloat(TilePool[tileBase + 0u]);
    const uint validCandidates = TilePool[tileBase + 2u];
    const uint sampleCount = max(Params0.z, 1u);

    // --- 提案分布の第3成分(可視灯リスト)を、候補プールが引いたのと同じ形で復元する ---
    //
    // 【再投影の式をここに書かない】どのタイルのリストを引いたかは候補プールが決めており、
    // その結果の添字がヘッダ[base+3]に入っている。式を両側に書くと、片方だけ直したときに
    // 割り戻しが実際の抽出確率と食い違って静かに偏る(プール本体と同じ考え方)。
    //
    // 【リストをレジスタへ載せてから数える】RIS の M 回の抽選のたびにバッファを
    // L 回読み直すと 1画素あたり M*L 回の読み出しになる。載せ替えは1画素1回で済む
    const uint listBase = TilePool[tileBase + 3u];
    const bool listValid = (listBase != 0xFFFFFFFFu);
    // 候補プールが実際に使った混合率。**新しい定数の枠を足していない** ――
    // このcbufferは MegaLights の5本が共有しており、宣言を1つ増やすだけで
    // 5本すべてのDXILが変わって、機能を切っていても絵がビット同一でなくなる
    const float listMix = listValid ? asfloat(Params6.z) : 0.0f;
    uint listLength = 0u;
    uint listCache[kMegaLightsVisibleListCapacityMax];
    {
        [unroll]
        for (uint i = 0u; i < kMegaLightsVisibleListCapacityMax; ++i)
        {
            listCache[i] = kMegaLightsInvalidLight;
        }
        if (listValid)
        {
            // 書き手(候補プール)と同じ1つの関数を通す。実行時の容量でクランプしない
            listLength = MegaLightsVisibleListLength(VisibleLights, listBase);
            [unroll]
            for (uint j = 0u; j < kMegaLightsVisibleListCapacityMax; ++j)
            {
                if (j < listLength)
                {
                    listCache[j] = VisibleLights[listBase + kMegaLightsVisibleListHeader + j];
                }
            }
        }
    }
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
    const uint bilinearMode = Params6.w;
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
        // 画素中心を「タイル中心の格子」へ移す。タイル t の中心は画素 t*S + S/2 なので、
        // g = (pixel + 0.5)/S - 0.5 とすれば g の整数部が左上タイル、小数部が補間係数になる。
        const float2 grid = (float2(pixel) + 0.5f) / float(tileSize) - 0.5f;
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

        // X/Yそれぞれのタイル数から末尾の有効添字を求める
        const int2 maxTile = int2(int(max(Params1.x, 1u)) - 1, int(max(Params6.z, 1u)) - 1);

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
        uint selectedLightIndex;
        bool visible;
        const MegaLightsReservoir reservoir = DrawSample(
            sampleSlot, pixel, outputSize, poolTiles, poolTileCount, ownTileSlot, bilinearMode,
            blockShift, sampleCount,
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

}
