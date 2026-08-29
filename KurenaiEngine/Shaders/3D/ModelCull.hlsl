// モデル単位のGPUカリング(Stage 5-3)。
//
// 1スレッドが1つの描画候補(= PSOとLODの段まで確定したドロー1件)を受け持ち、
// 視錐台とHi-Zで判定して、生き残ったものの ExecuteIndirect 引数を詰める。
//
// 【なぜGPUでやるのか】Hi-Z判定はGPU上の深度を読む。CPUで同じことをするには
// 深度を読み戻すことになり、フレームを直列化してしまう。判定をGPUへ置き、
// 描画発行もGPU(ExecuteIndirect)にすれば読み戻しが要らなくなる。
//
// 【区画(Region)に分けて詰める理由】1回のExecuteIndirectで切り替えられるのは
// 引数に含めたルートパラメータだけで、PSOは切り替えられない。ミラーリングの有無・
// 深度プリパスの不透明/カットアウトはPSOが違うため、行き先の配列を分けて
// PSOごとに1回ずつExecuteIndirectする。区画の割り当てはCPU側(ModelCullRegion)。

#include "Frustum.hlsli"
#include "HiZCull.hlsli"

// 引数1件ぶんのバイト数。
// 【RHI::IRHICommandList::kDispatchMeshIndirectArgStride と一致させること】
//   +0  : このドローが使う定数バッファ(b1)のGPU仮想アドレス(64bit)
//   +8  : DispatchMeshのスレッドグループ数X/Y/Z
//   +20 : 詰め物(次の要素のアドレスを8バイト境界に載せるため)
#define KURENAI_INDIRECT_ARG_STRIDE 24

cbuffer ModelCullConstants : register(b0)
{
    // 視錐台判定に使う。**今フレームの**ビュー射影行列(CPU側の判定と揃える)
    float4x4 CullViewProj;
    // Hi-Z判定に使う。**そのHi-Zの元になった深度を描いた行列。**
    // 深度プリパスから作る経路では今フレームの行列、Hi-ZをG-Bufferより後に作る
    // 経路(プリパス無効時)では前フレームの行列が入る。
    // 取り違えてもコンパイルは通り絵もそれらしく出るので、CPU側が明示的に選ぶ
    float4x4 CullPrevViewProj;
    // x=**このディスパッチが受け持つ**候補数、y=Hi-Zのミップ段数、
    // z=オクルージョン判定の有効フラグ、
    // w=引数配列の先頭オフセット[バイト](手前は区画ごとの発行数が占める)
    uint4 CullParams;
    // x=区画1つぶんのバイト数、y=区画数、
    // z=このディスパッチが受け持つ候補の先頭番号、
    // w=統計([0..3])を数え始める候補番号。
    //
    // 【zとwが要る理由】Hi-Zを深度プリパスから作るとき、判定は2回に分かれる ――
    // 深度プリパスぶんはプリパスの前に(前フレームのHi-Zで)、G-Bufferぶんは
    // Hi-Zを作り終えてから(今フレームのHi-Zで)。候補配列は1本のままなので、
    // どこからどこまでを受け持つかを渡す。
    // **統計はG-Bufferぶんだけ数える** ―― 両方数えると同じモデルを2回数えることになり、
    // 「判定 1534」のようにモデル数の2倍が出てCPU側の数と単位が合わなくなる
    uint4 CullRegionParams;
    // xy=Hi-Zのミップ0の解像度[画素]、zw=未使用
    float4 CullHiZScreenParams;
    // x=AABBを膨らませる量[m](前フレームからのカメラ移動距離)、yzw=未使用。
    //
    // 【倍率ではなく加算なのはモデル単位だから】メッシュレットの球と違い、
    // モデルのAABBは1.1km四方にもなる。倍率で膨らませると桁で効きすぎて
    // 一度も間引かなくなるため、視差ずれの実量だけを足す
    float4 CullExpandParams;
};

// Assets::GpuModelCullInstance(48バイト)と1対1で対応。並びとサイズを一致させること
struct ModelCullInstance
{
    float3 BoundsMin;
    // この候補を描くのに要る増幅シェーダーのスレッドグループ数(DispatchMeshのX)
    uint GroupCount;
    float3 BoundsMax;
    // 出力先の区画番号(PSOごとに分かれる)
    uint RegionIndex;
    // このドローが使う ObjectConstants のGPU仮想アドレス(x=下位32bit、y=上位32bit)。
    // CPU側が定数バッファのリングへ書き込んだスロットのアドレスをそのまま持ってくる。
    // シェーダーは中身を解釈せず、生き残ったものだけを引数へ書き写す
    uint2 CbvAddress;
    uint2 Padding;
};

StructuredBuffer<ModelCullInstance> CullInstances : register(t0);
Texture2D<float> CullHiZTexture : register(t1);

// [0]=判定した数 [1]=視錐台で間引いた数 [2]=オクルージョンで間引いた数 [3]=生き残った数
// [4以降]=区画ごとの生き残り数(CPU側の照合とログ用)
RWStructuredBuffer<uint> CullCounters : register(u0);
// ExecuteIndirectへそのまま渡すバッファ。先頭に区画ごとの発行数が並び、
// CullParams.w から先が引数の配列(区画ごとに CullRegionParams.x バイトずつ)。
//
// 【構造化バッファではなくraw】D3D11がDRAWINDIRECT_ARGSと構造化を同時に指定できない
// 制約に合わせて、RHIのBufferUsage::IndirectArgsはrawで統一してある
RWByteAddressBuffer CullDrawArgs : register(u1);

#define KURENAI_MODEL_CULL_GROUP_SIZE 64

[numthreads(KURENAI_MODEL_CULL_GROUP_SIZE, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= CullParams.x)
    {
        return;
    }
    const uint index = CullRegionParams.z + dispatchThreadId.x;
    // 統計を数えるのはG-Bufferぶんの候補だけ。詳細はCullRegionParamsのコメント
    const bool countStats = (index >= CullRegionParams.w);

    const ModelCullInstance candidate = CullInstances[index];

    // 判定した数。**間引いた数だけでは、判定式が常に通しているのか
    // 本当に全部見えているのかを区別できない**(CPU側の統計と同じ理由)
    if (countStats)
    {
        InterlockedAdd(CullCounters[0], 1);
    }

    float4 planes[6];
    ExtractFrustumPlanes(CullViewProj, planes);
    if (!AabbInFrustumPlanes(planes, candidate.BoundsMin, candidate.BoundsMax))
    {
        if (countStats)
        {
            InterlockedAdd(CullCounters[1], 1);
        }
        return;
    }

    if (CullParams.z != 0)
    {
        // 1フレーム古いHi-Zで判定するときだけ、カメラ移動ぶんだけAABBを膨らませて
        // 視差のずれを保守側へ吸収する。今フレームのHi-Zならこの値は0で渡される
        // (膨らませると間引けるものを取りこぼすだけ)
        const float expand = CullExpandParams.x;
        if (IsAabbOccludedByHiZ(
                CullHiZTexture, CullPrevViewProj,
                candidate.BoundsMin - expand, candidate.BoundsMax + expand,
                CullHiZScreenParams.xy, CullParams.y))
        {
            if (countStats)
            {
                InterlockedAdd(CullCounters[2], 1);
            }
            return;
        }
    }

    if (countStats)
    {
        InterlockedAdd(CullCounters[3], 1);
    }

    // 描くものが無い候補(メッシュレットを持たない段など)は引数に載せない。
    // グループ数0のDispatchMeshは害こそ無いが、発行数を水増しして
    // 「何件描いたのか」の統計を狂わせる
    if (candidate.GroupCount == 0)
    {
        return;
    }

    // 詰める位置は区画ごとの原子加算で取る(順序は保証されないが、
    // 描画は順序に依存しない ―― 深度テストが前後関係を決める)
    const uint region = min(candidate.RegionIndex, CullRegionParams.y - 1);
    uint slot;
    CullDrawArgs.InterlockedAdd(region * 4, 1, slot);
    InterlockedAdd(CullCounters[4 + region], 1);

    const uint base =
        CullParams.w + region * CullRegionParams.x + slot * KURENAI_INDIRECT_ARG_STRIDE;
    CullDrawArgs.Store2(base, candidate.CbvAddress);
    CullDrawArgs.Store3(base + 8, uint3(candidate.GroupCount, 1, 1));
}
