// MegaLights の候補プール。タイル(16x16ピクセル)ごとに「そこへ届くライト」を走査し、
// 寄与の大きさに比例した確率で K 灯を抽出して、重みつきで書き出す。
//
// 【何のためにあるか】MegaLights は各ピクセルで候補の中から数灯だけを確率的に選び、
// 選んだ確率で割り戻すことで「全灯を評価したのと同じ期待値」を作る。その**候補集合と
// 提案分布(どの確率で提案したか)**を用意するのがこのパス。ピクセルごとに全灯を走査すると
// ライト数に比例したコストが戻ってきてしまうので、タイル単位で1回だけ作る。
//
// 【既存のタイルライトカリング(LightCulling.hlsl)を流用しない理由】3つある。
//   1. あちらは1タイル64灯で頭打ちになる。MegaLights が扱いたいのはそれを超える灯数
//   2. あちらは重みを持たない。重要度サンプリングの提案分布 p_i が作れない
//   3. あちらは「見た目を変えない純粋な最適化」という契約を持っている。A/Bの対照として
//      無傷で残す価値がある
// ただし**「そのライトがタイルへ届くか」の判定だけは共有する**(TileLightCulling.hlsli)。
// 定義域がずれると、片方にしか入らないライトが出てサンプリングにバイアスが乗る。
//
// 【出力バッファのレイアウト】1本のRWStructuredBuffer<uint>に、タイルごとの固定長ブロックで詰める。
//   base = tileIndex * (kMegaLightsTilePoolHeader + 2 * K)
//   [base + 0]           = asuint(SumW)  そのタイルに届く全灯の重みの合計
//   [base + 1]           = 届いたライト数(重みが正になった灯の数。混合抽出の一様成分の
//                          割り戻しに使う。デバッグ表示と検証も兼ねる)
//   [base + 2]           = 有効な候補数(0 か K。SumW が0なら0)
//   [base + 3]           = 予約
//   [base + 4]           = asfloat(nearestViewZ)  タイル内で最も手前のサーフェスのView空間Z
//   [base + 5]           = asfloat(farthestViewZ) 同 最も奥
//   [base + 6 + 2n + 0]  = n番目の候補のライト番号(無効なスロットは 0xFFFFFFFF)
//   [base + 6 + 2n + 1]  = asuint(w_i)  その灯の重み
//
// 【深度スラブをヘッダへ入れてある理由】空間再利用のMIS重み(生成化バランスヒューリスティック)は
// 「隣の画素がそのサンプルを生成しえたか」を要る。それは「その灯が隣のタイルへ届くか」で決まり、
// 判定には隣のタイルの視錐台が要る。錐台の側面はタイル座標から作れるが、**深度スラブだけは
// そのタイルの深度を走査しないと分からない**ので、ここで書いておく。
// 無ければ隣のタイルの定義域が分からず、MIS重みが近似になってバイアスが残る
//
// 【invPdf ではなく w_i と SumW を別々に持つ理由】空間再利用の MIS 重み(生成化バランス
// ヒューリスティック)は「隣のピクセルがこのサンプルを生成しえた確率」を要る。w_i と SumW が
// 別々にあれば、隣のタイルの SumW を1回読んで w_i を再計算するだけで p_j(y) が**近似なしに**
// 求まる。invPdf に畳んでしまうとこれができなくなる。
//
// このパスはレイを撃たないので、3バリアント(SM5.0 / SM6.5 / SM6.6)すべてでコンパイルされる。

// C++側 KurenaiEngine3D.cpp の MegaLightsTilePoolConstants と並びを一致させること
cbuffer MegaLightsTilePoolConstants : register(b0)
{
    // ワールド座標をView空間へ変換する行列(ライトをタイル錐台と同じ空間へ持ち込むため)
    float4x4 View;
    // xy=候補プールのタイル数、z=有効ライト数, w=1タイルあたりの候補数K
    uint4 TileParams;
    // x=レンダー解像度の幅, y=同 高さ, zw=未使用
    uint4 RenderSize;
    // x=射影行列の(0,0)成分, y=同(1,1)成分、z=深度リニアライズ定数a, w=同b(viewZ = b / (depth - a))
    float4 ProjParams;
    // x=フレーム番号(サンプルを毎フレーム変えるための乱数の種)、yzw=未使用
    uint4 PoolParams;
    // 可視灯リスト(提案分布の第3成分)。
    // x=リストの容量(0なら機能そのものが無効)、y=前フレームのリストが使えるか(0で使わない)、
    // z=asuint(リスト枝へ回す混合率 c)、w=未使用
    uint4 VisibleListParams;
};

// ライト1灯ぶんのデータ(struct GPULight)とライトリストの宣言。BRDFは使わないため
// KURENAI_PUNCTUAL_LIGHTING_BRDF は定義しない(距離減衰 DistanceAttenuation は無条件部にある)
#define KURENAI_PUNCTUAL_LIGHT_REGISTER t0
#include "PunctualLighting.hlsli"
// タイル錐台の組み立て・AABB・「そのライトが届くか」の判定。GPULightを使うのでこの順で読む
#include "TileLightCulling.hlsli"
// 候補プールのレイアウト(ヘッダの大きさと添字の作り方)。書き手と読み手で1か所に集めてある
#include "MegaLightsCommon.hlsli"

Texture2D<float> DepthTexture : register(t1);
// タイルを前フレームへ再投影するための速度(TAAと同じ引き方: historyUv = uv - velocity)
Texture2D<float2> VelocityTexture : register(t2);
// 前フレームの可視灯リスト(MegaLightsVisibleLights.hlsl が書いたもの)
StructuredBuffer<uint> VisibleLights : register(t3);

RWStructuredBuffer<uint> TilePool : register(u0);

// 走査するライト数の上限。groupshared配列のサイズに使うためコンパイル時定数である必要がある。
// **C++側 KurenaiEngine3D.cpp の kMaxLights と必ず同じ値にすること**
static const uint kMegaLightsMaxLights = 1024u;

// 無効な候補スロットに入れるライト番号
static const uint kInvalidLightIndex = 0xFFFFFFFFu;

groupshared uint gsMinDepthBits;
groupshared uint gsMaxDepthBits;
// タイルを代表する速度。最も手前のサーフェスのものを採る(TAAの速度ディレーションと同じ考え方)。
// タイルが深度の段差をまたぐとき、平均には意味が無い
groupshared float2 gsTileVelocity;
// ライトごとの重み。届かない灯は0
groupshared float gsWeight[kMegaLightsMaxLights];
// スレッドごとの部分和と、届いた灯の数
groupshared float gsPartialSum[kTileThreadCount];
groupshared uint gsPartialCount[kTileThreadCount];

// PCG系の整数ハッシュ(RTShadow.hlsl / RTAO.hlsl と同じもの)
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

float UintToUnitFloat(uint x)
{
    return float(x) * 2.3283064365e-10f; // uintの最大値で割って[0,1)へ
}

[numthreads(16, 16, 1)]
void CSMain(
    uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID, uint groupIndex : SV_GroupIndex)
{
    const uint tileCountX = TileParams.x;
    const uint tileCountY = TileParams.y;
    const uint lightCount = min(TileParams.z, kMegaLightsMaxLights);
    const uint candidateCount = TileParams.w;

    if (groupID.x >= tileCountX || groupID.y >= tileCountY)
    {
        return;
    }

    const uint tileIndex = groupID.y * tileCountX + groupID.x;
    const uint tileBase = MegaLightsTilePoolBase(groupID.xy, tileCountX, candidateCount);

    if (groupIndex == 0u)
    {
        gsMinDepthBits = 0xFFFFFFFFu;
        gsMaxDepthBits = 0u;
        gsTileVelocity = float2(0.0f, 0.0f);
    }
    GroupMemoryBarrierWithGroupSync();

    // --- タイル内の深度範囲を求める(LightCulling.hlsl と同じ手順) ---
    const int2 tilePixelOrigin = int2(groupID.xy * kTileSize);
    const int2 pixel = tilePixelOrigin + int2(groupThreadID.xy);
    if (all(pixel >= int2(0, 0)) && all(pixel < int2(RenderSize.xy)))
    {
        const float depth = DepthTexture.Load(int3(pixel, 0));
        if (depth > 0.0f)
        {
            const uint depthBits = asuint(depth);
            InterlockedMin(gsMinDepthBits, depthBits);
            InterlockedMax(gsMaxDepthBits, depthBits);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // タイル全体が背景ならライトは1灯も要らない。
    // 【必ずヘッダと全スロットを書くこと】RHIにUAVのクリアが無いため、書かずにreturnすると
    // 前フレームの残骸が残り、読み手が存在しない候補を引く
    if (gsMaxDepthBits == 0u)
    {
        if (groupIndex == 0u)
        {
            TilePool[tileBase + 0u] = asuint(0.0f);
            TilePool[tileBase + 1u] = 0u;
            TilePool[tileBase + 2u] = 0u;
            // 可視灯リストは使っていない(読み手はここで早期に打ち切るが、約束は守る)
            TilePool[tileBase + 3u] = kInvalidLightIndex;
            // 深度スラブも書く。読み手(空間再利用のMIS)は有効候補数0で先に打ち切るが、
            // 「書かずにreturnしない」という約束をここでも守る
            TilePool[tileBase + 4u] = asuint(0.0f);
            TilePool[tileBase + 5u] = asuint(0.0f);
        }
        for (uint slot = groupIndex; slot < candidateCount; slot += kTileThreadCount)
        {
            TilePool[tileBase + kMegaLightsTilePoolHeader + 2u * slot + 0u] = kInvalidLightIndex;
            TilePool[tileBase + kMegaLightsTilePoolHeader + 2u * slot + 1u] = asuint(0.0f);
        }
        return;
    }

    // --- タイルを代表する速度を拾う(可視灯リストの再投影に使う) ---
    // 最も手前のサーフェスを持つ画素の速度を採る。同じ深度の画素が複数あれば競合するが、
    // 深度が同じなら速度もほぼ同じなので、どれが勝っても構わない
    if (VisibleListParams.x != 0u && VisibleListParams.y != 0u &&
        all(pixel >= int2(0, 0)) && all(pixel < int2(RenderSize.xy)))
    {
        if (asuint(DepthTexture.Load(int3(pixel, 0))) == gsMaxDepthBits)
        {
            gsTileVelocity = VelocityTexture.Load(int3(pixel, 0)).rg;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Reverse-Zなので「深度値が大きい=手前」
    const float nearestViewZ = TileViewZFromDepth(asfloat(gsMaxDepthBits), ProjParams.z, ProjParams.w);
    const float farthestViewZ = TileViewZFromDepth(asfloat(gsMinDepthBits), ProjParams.z, ProjParams.w);

    const TileFrustum frustum = MakeTileFrustumFromPixelOrigin(
        tilePixelOrigin, RenderSize.xy, ProjParams.x, ProjParams.y, nearestViewZ, farthestViewZ);

    float3 aabbMin;
    float3 aabbMax;
    TileViewSpaceAABBFromPixelOrigin(
        tilePixelOrigin, RenderSize.xy, ProjParams.x, ProjParams.y,
        nearestViewZ, farthestViewZ, aabbMin, aabbMax);

    // --- 各ライトの重みを求める ---
    // スレッドgroupIndexが groupIndex, groupIndex+256, ... 番のライトを担当する
    float partialSum = 0.0f;
    uint partialCount = 0u;
    for (uint lightIndex = groupIndex; lightIndex < lightCount; lightIndex += kTileThreadCount)
    {
        const GPULight light = Lights[lightIndex];

        // 【重みの式は TileLightCulling.hlsli に1本だけ置いてある】読み手側の
        // 確率的バイリニア参照(MegaLightsInitialSample.hlsl)が、隣のタイルについて
        // **同じ関数**を呼んで q_j(y) を組み立てる。ここへ式を書き戻してはいけない
        float3 viewCenter;
        float radius;
        ComputeLightBoundingSphere(light, View, viewCenter, radius);
        const float weight =
            TileLightCandidateWeight(light, viewCenter, radius, frustum, aabbMin, aabbMax);
        if (weight > 0.0f)
        {
            partialSum += weight;
            partialCount += 1u;
        }
        gsWeight[lightIndex] = weight;
    }
    gsPartialSum[groupIndex] = partialSum;
    gsPartialCount[groupIndex] = partialCount;
    GroupMemoryBarrierWithGroupSync();

    // --- 部分和をまとめる(木状の縮約) ---
    for (uint stride = kTileThreadCount / 2u; stride > 0u; stride >>= 1u)
    {
        if (groupIndex < stride)
        {
            gsPartialSum[groupIndex] += gsPartialSum[groupIndex + stride];
            gsPartialCount[groupIndex] += gsPartialCount[groupIndex + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    const float sumW = gsPartialSum[0];
    const uint reachableCount = gsPartialCount[0];

    // --- 前フレームの可視灯リストを再投影して引き当てる(提案分布の第3成分) ---
    //
    // 【再投影が外れても偏らない】リストは提案分布を可視な灯へ*寄せる*ためのもので、
    // 定義域そのものは一様枝(a>0)が押さえている。外れたリストを引いても
    // 「効率の悪い提案」になるだけで期待値は動かない。だから妥当性の判定は要らず、
    // UE と同じく範囲外は最寄りのタイルへ丸める(clamp がその役をする)。
    //
    // 【引き当てた場所をヘッダへ書き残す ―― 読み手に同じ式を書かせない】
    // 割り戻す側(MegaLightsInitialSample.hlsl)は、ここで実際に使ったリストと
    // **同じもの**を見なければ、割り戻しが実際の抽出確率と食い違って静かに偏る。
    // 再投影の式を両側に書くと片方だけ直す事故が起きるので、結果の添字を
    // プールのヘッダ[base+3](従来は予約・読み手なし)へ書いて唯一の出所にする
    uint listBase = kInvalidLightIndex;
    uint listLength = 0u;
    float listMix = 0.0f;
    if (VisibleListParams.x != 0u && VisibleListParams.y != 0u && sumW > 0.0f &&
        asfloat(VisibleListParams.z) > 0.0f)
    {
        // タイル中心を前フレームへ送る(TAAと同じ引き方: historyUv = uv - velocity)
        const float2 centerPixel = float2(tilePixelOrigin) + float(kTileSize) * 0.5f;
        const float2 historyPixel = centerPixel - gsTileVelocity * float2(RenderSize.xy);
        const int2 historyTile = int2(floor(historyPixel / float(kTileSize)));
        const uint2 clampedTile = uint2(clamp(
            historyTile, int2(0, 0), int2(int(tileCountX) - 1, int(tileCountY) - 1)));

        const uint candidateBase = MegaLightsVisibleListBase(clampedTile, tileCountX);
        // 【実行時の容量でクランプしない】読み手(Initial)と同じ1つの関数を通す。
        // 容量を下げた直後は前の容量で書かれた長さが入っており、
        // 片方だけ切り詰めると提案確率の分母 L が食い違って静かに偏る
        const uint length = MegaLightsVisibleListLength(VisibleLights, candidateBase);
        if (length > 0u)
        {
            listBase = candidateBase;
            listLength = length;
            listMix = asfloat(VisibleListParams.z);
        }
    }

    // --- K個の候補を抽出する(一様枝と重み枝の混合) ---
    // 【重みだけで引いてはいけない】距離減衰は光源のそばで発散するため、重みに比例した
    // 抽出だけだと1灯が K スロットを独占し、その灯が寄与0になる画素が何フレーム待っても
    // 他の灯を引けなくなる(理由と実測は MegaLightsCommon.hlsli の混合率の定義を参照)。
    // どちらの枝で選ばれても、1スロットの抽出確率は
    //   p = 混合率 / 届いた灯数 + (1 - 混合率) * w_i / SumW
    // で、Initial 側はこの同じ式で割り戻す。
    // 【逆CDF法を使う】リザーバサンプリングでも同じ分布が得られるが、こちらは
    // 抽出確率が厳密に成り立ち、スレッド間の縮約も要らない。
    // K本のスレッドがそれぞれ独立に歩く(復元抽出。重複は許容する)
    for (uint slot = groupIndex; slot < candidateCount; slot += kTileThreadCount)
    {
        uint pickedIndex = kInvalidLightIndex;
        float pickedWeight = 0.0f;
        // リスト枝は「届かない灯を引いたら空振り」で終わってよい。下の丸め対策の保険を
        // そこへ効かせてしまうと、空振りのはずのスロットに別の灯が入って
        // 割り戻しと食い違う(= 静かなバイアス)。枝を見分けるための印
        bool listBranch = false;

        if (sumW > 0.0f)
        {
            const uint seed = HashUint(tileIndex * 0x9E3779B9u + slot * 0x85EBCA6Bu + PoolParams.x * 0xC2B2AE35u);
            const float mixRandom = UintToUnitFloat(HashUint(seed ^ 0x7F4A7C15u));

            if (mixRandom < kMegaLightsUniformMixFraction && reachableCount > 0u)
            {
                // 一様枝: 届いた灯(重みが正の灯)の中から番号で一様に選ぶ
                const uint targetOrdinal = min(
                    (uint)(UintToUnitFloat(HashUint(seed ^ 0x94D049BBu)) * float(reachableCount)),
                    reachableCount - 1u);
                uint ordinal = 0u;
                [loop]
                for (uint i = 0u; i < lightCount; ++i)
                {
                    if (gsWeight[i] > 0.0f)
                    {
                        if (ordinal == targetOrdinal)
                        {
                            pickedIndex = i;
                            pickedWeight = gsWeight[i];
                            break;
                        }
                        ++ordinal;
                    }
                }
            }
            else if (listLength > 0u && UintToUnitFloat(HashUint(seed ^ 0x2545F491u)) < listMix)
            {
                // リスト枝: 前フレームに可視だった灯から番号で一様に選ぶ。
                // 【種を目的ごとに別の定数でハッシュしている】ここは乱数の*列*ではないので、
                // 枝を1つ足しても既存の一様枝・重み枝の抽選はビット単位で変わらない
                // (混合率0のときに出力がビット同一である根拠)
                listBranch = true;
                const uint ordinal = min(
                    (uint)(UintToUnitFloat(HashUint(seed ^ 0xB7E15163u)) * float(listLength)),
                    listLength - 1u);
                const uint candidate = VisibleLights[listBase + kMegaLightsVisibleListHeader + ordinal];
                // 【このタイルへ届かない灯は捨てる ―― 定義域を広げてはいけない】
                // リストは1フレーム古く、載っている灯が今フレームもこのタイルへ届くとは限らない。
                // 届かない灯を候補にすると候補集合が候補プールの定義域を超え、
                // 空間再利用のMIS(LightInTileDomain)の前提が崩れる。
                // ここで捨てた場合このスロットは無効のまま = 「空振り」で、
                // 割り戻す側もその確率ぶんを込みで数えているので偏らない
                if (candidate < lightCount && gsWeight[candidate] > 0.0f)
                {
                    pickedIndex = candidate;
                    pickedWeight = gsWeight[candidate];
                }
            }
            else
            {
                // 重み枝: 逆CDF法
                const float target = UintToUnitFloat(HashUint(seed)) * sumW;
                float accumulated = 0.0f;
                [loop]
                for (uint i = 0u; i < lightCount; ++i)
                {
                    accumulated += gsWeight[i];
                    if (accumulated > target)
                    {
                        pickedIndex = i;
                        pickedWeight = gsWeight[i];
                        break;
                    }
                }
            }

            // 浮動小数の丸めで最後まで超えなかった場合の保険。重みが正の最後の灯を採る
            // (ここで無効のまま返すと、そのスロットぶんの寄与が黙って欠ける)。
            // 【リスト枝には効かせない】あちらの無効は丸め誤差ではなく
            // 「届かない灯を引いた」という正規の空振りで、埋めると割り戻しと食い違う
            if (pickedIndex == kInvalidLightIndex && !listBranch)
            {
                [loop]
                for (uint j = lightCount; j > 0u; --j)
                {
                    if (gsWeight[j - 1u] > 0.0f)
                    {
                        pickedIndex = j - 1u;
                        pickedWeight = gsWeight[j - 1u];
                        break;
                    }
                }
            }
        }

        TilePool[tileBase + kMegaLightsTilePoolHeader + 2u * slot + 0u] = pickedIndex;
        TilePool[tileBase + kMegaLightsTilePoolHeader + 2u * slot + 1u] = asuint(pickedWeight);
    }

    if (groupIndex == 0u)
    {
        TilePool[tileBase + 0u] = asuint(sumW);
        TilePool[tileBase + 1u] = reachableCount;
        TilePool[tileBase + 2u] = (sumW > 0.0f) ? candidateCount : 0u;
        // 提案分布の第3成分に使った可視灯リストの先頭添字(0xFFFFFFFF で「使わなかった」)。
        // 読み手はこれをそのまま使う ―― 再投影の式を二度書かないための唯一の出所
        TilePool[tileBase + 3u] = listBase;
        // 空間再利用のMIS重みが、隣のタイルの錐台を組み立て直すのに使う
        TilePool[tileBase + 4u] = asuint(nearestViewZ);
        TilePool[tileBase + 5u] = asuint(farthestViewZ);
    }
}
