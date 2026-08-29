// モデル単位のGPUカリング(Stage 5-3)。
//
// 1スレッドが1つの描画候補(= LODの段まで確定したモデルインスタンス1件)を受け持ち、
// 視錐台とHi-Zで判定して、生き残ったものの DispatchMesh 引数を詰める。
//
// 【なぜGPUでやるのか】Hi-Z判定はGPU上の深度を読む。CPUで同じことをするには
// 深度を読み戻すことになり、フレームを直列化してしまう。判定をGPUへ置き、
// 描画発行もGPU(ExecuteIndirect)にすれば読み戻しが要らなくなる。
//
// 【この段階では描画を発行しない】まずカウンタだけを埋めて、CPU側の判定と
// 突き合わせられるようにする。ExecuteIndirect へ繋ぐのはその後 ――
// いきなり繋ぐとGPUハングの切り分けができない。

#include "Frustum.hlsli"
#include "HiZCull.hlsli"

cbuffer ModelCullConstants : register(b0)
{
    // 視錐台判定に使う。**今フレームの**ビュー射影行列(CPU側の判定と揃える)
    float4x4 CullViewProj;
    // Hi-Z判定に使う。**そのHi-Zの元になった深度を描いた行列。**
    // 構築パスがG-Bufferより後に登録されている現状では前フレームのものになる
    float4x4 CullPrevViewProj;
    // x=候補数、y=Hi-Zのミップ段数、z=オクルージョン判定の有効フラグ、w=未使用
    uint4 CullParams;
    // xy=Hi-Zのミップ0の解像度[画素]、zw=未使用
    float4 CullHiZScreenParams;
    // x=AABBを膨らませる量[m](前フレームからのカメラ移動距離)、yzw=未使用。
    //
    // 【倍率ではなく加算なのはモデル単位だから】メッシュレットの球と違い、
    // モデルのAABBは1.1km四方にもなる。倍率で膨らませると桁で効きすぎて
    // 一度も間引かなくなるため、視差ずれの実量だけを足す
    float4 CullExpandParams;
};

// Assets::GpuModelCullInstance(32バイト)と1対1で対応。並びとサイズを一致させること
struct ModelCullInstance
{
    float3 BoundsMin;
    // この候補を描くのに要る増幅シェーダーのスレッドグループ数(DispatchMeshのX)
    uint GroupCount;
    float3 BoundsMax;
    // 描画側が定数を引くための通し番号。ExecuteIndirectへ繋ぐときにルート定数で渡す
    uint DrawIndex;
};

StructuredBuffer<ModelCullInstance> CullInstances : register(t0);
Texture2D<float> CullHiZTexture : register(t1);

// [0]=判定した数 [1]=視錐台で間引いた数 [2]=オクルージョンで間引いた数 [3]=生き残った数
RWStructuredBuffer<uint> CullCounters : register(u0);
// 生き残った候補の DispatchMesh 引数。xyz=スレッドグループ数、w=DrawIndex
RWStructuredBuffer<uint4> CullDrawArgs : register(u1);

#define KURENAI_MODEL_CULL_GROUP_SIZE 64

[numthreads(KURENAI_MODEL_CULL_GROUP_SIZE, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if (index >= CullParams.x)
    {
        return;
    }

    const ModelCullInstance candidate = CullInstances[index];

    // 判定した数。**間引いた数だけでは、判定式が常に通しているのか
    // 本当に全部見えているのかを区別できない**(CPU側の統計と同じ理由)
    InterlockedAdd(CullCounters[0], 1);

    float4 planes[6];
    ExtractFrustumPlanes(CullViewProj, planes);
    if (!AabbInFrustumPlanes(planes, candidate.BoundsMin, candidate.BoundsMax))
    {
        InterlockedAdd(CullCounters[1], 1);
        return;
    }

    if (CullParams.z != 0)
    {
        // 1フレーム古いHi-Zで判定するので、カメラ移動ぶんだけAABBを膨らませて
        // 視差のずれを保守側へ吸収する
        const float expand = CullExpandParams.x;
        if (IsAabbOccludedByHiZ(
                CullHiZTexture, CullPrevViewProj,
                candidate.BoundsMin - expand, candidate.BoundsMax + expand,
                CullHiZScreenParams.xy, CullParams.y))
        {
            InterlockedAdd(CullCounters[2], 1);
            return;
        }
    }

    // 生き残り。詰める位置は原子加算で取る(順序は保証されないが、
    // 描画は順序に依存しない ―― 深度テストが前後関係を決める)
    uint slot;
    InterlockedAdd(CullCounters[3], 1, slot);
    CullDrawArgs[slot] = uint4(candidate.GroupCount, 1, 1, candidate.DrawIndex);
}
