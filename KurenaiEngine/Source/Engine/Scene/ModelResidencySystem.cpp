#include "../KurenaiEngine3D.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "Core/Logger.h"

// モデルのLOD選択・常駐(ストリーミング)・インスタンスのバッチ化と、
// レイトレーシング加速構造の作り直し。
// KurenaiEngine3D のメンバ関数のまま、翻訳単位だけをここへ分けている
// (宣言は KurenaiEngine3D.h のまま)
namespace Kurenai
{
    void KurenaiEngine3D::UpdateSceneStreaming()
    {
        // .ksceneのホットリロード。実際の読み込みは下の既存の経路にそのまま乗せる
        UpdateSceneHotReloadWatch();

        // --- 出来上がったシーンがあれば取り込む ---
        std::unique_ptr<LoadedScene> loaded;
        {
            std::lock_guard<std::mutex> lock(m_LoadedSceneMutex);
            loaded = std::move(m_LoadedScene);
        }
        if (loaded)
        {
            ApplyLoadedScene(*loaded);
            m_SceneLoadInFlight = false;
        }

        // --- 保留中の切り替え要求をLoaderスレッドへ発注する ---
        // 読み込み中は発注しない(最後の要求はm_PendingSceneRequestに残るので取りこぼさない)
        if (m_PendingSceneRequest < 0 || m_SceneLoadInFlight)
        {
            return;
        }

        const size_t sceneIndex = static_cast<size_t>(m_PendingSceneRequest);
        m_PendingSceneRequest = -1;

        // 【WaitForGPUIdleより前に止める】テクスチャストリーミングのワーカーは
        // 旧シーンのIRHITexture*を掴んだままGPUリソースを作っている。旧シーンを手放す前に
        // 必ず止めて、走っている要求を捨てる
        m_TextureStreaming.Reset();

        // 旧シーンのGPUリソースを手放す前に、GPUが旧シーンを参照するコマンド(直前まで提出されていた
        // 描画コマンド)の実行を終えるまで待つ。特にDX12はCPUがGPU完了を待たずに次フレームの記録を
        // 始める多重バッファリング設計のため、これを省くとGPUがまだ読んでいるバッファ/テクスチャを
        // 解放してしまう(詳細はIRHIDevice::WaitForGPUIdleのコメント参照)。
        // このフレームのGPUコマンドはまだ1つも積んでいないため、待ち時間は前フレームぶんだけで済む
        m_Device->WaitForGPUIdle();

        // 読み込み開始と同時に旧シーンを手放す。読み込み完了まで待ってから捨てると新旧の
        // GPUリソースが同時に載ってVRAMがほぼ2倍になるため、先に空にする方を選んでいる。
        // その代わり読み込み中はシーンが描かれない(UIとスカイボックスのみになる)
        RetiredAssets retired;
        retired.Scene = std::move(m_Scene);
        retired.RaytracingScene = std::move(m_RaytracingScene);
        retired.MeshLightScene = std::move(m_MeshLightScene);
        m_Scene = Assets::Scene{};
        m_RaytracingScene = Assets::RaytracingScene{};
        RetireAssets(std::move(retired));

        {
            std::lock_guard<std::mutex> lock(m_LoadRequestMutex);
            m_LoadRequestSceneIndex = static_cast<int>(sceneIndex);
        }
        m_LoadRequestCV.notify_one();
        m_SceneLoadInFlight = true;
        // 進捗表示にシーン名を出すために、いま読ませているシーンを控える
        m_SceneLoadingIndex = sceneIndex;
    }

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
    void KurenaiEngine3D::GetInstanceDrawUnits(bool coarsestLOD, std::vector<InstanceDrawUnit>& outUnits) const
    {
        const std::vector<InstanceBatch>& batches =
            coarsestLOD ? m_InstanceBatchesCoarsestLOD : m_InstanceBatchesCurrentLOD;
        const std::vector<uint8_t>& batched =
            coarsestLOD ? m_InstanceBatchedCoarsestLOD : m_InstanceBatchedCurrentLOD;

        outUnits.clear();
        outUnits.reserve(m_Scene.Instances.size());

        for (const InstanceBatch& batch : batches)
        {
            InstanceDrawUnit unit;
            // 代表はバッチの先頭。IsMirrored/IsWaterはバッチ内で同一(グループ化のキー)なので、
            // どれを代表にしても同じ値になる
            unit.Instance = &m_Scene.Instances[batch.RepresentativeIndex];
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

        for (size_t i = 0; i < m_Scene.Instances.size(); ++i)
        {
            if (i < batched.size() && batched[i] != 0)
            {
                continue;   // バッチとして既に積んである
            }
            InstanceDrawUnit unit;
            unit.Instance = &m_Scene.Instances[i];
            unit.InstanceIndex = i;
            unit.Model = nullptr;   // 段は呼び出し側が決める(フェード中は2段になる)
            unit.InstanceBase = 0;
            unit.InstanceCount = 1;
            for (int axis = 0; axis < 3; ++axis)
            {
                unit.WorldBoundsMin[axis] = m_Scene.Instances[i].WorldBoundsMin[axis];
                unit.WorldBoundsMax[axis] = m_Scene.Instances[i].WorldBoundsMax[axis];
            }
            outUnits.push_back(unit);
        }
    }

    void KurenaiEngine3D::BuildInstanceBatches(RHI::IRHICommandList* commandList)
    {
        m_InstanceBatchesCurrentLOD.clear();
        m_InstanceBatchesCoarsestLOD.clear();
        m_ModelInstanceRecords.clear();
        m_InstanceBatchedCurrentLOD.assign(m_Scene.Instances.size(), 0u);
        m_InstanceBatchedCoarsestLOD.assign(m_Scene.Instances.size(), 0u);
        m_InstancedBatchCount = 0;
        m_InstancedInstanceCount = 0;

        if (!m_GeometrySettings.InstancingEnabled || m_Scene.Instances.empty() || !m_ModelInstanceBuffer)
        {
            return;
        }

        // グループ化のキー。ワインディング(IsMirrored)と水面(IsWater)はパイプラインステートが
        // 分かれるため、違うものを同じドローへまとめてはいけない
        struct GroupKey
        {
            const Assets::Model* Model;
            bool IsMirrored;
            bool IsWater;
            bool operator==(const GroupKey& other) const
            {
                return Model == other.Model && IsMirrored == other.IsMirrored && IsWater == other.IsWater;
            }
        };

        // キーごとのインスタンス番号。シーンの並び順で走査するので、同じシーンなら毎フレーム同じ順になる
        // (順序が揺れるとフレーム間でバッチの内容が変わり、A/B比較の再現性が落ちる)
        std::vector<std::pair<GroupKey, std::vector<size_t>>> groups;

        // 1つの組(段の選び方)ぶんのバッチを作る。
        // modelOf: そのインスタンスがこの組で描く段を返す。nullptrならこの組の対象外
        const auto buildFor =
            [this, &groups](
                const std::function<const Assets::Model*(size_t)>& modelOf,
                std::vector<InstanceBatch>& outBatches, std::vector<uint8_t>& outBatched)
        {
            groups.clear();
            for (size_t i = 0; i < m_Scene.Instances.size(); ++i)
            {
                const Assets::ModelInstance& instance = m_Scene.Instances[i];
                const Assets::Model* const model = modelOf(i);
                if (!model)
                {
                    continue;
                }
                // メッシュシェーダー経路はDispatchMeshで描くのでまとめられない
                if (ShouldUseModelMeshletPath(instance, *model))
                {
                    continue;
                }

                const GroupKey key{ model, instance.IsMirrored, instance.IsWater };
                bool found = false;
                for (auto& group : groups)
                {
                    if (group.first == key)
                    {
                        group.second.push_back(i);
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    groups.push_back({ key, { i } });
                }
            }

            // 【空間セルでソートしてから刻む】上限なしで1バッチにすると、広く散らばった
            // グループが1つの巨大AABBになり、どのパスからも一度も間引かれなくなる。
            // セルの幅はシーン対角の1/64を目安にする(バッチの粒度がシーンの規模に追随する)
            const float diagonalX = m_Scene.BoundsMax[0] - m_Scene.BoundsMin[0];
            const float diagonalY = m_Scene.BoundsMax[1] - m_Scene.BoundsMin[1];
            const float diagonalZ = m_Scene.BoundsMax[2] - m_Scene.BoundsMin[2];
            const float diagonal =
                std::sqrt(diagonalX * diagonalX + diagonalY * diagonalY + diagonalZ * diagonalZ);
            const float cellSize = std::max(diagonal / 64.0f, 1.0f);

            for (auto& group : groups)
            {
                if (group.second.size() < 2)
                {
                    // まとめる相手がいない。従来どおり個別に描く(コマンド列は今までと同一)
                    continue;
                }

                std::stable_sort(
                    group.second.begin(), group.second.end(),
                    [this, cellSize](size_t a, size_t b)
                    {
                        const auto cell = [this, cellSize](size_t index, int axis)
                        {
                            const float center =
                                (m_Scene.Instances[index].WorldBoundsMin[axis]
                                 + m_Scene.Instances[index].WorldBoundsMax[axis]) * 0.5f;
                            return static_cast<int64_t>(std::floor(center / cellSize));
                        };
                        // Z→X→Y の順に見る。格子状の配置ではこれで行ごとにまとまる
                        const int axes[3] = { 2, 0, 1 };
                        for (const int axis : axes)
                        {
                            const int64_t ca = cell(a, axis);
                            const int64_t cb = cell(b, axis);
                            if (ca != cb)
                            {
                                return ca < cb;
                            }
                        }
                        return a < b;
                    });

                for (size_t offset = 0; offset < group.second.size(); offset += kMaxInstancesPerBatch)
                {
                    const size_t count = std::min<size_t>(kMaxInstancesPerBatch, group.second.size() - offset);
                    if (count < 2)
                    {
                        // 刻んだ余りが1体だけになった場合。まとめる意味が無いので個別へ回す
                        continue;
                    }

                    InstanceBatch batch;
                    batch.Model = group.first.Model;
                    batch.IsMirrored = group.first.IsMirrored;
                    batch.IsWater = group.first.IsWater;
                    batch.InstanceBase = static_cast<uint32_t>(m_ModelInstanceRecords.size());
                    batch.InstanceCount = static_cast<uint32_t>(count);
                    batch.RepresentativeIndex = group.second[offset];
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        batch.WorldBoundsMin[axis] = (std::numeric_limits<float>::max)();
                        batch.WorldBoundsMax[axis] = std::numeric_limits<float>::lowest();
                    }

                    for (size_t k = 0; k < count; ++k)
                    {
                        const size_t instanceIndex = group.second[offset + k];
                        const Assets::ModelInstance& instance = m_Scene.Instances[instanceIndex];

                        GPUModelInstance record{};
                        record.World = instance.World;
                        record.NormalMatrix = instance.NormalMatrix;
                        record.TangentSignFlip = instance.TangentSignFlip;
                        m_ModelInstanceRecords.push_back(record);

                        for (int axis = 0; axis < 3; ++axis)
                        {
                            batch.WorldBoundsMin[axis] =
                                std::min(batch.WorldBoundsMin[axis], instance.WorldBoundsMin[axis]);
                            batch.WorldBoundsMax[axis] =
                                std::max(batch.WorldBoundsMax[axis], instance.WorldBoundsMax[axis]);
                        }
                        outBatched[instanceIndex] = 1u;
                    }

                    outBatches.push_back(batch);
                }
            }
        };

        // 組1: そのフレームに選ばれた段(深度プリパス / G-Buffer / 平面反射)。
        // フェード中(段が2つ)は個別に描くのでバッチへ入れない
        buildFor(
            [this](size_t i) -> const Assets::Model*
            {
                LODDraw draws[2];
                if (GetLODDraws(i, draws) != 1)
                {
                    return nullptr;
                }
                // 【GetCurrentLODと一致するときだけまとめる】GetLODDrawsは、フェード中でも
                // 片方の段が未ストリーミングなら「読めているほうを全画素で描く」として1を返す。
                // その段は PreviousLOD のことがあり、GetCurrentLOD は nullptr を返す。
                // 平面反射パスは単体を GetCurrentLOD で引くので、そこを一致させておかないと
                // 「まとめられた個体は水面に映るが、同じ状態でまとめられなかった個体は映らない」
                // という食い違いが出る
                if (draws[0].Model != GetCurrentLOD(i))
                {
                    return nullptr;
                }
                return draws[0].Model;
            },
            m_InstanceBatchesCurrentLOD, m_InstanceBatchedCurrentLOD);

        // 組2: 常に最も粗い段(シャドウ / 反射プローブ)
        buildFor(
            [this](size_t i) -> const Assets::Model* { return GetCoarsestLOD(m_Scene.Instances[i]); },
            m_InstanceBatchesCoarsestLOD, m_InstanceBatchedCoarsestLOD);

        m_InstancedBatchCount =
            static_cast<uint32_t>(m_InstanceBatchesCurrentLOD.size() + m_InstanceBatchesCoarsestLOD.size());
        m_InstancedInstanceCount = static_cast<uint32_t>(m_ModelInstanceRecords.size());

        if (m_ModelInstanceRecords.empty())
        {
            return;
        }

        // 容量はシーン読み込み時に「インスタンス数×2組」で確保してある。超えることは無いが、
        // 超えたときに黙って壊れないよう検査してログを残す
        const size_t capacity = m_Scene.Instances.size() * 2;
        if (m_ModelInstanceRecords.size() > capacity)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "インスタンスバッファの容量(" + std::to_string(capacity) + "件)を超えました("
                    + std::to_string(m_ModelInstanceRecords.size()) + "件)。このフレームはインスタンシングを見送ります");
            m_InstanceBatchesCurrentLOD.clear();
            m_InstanceBatchesCoarsestLOD.clear();
            std::fill(m_InstanceBatchedCurrentLOD.begin(), m_InstanceBatchedCurrentLOD.end(), 0u);
            std::fill(m_InstanceBatchedCoarsestLOD.begin(), m_InstanceBatchedCoarsestLOD.end(), 0u);
            m_InstancedBatchCount = 0;
            m_InstancedInstanceCount = 0;
            return;
        }

        // 【1フレームに1回だけ】どのパスもこの1本を読む。バインドは各パスがDraw直前に張り直す
        // (頂点シェーダー用SRVはt0の1本しかなく、ドローンショーが同じスロットを使うため)
        commandList->UpdateBuffer(
            m_ModelInstanceBuffer.get(), m_ModelInstanceRecords.data(),
            m_ModelInstanceRecords.size() * sizeof(GPUModelInstance));
    }

    void KurenaiEngine3D::UpdateModelLOD(const DirectX::XMFLOAT3& cameraPosition, float deltaSeconds)
    {
        m_LODSwitchCount = 0;
        m_RenderStats.LODFadingCount = 0;

        if (m_InstanceLODStates.size() != m_Scene.Instances.size())
        {
            // シーンが差し替わった直後。状態を作り直す(全インスタンスが最も詳細な段から始まる)
            m_InstanceLODStates.assign(m_Scene.Instances.size(), InstanceLODState{});
        }

        for (size_t i = 0; i < m_Scene.Instances.size(); ++i)
        {
            Assets::ModelInstance& instance = m_Scene.Instances[i];
            InstanceLODState& state = m_InstanceLODStates[i];

            const size_t levelCount = instance.LODModels.size() + 1;
            if (levelCount <= 1)
            {
                // LODを持たないインスタンス。従来どおり1段だけ
                state.CurrentLOD = 0;
                state.PreviousLOD = 0;
                state.FadeT = 1.0f;
                instance.LODLevel = 0;
                continue;
            }

            // 【AABBの最近接点までの距離】中心距離だと1.1km四方のPLATEAUタイルで破綻する。
            // タイルの上に立っていても中心までは500m以上あるため、近景なのに粗い段が選ばれる。
            // 点がAABBの内側なら距離0になる(各軸の食い込み量が0になるため)
            float squaredDistance = 0.0f;
            const float cameraXYZ[3] = { cameraPosition.x, cameraPosition.y, cameraPosition.z };
            for (int axis = 0; axis < 3; ++axis)
            {
                const float outside = (std::max)(
                    { instance.WorldBoundsMin[axis] - cameraXYZ[axis],
                      cameraXYZ[axis] - instance.WorldBoundsMax[axis], 0.0f });
                squaredDistance += outside * outside;
            }
            const float distance = std::sqrt(squaredDistance);

            // 【1フレームに1段だけ動かす】ヒステリシスを素直に書ける。段数の上限は4なので、
            // 遠くから一気に近づいても数フレームで追いつく
            uint32_t desired = state.CurrentLOD;
            if (desired < instance.LODDistances.size() &&
                distance > instance.LODDistances[desired] * (1.0f + m_GeometrySettings.LODHysteresis))
            {
                desired = desired + 1;
            }
            else if (desired > 0 &&
                     distance < instance.LODDistances[desired - 1] * (1.0f - m_GeometrySettings.LODHysteresis))
            {
                desired = desired - 1;
            }

            if (desired != state.CurrentLOD)
            {
                // フェード中に次の切り替えが来たら、いま描いている「先」を新しい「元」にする。
                // 3段以上を同時に重ねることはしない(ディザが排他にならず穴が開く)
                state.PreviousLOD = state.CurrentLOD;
                state.CurrentLOD = desired;
                state.FadeT = 0.0f;
                ++m_LODSwitchCount;
            }
            else if (state.FadeT < 1.0f)
            {
                state.FadeT = (m_GeometrySettings.LODFadeDuration > 0.0f)
                    ? (std::min)(1.0f, state.FadeT + deltaSeconds / m_GeometrySettings.LODFadeDuration)
                    : 1.0f;
            }

            if (state.FadeT < 1.0f)
            {
                ++m_RenderStats.LODFadingCount;
            }

            // 常駐マップ(StreamingPanel)が色分けに使う。ここが唯一の書き込み元
            instance.LODLevel = state.CurrentLOD;
        }
    }

    void KurenaiEngine3D::RequestRaytracingRebuild()
    {
        if (!m_Device->SupportsRaytracing() || !m_Scene.HasStreamingDistance)
        {
            return;
        }
        m_RaytracingRebuildPending = true;
        m_RaytracingRebuildAfter = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(static_cast<int>(kRaytracingRebuildQuietSeconds * 1000.0f));
    }

    void KurenaiEngine3D::UpdateRaytracingRebuild()
    {
        // --- 出来上がったものを差し替える ---------------------------------------------------
        {
            std::unique_ptr<Assets::RaytracingScene> rebuilt;
            uint64_t generation = 0;
            {
                std::lock_guard<std::mutex> lock(m_RaytracingRebuiltMutex);
                rebuilt = std::move(m_RaytracingRebuilt);
                generation = m_RaytracingRebuiltGeneration;
            }
            if (rebuilt && generation == m_StreamingGeneration)
            {
                auto retired = std::make_unique<Assets::RaytracingScene>(std::move(m_RaytracingScene));
                m_RaytracingPendingRelease.push_back({ std::move(retired), kStreamingReleaseDelayFrames });
                m_RaytracingScene = std::move(*rebuilt);
                ++m_RaytracingRebuildCount;
            }
        }

        // --- 寝かせ終えたものをLoaderスレッドへ渡す -------------------------------------------
        //
        // 【ここでresetしてはいけない】RaytracingSceneが持つディスクリプタは、ロックを持たない
        // アセット用ヒープから取られている。Loaderスレッドがストリーミングで確保している最中に
        // Renderスレッドが解放するとフリーリストが壊れる。モデルの破棄と同じ経路へ寄せる
        if (!m_RaytracingPendingRelease.empty())
        {
            std::vector<std::unique_ptr<Assets::RaytracingScene>> ready;
            for (PendingRaytracingRelease& pending : m_RaytracingPendingRelease)
            {
                if (pending.FramesRemaining > 0)
                {
                    --pending.FramesRemaining;
                    continue;
                }
                ready.push_back(std::move(pending.Scene));
            }
            m_RaytracingPendingRelease.erase(
                std::remove_if(
                    m_RaytracingPendingRelease.begin(), m_RaytracingPendingRelease.end(),
                    [](const PendingRaytracingRelease& pending) { return !pending.Scene; }),
                m_RaytracingPendingRelease.end());

            if (!ready.empty())
            {
                {
                    std::lock_guard<std::mutex> lock(m_RaytracingReleaseMutex);
                    for (auto& scene : ready)
                    {
                        m_RaytracingRelease.push_back(std::move(scene));
                    }
                }
                m_LoadRequestCV.notify_one();
            }
        }

        // --- 静かになったら発注する -----------------------------------------------------------
        if (!m_RaytracingRebuildPending || std::chrono::steady_clock::now() < m_RaytracingRebuildAfter)
        {
            return;
        }
        m_RaytracingRebuildPending = false;
        m_RaytracingRebuildInFlight.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(m_LoadRequestMutex);
            m_RaytracingRebuildRequested = true;
        }
        m_LoadRequestCV.notify_one();
    }

    void KurenaiEngine3D::UpdateModelStreaming(const DirectX::XMFLOAT3& cameraPosition)
    {
        m_StreamingResidentCount = 0;
        m_StreamingTargetCount = 0;

        // 破棄待ちを1フレーム進める。0になったものだけLoaderスレッドへ渡す。
        // 【ストリーミングを使わないシーンでも回す】シーンを切り替えた直後に、
        // 前のシーンで積んだ分が残っていることがある
        if (!m_StreamingPendingRelease.empty())
        {
            std::vector<std::shared_ptr<Assets::Model>> ready;
            for (PendingModelRelease& pending : m_StreamingPendingRelease)
            {
                if (pending.FramesRemaining > 0)
                {
                    --pending.FramesRemaining;
                    continue;
                }
                // 【常駐ミップの読み直しが終わるまで待つ】Loaderスレッドがこのモデルの
                // IRHITexture*を掴んでいる間に解放すると解放済みを触る。
                // フレーム数の遅延では足りない(1件が数十msかかることがある)
                if (m_TextureStreaming.IsModelBusy(pending.Model.get()))
                {
                    continue;
                }
                ready.push_back(std::move(pending.Model));
            }
            m_StreamingPendingRelease.erase(
                std::remove_if(
                    m_StreamingPendingRelease.begin(), m_StreamingPendingRelease.end(),
                    [](const PendingModelRelease& pending) { return !pending.Model; }),
                m_StreamingPendingRelease.end());

            if (!ready.empty())
            {
                {
                    std::lock_guard<std::mutex> lock(m_StreamingReleaseMutex);
                    for (std::shared_ptr<Assets::Model>& model : ready)
                    {
                        m_StreamingRelease.push_back(std::move(model));
                    }
                }
                // Loaderスレッドが寝ていると破棄が溜まり続けるので起こす
                m_LoadRequestCV.notify_one();
            }
        }

        if (!m_Scene.HasStreamingDistance)
        {
            return;
        }

        // --- Loaderスレッドが仕上げたものを取り込む -----------------------------------------
        {
            std::vector<StreamingLoaded> loaded;
            {
                std::lock_guard<std::mutex> lock(m_StreamingLoadedMutex);
                loaded.swap(m_StreamingLoaded);
            }
            // 再構築中はLoaderスレッドが m_Scene を走査しているので差し込まない
            if (m_RaytracingRebuildInFlight.load(std::memory_order_acquire))
            {
                std::lock_guard<std::mutex> lock(m_StreamingLoadedMutex);
                for (StreamingLoaded& item : loaded)
                {
                    m_StreamingLoaded.push_back(std::move(item));
                }
                loaded.clear();
            }

            for (StreamingLoaded& item : loaded)
            {
                m_StreamingInFlight.erase(item.Path);
                // 【古い世代は捨てる】シーンを切り替えた後に前のシーンのモデルが届くことがある
                if (item.Generation != m_StreamingGeneration || !item.Model)
                {
                    continue;
                }
                ++m_StreamingLoadedTotal;
                RequestRaytracingRebuild();
                // 同じパスを指すすべての段へ差し込む(モデル共有。2-1と同じ考え方)
                auto shared = std::shared_ptr<const Assets::Model>(item.Model);
                m_Scene.ModelCache[item.Path] = std::move(item.Model);
                for (Assets::ModelInstance& instance : m_Scene.Instances)
                {
                    bool referenced = false;
                    for (size_t level = 0; level < instance.ModelPaths.size(); ++level)
                    {
                        if (instance.ModelPaths[level] != item.Path)
                        {
                            continue;
                        }
                        referenced = true;
                        if (level == 0)
                        {
                            instance.Model = shared;
                        }
                        else
                        {
                            instance.LODModels[level - 1] = shared;
                        }
                    }
                    // 常駐ミップ制御の追跡表へ入れる。
                    // 【インスタンスごとに1回だけ】メッシュのAABBはインスタンスのWorldで
                    // ワールド空間へ移して持つので、参照はインスタンスの数だけ要る。
                    // 逆に同じインスタンスで2回呼ぶと、同じ参照が二重に積まれる
                    // (同じパスが複数の段に入っているシーンで起きうる)
                    if (referenced)
                    {
                        m_TextureStreaming.AttachModel(*shared, instance.World, *m_Device);
                    }
                }
            }
        }

        // --- 距離を見て、足りないものを近い順に発注する -------------------------------------
        //
        // 【段ごとに要否が違う】いま選ばれている段だけを読めばよい。遠くて粗い段しか使わない
        // タイルの詳細な段まで読むと、ストリーミングの意味が無くなる
        struct Candidate
        {
            float DistanceSq = 0.0f;
            const std::wstring* Path = nullptr;
        };
        std::vector<Candidate> candidates;

        // 破棄しない(=まだ要る)パスの集合。読み込みの判定より広い距離で集める
        std::unordered_set<std::wstring> neededPaths;

        const float limit = m_Scene.StreamingDistance;
        const float limitSq = limit * limit;
        // 【破棄は読み込みより遠くで行う】同じ距離でやると、境界上でカメラが揺れるたびに
        // 読み込みと破棄が交互に起きて、ディスクアクセスが止まらなくなる。
        // 1.25倍の不感帯を置く(モデルLODのヒステリシスと同じ考え方)
        const float evictLimitSq = (limit * 1.25f) * (limit * 1.25f);
        const float cameraXYZ[3] = { cameraPosition.x, cameraPosition.y, cameraPosition.z };

        for (size_t i = 0; i < m_Scene.Instances.size(); ++i)
        {
            Assets::ModelInstance& instance = m_Scene.Instances[i];
            if (instance.ModelPaths.empty())
            {
                continue;
            }

            // 常駐マップ(StreamingPanel)が色分けに使う3値。
            // 【距離で抜ける前に書く】範囲外のインスタンスもここを通らなければ
            // 古い値が残り、破棄されたものが「常駐」の色のまま地図に出る
            const uint32_t level = (i < m_InstanceLODStates.size()) ? m_InstanceLODStates[i].CurrentLOD : 0u;
            const size_t levelIndex = (level < instance.ModelPaths.size()) ? level : 0u;
            instance.Residency =
                instance.IsLODLoaded(levelIndex)                             ? Assets::ResidencyState::Loaded
                : (m_StreamingInFlight.count(instance.ModelPaths[levelIndex]) != 0)
                                                                            ? Assets::ResidencyState::Loading
                                                                            : Assets::ResidencyState::Unloaded;

            // モデルLODと同じ「AABBの最近接点まで」の距離
            float squaredDistance = 0.0f;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float outside = (std::max)(
                    { instance.WorldBoundsMin[axis] - cameraXYZ[axis],
                      cameraXYZ[axis] - instance.WorldBoundsMax[axis], 0.0f });
                squaredDistance += outside * outside;
            }
            // 破棄の不感帯(1.25倍)の内側にあるものは、読み込み対象でなくても捨てない
            if (squaredDistance <= evictLimitSq)
            {
                for (const std::wstring& path : instance.ModelPaths)
                {
                    neededPaths.insert(path);
                }
            }

            if (squaredDistance > limitSq)
            {
                continue;
            }
            ++m_StreamingTargetCount;

            if (instance.IsLODLoaded(levelIndex))
            {
                ++m_StreamingResidentCount;
                continue;
            }

            const std::wstring& path = instance.ModelPaths[levelIndex];
            if (m_StreamingInFlight.count(path) != 0)
            {
                continue;
            }
            candidates.push_back({ squaredDistance, &path });
        }

        // --- 遠ざかったものを破棄する ---------------------------------------------------------
        //
        // 【モデルは共有されている】同じ.kmodelを複数のインスタンスが指しうるので、
        // 「どれか1つでもまだ要る」なら捨てられない。インスタンス単位ではなく
        // ModelCacheをパス単位で見て、needed に無いものだけを外す
        // 再構築中は破棄しない(理由は上の差し込みと同じ)
        if (!m_RaytracingRebuildInFlight.load(std::memory_order_acquire))
        {
            std::vector<std::wstring> evictPaths;
            for (const auto& entry : m_Scene.ModelCache)
            {
                if (neededPaths.count(entry.first) == 0)
                {
                    evictPaths.push_back(entry.first);
                }
            }

            for (const std::wstring& path : evictPaths)
            {
                // インスタンス側の参照を外す。描画ループは未読み込みとして飛ばす
                for (Assets::ModelInstance& instance : m_Scene.Instances)
                {
                    for (size_t level = 0; level < instance.ModelPaths.size(); ++level)
                    {
                        if (instance.ModelPaths[level] != path)
                        {
                            continue;
                        }
                        if (level == 0)
                        {
                            instance.Model.reset();
                        }
                        else
                        {
                            instance.LODModels[level - 1].reset();
                        }
                    }
                }

                auto cached = m_Scene.ModelCache.find(path);
                if (cached == m_Scene.ModelCache.end())
                {
                    continue;
                }
                // 【破棄より前に追跡表から外す】これ以降このモデルへ新しい読み直しは発注されない。
                // 発注済みのものは破棄待ちの側(IsModelBusy)で待つ
                m_TextureStreaming.DetachModel(*cached->second);
                // 実体はここで消さず、GPUが読み終わるまで寝かせる
                m_StreamingPendingRelease.push_back(
                    { std::move(cached->second), kStreamingReleaseDelayFrames });
                m_Scene.ModelCache.erase(cached);
                ++m_StreamingEvictedTotal;
                RequestRaytracingRebuild();
            }
        }

        if (candidates.empty())
        {
            return;
        }

        // 近い順に発注する。手前のものから絵が埋まるので、遠くの読み込みで手前が待たされない
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) { return a.DistanceSq < b.DistanceSq; });

        // 【1フレームの発注数に上限を置く】Loaderスレッドは1本で、シーン切り替えもここを通る。
        // 際限なく積むと、切り替え要求が数百件の読み込みの後ろで待たされる
        constexpr size_t kMaxStreamingRequestsPerFrame = 8;
        const size_t requestCount = (std::min)(candidates.size(), kMaxStreamingRequestsPerFrame);

        {
            std::lock_guard<std::mutex> lock(m_LoadRequestMutex);
            for (size_t i = 0; i < requestCount; ++i)
            {
                m_StreamingRequests.push_back({ *candidates[i].Path, m_StreamingGeneration });
                m_StreamingInFlight.insert(*candidates[i].Path);
            }
        }
        m_LoadRequestCV.notify_one();
    }

    uint32_t KurenaiEngine3D::GetLODDraws(size_t instanceIndex, LODDraw (&outDraws)[2]) const
    {
        const Assets::ModelInstance& instance = m_Scene.Instances[instanceIndex];
        // ストリーミング中はまだ読み込まれていない段がある。nullptrの段は描画対象から外す
        const auto modelAt = [&instance](uint32_t level) -> const Assets::Model*
        {
            return (level == 0) ? instance.Model.get() : instance.LODModels[level - 1].get();
        };

        if (instanceIndex >= m_InstanceLODStates.size())
        {
            outDraws[0] = { instance.Model.get(), 1.0f };
            return instance.Model ? 1u : 0u;
        }

        const InstanceLODState& state = m_InstanceLODStates[instanceIndex];
        if (state.FadeT >= 1.0f)
        {
            outDraws[0] = { modelAt(state.CurrentLOD), 1.0f };
            return outDraws[0].Model ? 1u : 0u;
        }

        // 切り替え「先」は +FadeT、「元」は -FadeT。同じノイズをしきい値の両側で分け合うので、
        // 2段が同じ画素に重ならず(Zファイティングにならず)、隙間もできない。
        //
        // 【片方が未読み込みなら、もう片方を全画素で描く】ディザで分け合う相手がいないのに
        // 半分だけ描くと、その間だけモデルに穴が開く
        const Assets::Model* const toModel = modelAt(state.CurrentLOD);
        const Assets::Model* const fromModel = modelAt(state.PreviousLOD);
        if (!toModel || !fromModel)
        {
            const Assets::Model* const only = toModel ? toModel : fromModel;
            outDraws[0] = { only, 1.0f };
            return only ? 1u : 0u;
        }
        outDraws[0] = { toModel, state.FadeT };
        outDraws[1] = { fromModel, -state.FadeT };
        return 2;
    }

    const Assets::Model* KurenaiEngine3D::GetCurrentLOD(size_t instanceIndex) const
    {
        const Assets::ModelInstance& instance = m_Scene.Instances[instanceIndex];
        const uint32_t level = (instanceIndex < m_InstanceLODStates.size())
            ? m_InstanceLODStates[instanceIndex].CurrentLOD
            : 0u;
        // ストリーミング中はまだ読み込まれていないことがある。nullptrを返し、呼び出し側が飛ばす
        return (level == 0) ? instance.Model.get() : instance.LODModels[level - 1].get();
    }

    const Assets::Model* KurenaiEngine3D::GetCoarsestLOD(const Assets::ModelInstance& instance) const
    {
        // 【影と間接光は常に最も粗い段】どちらもテクスチャを読まないので、詳細な段を描く意味が無い。
        // PLATEAUではLOD2(約1715メッシュ)がLOD1(1メッシュ)になるため、
        // シャドウのドローコールが4カスケード分まとめて桁で減る
        return instance.LODModels.empty() ? instance.Model.get() : instance.LODModels.back().get();
    }

    void KurenaiEngine3D::RetireAssets(RetiredAssets&& retired)
    {
        std::lock_guard<std::mutex> lock(m_RetiredAssetsMutex);
        m_RetiredAssets.push_back(std::move(retired));
    }
}
