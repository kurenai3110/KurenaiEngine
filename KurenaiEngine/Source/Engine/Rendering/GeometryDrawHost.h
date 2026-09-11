#pragma once

#include <cstdint>

#include "Assets/Scene.h"

#include "GeometryDrawTypes.h"
#include "SceneDrawList.h"
#include "../Scene/InstanceLODState.h"

// ジオメトリ描画の共通ループが要るものを1つにまとめた口。
//
// 【なぜ口を切るのか】共通ループはテンプレート(コールバックを間接呼び出しにしないため)で、
// 純粋仮想にはできない。かわりに**ループが要るものだけ**を参照で束ねたこの型を
// パス群へ渡す。パス群がエンジンの公開ヘッダを引かずに済むのが目的。
//
// 【段の決定だけは仮想で受ける】どの段を描くかはフェード中に2段へ割れる都合で
// エンジンが持っている。1インスタンスにつき1回しか呼ばないので間接呼び出しでよい。
namespace Kurenai::Rendering
{
    // 段の決定。実装はエンジンにある
    class ILODSelector
    {
    public:
        virtual ~ILODSelector() = default;

        // instanceIndex番目がこのフレームで描く段。フェード中はnullptrのことがある
        virtual const Assets::Model* GetCurrentLOD(size_t instanceIndex) const = 0;
        // 常に最も粗い段(シャドウ・プローブ用)
        virtual const Assets::Model* GetCoarsestLOD(const Assets::ModelInstance& instance) const = 0;
        // このフレームで重ねて描く段(フェード中は2段)。戻り値は段数
        virtual uint32_t GetLODDraws(size_t instanceIndex, Scene::LODDraw (&outDraws)[2]) const = 0;
        // メッシュシェーダー経路で描くか(バッチにまとめられない)
        virtual bool ShouldUseModelMeshletPath(
            const Assets::ModelInstance& instance, const Assets::Model& model) const = 0;
    };

    // 共通ループが読み書きするカウンタ。**フレーム先頭で0に戻すのは持ち主の仕事**
    struct GeometryDrawCounters
    {
        uint32_t& FrustumCullTested;
        uint32_t& FrustumCullCulled;
        uint32_t& MeshCullTested;
        uint32_t& MeshCullCulled;
    };

    // 共通ループが要るものを束ねた口。参照だけを持つので、**フレームより長く持たないこと**
    struct GeometryDrawHost
    {
        const Assets::Scene& Scene;
        SceneDrawList& DrawList;
        const ILODSelector& LOD;
        GeometryDrawCounters Counters;
        // メッシュ単位のカリングを行うか(設定)。descのMeshCullingとANDを取る
        bool MeshCullingEnabled = false;
    };
}
