#pragma once

#include <cstdint>
#include <vector>

#include <DirectXMath.h>

#include "Assets/Scene.h"

#include "GeometryDrawTypes.h"

// このフレームの描画リスト。インスタンシングのバッチと、描画単位を並べる作業領域を持つ。
//
// 【持ち主は1つだけにすること】作業領域(Scratch)が1本しか無いのは、パスが順に実行される
// ことを前提に確保のやり直しを避けるため。群ごとに持たせると入れ子の検査
// (Rendering/GeometryDrawLoop.h)が効かなくなり、内側の列挙が外側の列挙対象を
// 書き換える事故を検出できなくなる。
namespace Kurenai::Rendering
{
    // インスタンシングで1体ぶんの変換を渡すレコード。
    // Shaders/3D/ObjectConstants.hlsli の struct ModelInstanceRecord と
    // **バイト単位で一致させること**(144バイト。ずれると全インスタンスが見当違いの場所へ飛ぶ)
    struct alignas(16) GPUModelInstance
    {
        DirectX::XMFLOAT4X4 World;
        DirectX::XMFLOAT4X4 NormalMatrix;
        float TangentSignFlip;
        float Padding[3];
    };
    static_assert(sizeof(GPUModelInstance) == 144, "GPUModelInstanceはHLSL側と同じ144バイトであること");

    // バッチのグループ化キー。ワインディング(IsMirrored)と水面(IsWater)はパイプライン
    // ステートが分かれるため、違うものを同じドローへまとめてはいけない
    struct InstanceGroupKey
    {
        const Assets::Model* Model = nullptr;
        bool IsMirrored = false;
        bool IsWater = false;
        bool operator==(const InstanceGroupKey& other) const
        {
            return Model == other.Model && IsMirrored == other.IsMirrored && IsWater == other.IsWater;
        }
    };

    struct InstanceBatch
    {
        // このバッチが描く段。同じ段を選んだインスタンスだけをまとめる
        const Assets::Model* Model = nullptr;
        // SceneGPUResources::ModelInstanceBuffer の中の先頭位置。頂点シェーダーは
        // ModelInstances[InstanceBase + SV_InstanceID] を読む
        uint32_t InstanceBase = 0;
        uint32_t InstanceCount = 0;
        // ワインディングと水面の別はパイプラインステートで分かれるため、
        // 違うものを1つのドローへまとめてはいけない(まとめると片方が裏面として全部捨てられる)
        bool IsMirrored = false;
        bool IsWater = false;
        // 構成インスタンスのワールドAABBの包絡。パスごとのフラスタム判定に使う
        float WorldBoundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float WorldBoundsMax[3] = { 0.0f, 0.0f, 0.0f };
        // 代表インスタンスのシーン内番号(バッチの先頭)。IsMirrored/IsWaterはバッチ内で
        // 同一なので、定数バッファを作るのに1体を代表として使える
        size_t RepresentativeIndex = 0;
    };

    struct SceneDrawList
    {
        // バッチの一覧は「どの段を描くパスか」で2組に分かれる。
        // 変換そのものはどちらでも同じだが、**まとめられる相手が違う** ――
        // G-Buffer は各インスタンスがそのフレームに選んだ段、シャドウとプローブは常に
        // 最も粗い段(GetCoarsestLOD)を描くため、同じ組では括れない
        std::vector<InstanceBatch> BatchesCurrentLOD;   // 深度プリパス / G-Buffer / 平面反射
        std::vector<InstanceBatch> BatchesCoarsestLOD;  // シャドウ / 反射プローブ
        // インスタンスがどちらの組でバッチに入ったか。パスの個別ループはここが立っているものを飛ばす
        std::vector<uint8_t> BatchedCurrentLOD;
        std::vector<uint8_t> BatchedCoarsestLOD;
        // アップロード用の作業領域(毎フレーム作り直す。確保のやり直しを避けるため持っておく)
        std::vector<GPUModelInstance> InstanceRecords;

        // 統計。**フラスタムカリングとは別建てにする** ―― 「バッチが0のまま」は
        // 「まとめられる相手がいない」のか「一度も実行されていない」のかを区別できないため、
        // まとめた数と減らせたドロー数の両方を出す
        uint32_t InstancedBatchCount = 0;
        uint32_t InstancedInstanceCount = 0;

        // 共通ループの出力先。パスは順に実行されるので1本を使い回してよい(確保のやり直しを避ける)。
        // **パスのラムダより長生きする必要がある**ため、ローカル変数ではなくここに置く
        mutable std::vector<InstanceDrawUnit> Scratch;
        // 上が1本しかないことを守るための旗。入れ子で列挙すると内側が外側の列挙対象を
        // 書き換えてしまう。検査の中身はRendering/GeometryDrawLoop.hにある
        mutable bool ScratchInUse = false;

        // 1バッチの上限。上限が無いと「街灯を市街全域に5000個」のようなグループが
        // 1つの巨大AABBになり、どのパスからも一度も間引かれなくなる。
        // グループ内を空間セルでソートしてから刻むので、バッチは局所的にまとまる
        static constexpr uint32_t kMaxInstancesPerBatch = 128;

        // このフレームの描画単位を組み立てる。coarsestLOD が真ならシャドウ/プローブ用の組、
        // 偽なら深度プリパス/G-Buffer/平面反射用の組を使う。
        // シーンの全インスタンスがちょうど1回ずつ現れる(バッチに入ったものはバッチとして)
        void GetInstanceDrawUnits(
            const Assets::Scene& scene, bool coarsestLOD, std::vector<InstanceDrawUnit>& outUnits) const;
        // インスタンシングのバッチを使わないパス(DDGI / 半透明 / ソフトウェアラスタライザ)向けに、
        // シーンの全インスタンスを単体の描画単位として詰める。列挙順はscene.Instancesの並びのまま
        void BuildSingleInstanceDrawUnits(
            const Assets::Scene& scene, std::vector<InstanceDrawUnit>& outUnits) const;
    };
}
