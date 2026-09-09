#include "SceneDrawList.h"

// 描画リストの読み出し。持ち主を1つにする理由は SceneDrawList.h にある
namespace Kurenai::Rendering
{
    // 同じモデルを指すインスタンスを1回のDrawIndexedへまとめるバッチを作り直す。
    //
    // 【レンダーグラフの構築より前に1フレーム1回だけ呼ぶこと】UpdateModelLODが決めた段を読むので
    // その後、かつどのパスより前。パスごとに組み直すと、深度プリパスとG-Bufferが違うまとめ方をして
    // 同じ画素を別の経路で描くことになる。
    //
    // 【バッチに入れないもの】
    //   - まだ読み込まれていない段(ストリーミング中)
    //   - LOD切替のフェード中。DitherFadeはインスタンスごとに違い、定数バッファで渡す値なので
    //     1ドローにまとめられない。フェードは短時間で終わるので、そのあいだ個別に描けばよい
    //   - メッシュシェーダー経路に載るモデル。DispatchMeshにインスタンス数の概念が無い
    //   - まとめる相手がいないもの(1体だけのグループ)。この場合は従来とまったく同じ描画になる
    // このフレームの描画単位を組み立てる。バッチに入ったインスタンスはバッチとして1回、
    // 入らなかったものは1体ずつ現れる ―― 全インスタンスがちょうど1回ずつ現れることが要点で、
    // 取りこぼすと物が消え、二重に出すと同じ場所へ2回描いてZファイティングになる
    void SceneDrawList::GetInstanceDrawUnits(
        const Assets::Scene& scene, bool coarsestLOD, std::vector<InstanceDrawUnit>& outUnits) const
    {
        const std::vector<InstanceBatch>& batches =
            coarsestLOD ? BatchesCoarsestLOD : BatchesCurrentLOD;
        const std::vector<uint8_t>& batched =
            coarsestLOD ? BatchedCoarsestLOD : BatchedCurrentLOD;

        outUnits.clear();
        outUnits.reserve(scene.Instances.size());

        for (const InstanceBatch& batch : batches)
        {
            InstanceDrawUnit unit;
            // 代表はバッチの先頭。IsMirrored/IsWaterはバッチ内で同一(グループ化のキー)なので、
            // どれを代表にしても同じ値になる
            unit.Instance = &scene.Instances[batch.RepresentativeIndex];
            unit.InstanceIndex = batch.RepresentativeIndex;
            unit.Model = batch.Model;
            unit.InstanceBase = batch.InstanceBase;
            unit.InstanceCount = batch.InstanceCount;
            for (int axis = 0; axis < 3; ++axis)
            {
                unit.WorldBoundsMin[axis] = batch.WorldBoundsMin[axis];
                unit.WorldBoundsMax[axis] = batch.WorldBoundsMax[axis];
            }
            outUnits.push_back(unit);
        }

        for (size_t i = 0; i < scene.Instances.size(); ++i)
        {
            if (i < batched.size() && batched[i] != 0)
            {
                continue;   // バッチとして既に積んである
            }
            InstanceDrawUnit unit;
            unit.Instance = &scene.Instances[i];
            unit.InstanceIndex = i;
            unit.Model = nullptr;   // 段は呼び出し側が決める(フェード中は2段になる)
            unit.InstanceBase = 0;
            unit.InstanceCount = 1;
            for (int axis = 0; axis < 3; ++axis)
            {
                unit.WorldBoundsMin[axis] = scene.Instances[i].WorldBoundsMin[axis];
                unit.WorldBoundsMax[axis] = scene.Instances[i].WorldBoundsMax[axis];
            }
            outUnits.push_back(unit);
        }
    }

    void SceneDrawList::BuildSingleInstanceDrawUnits(
        const Assets::Scene& scene, std::vector<InstanceDrawUnit>& outUnits) const
    {
        // インスタンシングのバッチを使わないパス用に、シーンの全インスタンスを
        // 単体(InstanceCount==1)の描画単位として詰める。
        // **列挙順はscene.Instancesの並びそのもの** ―― 描画順が変わると、
        // 同じ絵でも中間バッファのバイト列が変わりうる
        outUnits.clear();
        outUnits.reserve(scene.Instances.size());
        for (size_t instanceIndex = 0; instanceIndex < scene.Instances.size(); ++instanceIndex)
        {
            const Assets::ModelInstance& instance = scene.Instances[instanceIndex];
            InstanceDrawUnit unit;
            unit.Instance = &instance;
            unit.InstanceIndex = instanceIndex;
            // 段は呼び出し側(共通ループ)がGetCurrentLOD/GetCoarsestLODで決める
            unit.Model = nullptr;
            unit.InstanceBase = 0;
            unit.InstanceCount = 1;
            for (int axis = 0; axis < 3; ++axis)
            {
                unit.WorldBoundsMin[axis] = instance.WorldBoundsMin[axis];
                unit.WorldBoundsMax[axis] = instance.WorldBoundsMax[axis];
            }
            outUnits.push_back(unit);
        }
    }
}
