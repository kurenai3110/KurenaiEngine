#include "../KurenaiEngine3D.h"

#include <algorithm>
#include <vector>

#include "Core/Logger.h"
#include "Core/RenderGraph.h"
#include "GeometryPasses.h"
#include "GeometryConstants.h"
#include "../Rendering/GeometryDrawLoop.h"
#include "../Rendering/ObjectConstants.h"
#include "../Rendering/RenderBlackboard.h"
#include "../Rendering/RenderFrameContext.h"
#include "../Rendering/SunLighting.h"
#include "../ShaderInterop/FrameConstants.h"
#include "../ShaderInterop/GroupSizes.h"

namespace Kurenai::Passes
{
    namespace
    {
        using Rendering::FrustumPlanes;
        using Rendering::ExtractFrustumPlanes;
        using ShaderInterop::FrameConstants;
    }

    void GeometryPasses::Register(
        Core::RenderGraph& graph,
        const Rendering::RenderFrameContext& frame,
        Rendering::RenderBlackboard& bb)
    {
        const uint32_t renderWidth = frame.RenderWidth;
        const uint32_t renderHeight = frame.RenderHeight;
        RHI::IRHIBuffer* const frameConstantBuffer = frame.FrameConstantBuffer;
        RHI::IRHIBuffer* const objectConstantBuffer = frame.ObjectConstantBuffer;
        RHI::IRHISamplerSet* const materialSamplers = frame.MaterialSamplers;

        // 【フレームの値をここで写し取る】以下はRender()から機械的に移した登録コードなので、
        // 参照している名前を変えずに済むよう同じ名前で受け直す。
        //
        // viewProj と sunLighting は**参照として束縛する**。実体は Render() の
        // frameContext / ローカルで、graph.Execute() が終わるまで生きているため、
        // 参照捕捉している既存のラムダがそのまま正しく動く。
        // 一方 gbufferViewport は値の複製なので、ラムダへは値で捕捉すること
        const DirectX::XMMATRIX& viewProj = frame.ViewProj;
        const SunLighting& sunLighting = *frame.Sun;
        const RHI::Viewport gbufferViewport = frame.GBufferViewport;
        const bool depthPrepassRuns = frame.DepthPrepassRuns;
        const bool hiZFromDepthPrepass = frame.HiZFromDepthPrepass;
        const bool occlusionCullEnabledThisFrame = frame.OcclusionCullEnabledThisFrame;
        const bool occlusionCullingActive = frame.OcclusionCullingActive;
        const bool meshletPathActive = frame.MeshletPathActive;
        const bool meshletCullStatsActive = frame.MeshletCullStatsActive;
        const float cameraMoveDistance = frame.CameraMoveDistance;

        // --- 深度プリパス(41.22節): 不透明ジオメトリの深度だけを先に埋める ---
        //
        // これを通しておくと、次のG-Bufferパスでは最前面の断片だけが深度テストを通り
        // (PSOのDepthAllowEqual)、隠れる画素のピクセルシェーダー ―― 6テクスチャの
        // サンプルと6枚のレンダーターゲットへの書き込み ―― がまるごと省ける。
        //
        // 【カットアウトのPSOが作れていないときはプリパスごと切る】カットアウトのメッシュだけを
        // プリパスから外すと、そのメッシュはG-Buffer側で深度を書くことになるが、
        // プリパスで手前に別のものが書かれていると早期Zに落とされて消える。
        // 中途半端に混ぜるより丸ごと従来経路にするほうが安全
        //
        // 【メッシュシェーダー経路とも併用できるようになった】かつてプリパスはメッシュレット経路と
        // 排他だった。プリパスが頂点シェーダーで深度を書き、G-Bufferがメッシュシェーダーで描くと、
        // 同じ頂点でも変換の丸めが一致する保証が無く、深度が1ulpずれた面がGREATER_EQUALを
        // 通らずに消えるため。プリパスにもG-Bufferとまったく同じ増幅/メッシュシェーダーを使う
        // PSOを用意したので、変換は文字どおり同一のコードになりこの問題は起きない。
        //
        // メッシュレット版のPSOが作れなかった場合は、その経路で描くモデルだけを
        // プリパスから外す(下のループ参照)。深度が埋まらないぶん早期Zが効かないだけで、
        // G-Buffer側が改めて深度を書くため絵は壊れない
        // (depthPrepassRuns / hiZFromDepthPrepass の宣言は FrameConstants を組む箇所にある)

        // --- モデル単位のGPUカリングパス(Stage 5-3) ---
        //
        // 描画候補のワールドAABBを、コンピュートシェーダーが視錐台とHi-Zで判定し、
        // 生き残ったものだけの ExecuteIndirect 引数をGPU上に作る。
        // 深度プリパスとG-Bufferは、その引数でまとめて描く。
        //
        // 【深度プリパスより前に登録すること】RenderGraphは登録順で実行する。
        // 引数を使うのはプリパスとG-Bufferの両方なので、どちらよりも前にいる必要がある。
        // **片方だけ間引くと絵が壊れる** ―― プリパスが深度を書いたものをG-Bufferが
        // 描かないと、その画素は「深度はあるのに色が無い」穴になる。
        //
        // 【Hi-Zは前フレームのもの】Hi-Zパスの登録はG-Bufferより後なので、ここが読むのは
        // 前フレームに書かれた内容になる。カメラ移動ぶんAABBを膨らませて視差を吸収する
        const bool modelCullGpuActive = frame.Settings.Geometry.ModelCullGpuEnabled && meshletPathActive
            && m_Engine.m_ModelCullPipelineState && m_Engine.m_ModelCullCounterBuffer && !m_Engine.m_Scene.Instances.empty();

        // hiZFromDepthPrepass = Hi-Zを**深度プリパスの深度から**作るか(宣言は上流にある)。
        // 作れるなら、G-Bufferの判定は今フレームのHi-Zで行える ――
        // 1フレーム遅れも保守的な膨張も要らなくなる。
        //
        // 【プリパスが走らないフレームは従来どおり】プリパスが無ければ深度が埋まっておらず、
        // そこでHi-Zを作っても中身は空になる。その場合はG-Bufferの後で作り、
        // 次フレームに前フレームのものとして読む(m_HiZValidが立つのもそのとき)。
        //
        // 【プリパス自身は今フレームのHi-Zを使えない】そのHi-Zはプリパスの出力から作る。
        // プリパスは従来どおり前フレームのHi-Zで判定する ―― 保守的なので絵は壊れないし、
        // 判定を捨てるとプリパスが描くメッシュレットが9倍近くに戻る

        // 前フレームのHi-Zで判定してよいか(深度プリパス、およびプリパスが無いときのG-Buffer)
        const bool occlusionPrevFrameEnabled = occlusionCullEnabledThisFrame;
        // 今フレームのHi-Zで判定してよいか(G-Buffer。Hi-Z構築パスが必ず先に走る)
        const bool occlusionCurrentFrameEnabled = hiZFromDepthPrepass;

        // ObjectConstants::MeshletOcclusionMode と ModelCull のオクルージョン有効フラグへ渡す値
        const uint32_t prepassOcclusionMode = occlusionPrevFrameEnabled ? 1u : 0u;
        const uint32_t gbufferOcclusionMode = occlusionCurrentFrameEnabled
            ? 2u
            : (occlusionPrevFrameEnabled ? 1u : 0u);

        // 区画(=PSO)の対応表。nullptrの区画は候補に載せない(そのぶんは従来のCPUループが描く)
        RHI::IRHIPipelineState* modelCullRegionPipelines[kModelCullRegionCount]{};
        if (modelCullGpuActive)
        {
            const bool meshletDebug = frame.Settings.Geometry.MeshletDebugViewEnabled && m_Engine.m_GBufferMeshletDebugPipelineState;
            modelCullRegionPipelines[kModelCullRegionGBuffer] = meshletDebug
                ? m_Engine.m_GBufferMeshletDebugPipelineState.get()
                : m_Engine.m_GBufferMeshletPipelineState.get();
            modelCullRegionPipelines[kModelCullRegionGBufferMirrored] = meshletDebug
                ? m_Engine.m_GBufferMeshletDebugPipelineStateMirrored.get()
                : m_Engine.m_GBufferMeshletPipelineStateMirrored.get();
            if (depthPrepassRuns)
            {
                modelCullRegionPipelines[kModelCullRegionPrepassOpaque] =
                    m_Engine.m_DepthPrepassMeshletPipelineState.get();
                modelCullRegionPipelines[kModelCullRegionPrepassOpaqueMirrored] =
                    m_Engine.m_DepthPrepassMeshletPipelineStateMirrored.get();
                modelCullRegionPipelines[kModelCullRegionPrepassCutout] =
                    m_Engine.m_DepthPrepassMeshletCutoutPipelineState.get();
                modelCullRegionPipelines[kModelCullRegionPrepassCutoutMirrored] =
                    m_Engine.m_DepthPrepassMeshletCutoutPipelineStateMirrored.get();
            }
        }

        // 候補の列挙。**プリパスとG-Bufferの元のループとまったく同じ条件で選ぶこと** ――
        // 選び方がずれると、間引かれてもいないのに描かれないドローが出る
        //
        // 【並びは「深度プリパスぶん → G-Bufferぶん」】判定を2回に分けるため、
        // 各ディスパッチが受け持つ範囲が連続していなければならない
        // 【パス群の持ち物にしてある】複数のExecuteラムダが同じ実体を参照捕捉するため、
        // この関数のローカルにすると登録を抜けた時点で浮く。中身はフレームごとに作り直す
        m_ModelCullDraws.clear();
        std::vector<ModelCullDrawCandidate>& modelCullDraws = m_ModelCullDraws;
        std::vector<ModelCullDrawCandidate> modelCullGBufferDraws;
        std::fill(std::begin(m_Engine.m_ModelCullRegionCandidates), std::end(m_Engine.m_ModelCullRegionCandidates), 0u);
        m_Engine.m_ModelCullCandidateCount = 0;
        m_Engine.m_ModelCullPrepassCandidateCount = 0;
        m_Engine.m_ModelCullCpuFrustumCulled = 0;
        if (modelCullGpuActive)
        {
            // 突き合わせ相手のCPU側の判定。G-Bufferのループが使うものと同じ錐台
            const FrustumPlanes cullFrustum = ExtractFrustumPlanes(viewProj);
            modelCullDraws.reserve(m_Engine.m_Scene.Instances.size() * 2);

            // モデルLOD。フェード中は2段を重ねる。
            // 【プリパス・G-Bufferと同じ組になる】列挙も段の選択も、あちらと同じ
            // ForEachGeometryDrawが行う。UpdateModelLODがフレーム先頭で1回だけ決めた
            // 結果を全員が引くので、ここで距離を測り直さない。
            // 【カリングはしない】間引くのはGPU側の仕事で、ここで落とすと候補から
            // 消えてしまう。CPU側の判定は突き合わせ用にcpuVisibleとして数えるだけ。
            // 【バッチは使わない】1モデル1ドロー経路の候補を集めるので、
            // インスタンスを1体ずつ見る
            Rendering::GeometryDrawLoopDesc cullLoop;
            cullLoop.Frustum = nullptr;
            cullLoop.UseDrawUnits = false;
            cullLoop.LODMode = Rendering::GeometryLODMode::Fade;

            m_Engine.ForEachGeometryDraw(
                cullLoop,
                [&](const Rendering::InstanceDrawUnit& unit, const Assets::Model& lodModel, float lodDitherFade)
                {
                    const Assets::ModelInstance& instance = *unit.Instance;
                    // このモデル単位で描き切る経路の候補だけを集める。
                    // **必ず真を返してメッシュのループへ入らない**(候補集めであって描画ではない)
                    if (!m_Engine.ShouldUseModelMeshletPath(instance, lodModel))
                    {
                        return true;
                    }

                    // 起動するのは「モデル全体のメッシュレット数 ÷ 増幅シェーダーのグループサイズ」。
                    // 実際にラスタライズされるのはカリングとふるい分けを生き延びたぶんに絞られる
                    const uint32_t groupCount = (lodModel.TotalMeshletCount
                        + ShaderInterop::kAmplificationGroupSize - 1) / ShaderInterop::kAmplificationGroupSize;
                    if (groupCount == 0)
                    {
                        return true;
                    }

                    const bool mirrored = instance.IsMirrored;
                    // 同じ候補をCPU側の判定なら間引くか。GPUの「視錐台で間引いた数」と突き合わせる
                    const bool cpuVisible =
                        IsAABBVisible(cullFrustum, instance.WorldBoundsMin, instance.WorldBoundsMax);

                    const auto addCandidate = [&](std::vector<ModelCullDrawCandidate>& list, uint32_t region,
                                                  uint32_t rejectMask, uint32_t requireMask, float ditherFade,
                                                  bool countCullStats, uint32_t occlusionMode)
                    {
                        if (!modelCullRegionPipelines[region])
                        {
                            return;
                        }
                        ModelCullDrawCandidate candidate{};
                        candidate.Instance = &instance;
                        candidate.Model = &lodModel;
                        candidate.Region = region;
                        candidate.GroupCount = groupCount;
                        candidate.RejectMask = rejectMask;
                        candidate.RequireMask = requireMask;
                        candidate.DitherFade = ditherFade;
                        candidate.CountCullStats = countCullStats;
                        candidate.OcclusionMode = occlusionMode;
                        list.push_back(candidate);
                        ++m_Engine.m_ModelCullRegionCandidates[region];
                        // 【CPU側の間引き数もG-Bufferぶんだけ数える】GPU側の統計と単位を揃えるため。
                        // 両方数えると1モデルを2回数え、モデル数の2倍という比べにくい数になる
                        if (countCullStats && !cpuVisible)
                        {
                            ++m_Engine.m_ModelCullCpuFrustumCulled;
                        }
                    };

                    // G-Buffer。半透明(BLEND)だけは増幅シェーダーが落とす ――
                    // G-Bufferに書かず専用のフォワードパスへ回るため
                    addCandidate(
                        modelCullGBufferDraws,
                        mirrored ? kModelCullRegionGBufferMirrored : kModelCullRegionGBuffer,
                        Assets::kGpuMaterialFlagTransparent, 0u, lodDitherFade, /*countCullStats=*/true,
                        gbufferOcclusionMode);

                    // 深度プリパス。
                    // 【クロスディザのフェード中は載せない】不透明用のPSOはピクセルシェーダーを
                    // 持たずApplyLODDitherを通せないため、捨てるはずの画素まで深度を書いて
                    // G-Bufferとの食い違いで穴が開く(深度プリパスのループと同じ条件)
                    if (!depthPrepassRuns || lodDitherFade < 1.0f)
                    {
                        return true;
                    }
                    addCandidate(
                        modelCullDraws,
                        mirrored ? kModelCullRegionPrepassOpaqueMirrored : kModelCullRegionPrepassOpaque,
                        Assets::kGpuMaterialFlagTransparent | Assets::kGpuMaterialFlagCutout, 0u, 1.0f, false,
                        prepassOcclusionMode);
                    // カットアウトぶん(clipを通す)。持たないモデルではこの回は発行しない
                    if (lodModel.HasCutoutMaterial)
                    {
                        addCandidate(
                            modelCullDraws,
                            mirrored ? kModelCullRegionPrepassCutoutMirrored : kModelCullRegionPrepassCutout,
                            Assets::kGpuMaterialFlagTransparent, Assets::kGpuMaterialFlagCutout, 1.0f, false,
                            prepassOcclusionMode);
                    }
                    return true;
                },
                // 上が常に真を返すのでここへは来ない
                [](const Rendering::InstanceDrawUnit&, const Assets::Model&, const Assets::Mesh&, float) { return true; });

            // プリパスぶんを前半、G-Bufferぶんを後半に置く
            m_Engine.m_ModelCullPrepassCandidateCount = static_cast<uint32_t>(modelCullDraws.size());
            modelCullDraws.insert(
                modelCullDraws.end(), modelCullGBufferDraws.begin(), modelCullGBufferDraws.end());
            m_Engine.m_ModelCullCandidateCount = static_cast<uint32_t>(modelCullDraws.size());
            m_Engine.EnsureModelCullCapacity(m_Engine.m_ModelCullCandidateCount);
        }

        // ここまで来て初めてバッファが揃っているかが分かる(容量確保は失敗しうる)
        const bool modelCullReady = modelCullGpuActive && m_Engine.m_ModelCullCandidateCount > 0
            && m_Engine.m_ModelCullInstanceBuffer && m_Engine.m_ModelCullDrawArgsBuffer;
        // 【ブラックボードへ載せる】graph.Execute() の後で走るカウンタの読み戻しが、
        // 「このフレームは間接描画の候補が揃っていたか」を同じ判断で知る必要がある
        bb.ModelCullReady = modelCullReady;
        // 実際に描画発行まで任せるか。
        // 【DX11とメッシュシェーダー非対応環境では常にfalse】従来のCPUループへ縮退する
        const bool modelCullIndirectActive =
            modelCullReady && frame.Settings.Geometry.ModelCullIndirectEnabled && m_Engine.m_Device->SupportsIndirectDispatchMesh();
        m_Engine.m_ModelCullIndirectActiveLastFrame = modelCullIndirectActive;
        m_Engine.m_HiZFromDepthPrepassLastFrame = hiZFromDepthPrepass;
        m_Engine.m_ModelCullDispatchCounts[0] = hiZFromDepthPrepass
            ? m_Engine.m_ModelCullPrepassCandidateCount
            : m_Engine.m_ModelCullCandidateCount;
        m_Engine.m_ModelCullDispatchCounts[1] = hiZFromDepthPrepass
            ? (m_Engine.m_ModelCullCandidateCount - m_Engine.m_ModelCullPrepassCandidateCount)
            : 0u;

        // 候補配列のうち [beginIndex, beginIndex + count) だけを判定するパスを1つ登録する。
        //
        // 【2回に分ける必要がある】Hi-Zを深度プリパスの深度から作ると、判定に使えるHi-Zが
        // フレームの途中で「前フレームのもの」から「今フレームのもの」へ変わる。
        // 深度プリパスぶんはプリパスより前に前フレームのHi-Zで、G-Bufferぶんは
        // Hi-Zを作り終えてから今フレームのHi-Zで判定する。
        // initializeBuffers は候補配列の書き込みとカウンタ初期化を行うか(先頭の1回だけtrue)
        const auto addModelCullPass =
            [&](std::string passName, uint32_t beginIndex, uint32_t count, bool initializeBuffers,
                bool useCurrentFrameHiZ, bool occlusionEnabled)
        {
            if (!modelCullReady || (count == 0 && !initializeBuffers))
            {
                return;
            }
            const uint32_t regionStride = m_Engine.m_ModelCullRegionStride;
            const uint32_t statsBeginIndex = m_Engine.m_ModelCullPrepassCandidateCount;
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = std::move(passName),
                // Hi-Zを読む。前フレームのものを読む側では、それより前に書き手がいないので
                // 辺は張られない(RenderGraphのReadsは登録順で解決する)
                .Reads = { m_Engine.m_RenderTargets.HiZTexture.get() },
                .BufferWrites = { m_Engine.m_ModelCullCounterBuffer.get(), m_Engine.m_ModelCullDrawArgsBuffer.get() },
                .Execute = [this, beginIndex, count, initializeBuffers, useCurrentFrameHiZ, occlusionEnabled, regionStride, statsBeginIndex, cameraMoveDistance, modelCullIndirectActive, &viewProj, &modelCullDraws, renderWidth, renderHeight, objectConstantBuffer](RHI::IRHICommandList* cmd)
                {
                    if (initializeBuffers)
                    {
                        // 候補ごとのObjectConstantsを定数バッファのリングへ書き、そのスロットの
                        // GPUアドレスを候補に添える。
                        //
                        // 【描画パスではなくここで書く理由】ExecuteIndirectの引数には
                        // 「このドローが使う定数バッファのアドレス」そのものが要る。アドレスは
                        // UpdateBufferがリングを1つ進めた後にしか分からず、しかも引数を組み立てるのは
                        // このパスのコンピュートシェーダーなので、その材料より前に書いておく必要がある。
                        //
                        // 【後半(G-Bufferぶん)もここで書く】2回目のディスパッチのぶんも含めて
                        // 一度に載せる。リングのスロットは上書きされない限り生き続けるので、
                        // 使うのが後のパスでも構わない
                        m_Engine.m_ModelCullUploadScratch.clear();
                        m_Engine.m_ModelCullUploadScratch.reserve(modelCullDraws.size());
                        for (const ModelCullDrawCandidate& draw : modelCullDraws)
                        {
                            KurenaiEngine3D::GpuModelCullInstance candidate{};
                            candidate.BoundsMin[0] = draw.Instance->WorldBoundsMin[0];
                            candidate.BoundsMin[1] = draw.Instance->WorldBoundsMin[1];
                            candidate.BoundsMin[2] = draw.Instance->WorldBoundsMin[2];
                            candidate.BoundsMax[0] = draw.Instance->WorldBoundsMax[0];
                            candidate.BoundsMax[1] = draw.Instance->WorldBoundsMax[1];
                            candidate.BoundsMax[2] = draw.Instance->WorldBoundsMax[2];
                            candidate.GroupCount = draw.GroupCount;
                            candidate.RegionIndex = draw.Region;

                            if (modelCullIndirectActive)
                            {
                                const ObjectConstants objectConstants = MakeModelObjectConstants(
                                    *draw.Instance, *draw.Model, m_Engine.m_EmissiveLightSettings.Intensity, m_Engine.m_AmbientOcclusionSettings.OcclusionMapEnabled,
                                    draw.RejectMask, draw.RequireMask, m_Engine.m_MeshletLODFrame,
                                    draw.CountCullStats, draw.DitherFade, draw.OcclusionMode);
                                cmd->UpdateBuffer(
                                    objectConstantBuffer, &objectConstants, sizeof(objectConstants));
                                if (!objectConstantBuffer->GetLastUpdateGpuAddress(
                                        candidate.CbvAddress[0], candidate.CbvAddress[1]))
                                {
                                    // アドレスが取れないバックエンドはそもそも間接描画へ行かない。
                                    // 万一ここへ来たら、嘘のアドレスを引数へ書くよりは
                                    // 「描くものが無い候補」にしてGPUに触らせない
                                    candidate.GroupCount = 0;
                                }
                            }

                            m_Engine.m_ModelCullUploadScratch.push_back(candidate);
                        }
                        cmd->UpdateBuffer(
                            m_Engine.m_ModelCullInstanceBuffer.get(), m_Engine.m_ModelCullUploadScratch.data(),
                            static_cast<uint32_t>(
                                sizeof(KurenaiEngine3D::GpuModelCullInstance) * m_Engine.m_ModelCullUploadScratch.size()));

                        // 加算しかしないので毎フレーム0へ戻す。生き残りを詰める位置も
                        // このカウンタで取るため、戻さないと2フレーム目以降が範囲外へ書く
                        cmd->ClearUnorderedAccessBufferUint(m_Engine.m_ModelCullCounterBuffer.get(), 0);
                        // 引数バッファも同じ理由で戻す。**先頭の発行数だけでなく全体を0にする** ――
                        // 前フレームの引数が残っていると、件数が減ったときに古い引数が
                        // 範囲内に居座り、そのぶんが二重に描かれる
                        cmd->ClearUnorderedAccessBufferUint(m_Engine.m_ModelCullDrawArgsBuffer.get(), 0);
                    }

                    if (count == 0)
                    {
                        return;
                    }

                    ModelCullConstants cullConstants{};
                    DirectX::XMStoreFloat4x4(
                        &cullConstants.CullViewProj, DirectX::XMMatrixTranspose(viewProj));
                    if (useCurrentFrameHiZ)
                    {
                        // 今フレームの深度プリパスから作ったHi-Zを読む。投影も今フレームの行列
                        cullConstants.CullPrevViewProj = cullConstants.CullViewProj;
                    }
                    else
                    {
                        cullConstants.CullPrevViewProj =
                            m_Engine.m_TAAPrevViewProjValid ? m_Engine.m_TAAPrevViewProj : DirectX::XMFLOAT4X4{};
                    }
                    cullConstants.CullParams = {
                        count, m_Engine.m_HiZMipLevels, occlusionEnabled ? 1u : 0u, kModelCullArgsBaseOffset
                    };
                    cullConstants.CullRegionParams = {
                        regionStride, kModelCullRegionCount, beginIndex, statsBeginIndex
                    };
                    cullConstants.CullHiZScreenParams = {
                        static_cast<float>(renderWidth), static_cast<float>(renderHeight), 0.0f, 0.0f
                    };
                    // 今フレームのHi-Zなら視差のずれが無いので膨らませない
                    cullConstants.CullExpandParams = {
                        useCurrentFrameHiZ ? 0.0f : cameraMoveDistance, 0.0f, 0.0f, 0.0f
                    };
                    cmd->UpdateBuffer(m_Engine.m_ModelCullConstantBuffer.get(), &cullConstants, sizeof(cullConstants));

                    cmd->SetComputePipelineState(m_Engine.m_ModelCullPipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_Engine.m_ModelCullConstantBuffer.get());
                    cmd->SetComputeShaderResourceBuffer(0, m_Engine.m_ModelCullInstanceBuffer.get());
                    cmd->SetComputeTexture(1, m_Engine.m_RenderTargets.HiZTexture.get());
                    cmd->SetComputeUnorderedAccessBuffer(0, m_Engine.m_ModelCullCounterBuffer.get());
                    cmd->SetComputeUnorderedAccessBuffer(1, m_Engine.m_ModelCullDrawArgsBuffer.get());

                    cmd->Dispatch(
                        (count + ShaderInterop::kModelCullGroupSize - 1) / ShaderInterop::kModelCullGroupSize,
                        1, 1);
                },
            });
        };

        // Hi-Zミップチェーンの構築パス。**登録する場所が2つある**ため関数にしてある。
        // 深度プリパスが走るなら直後に(今フレームの深度から)、走らないならG-Bufferの後に。
        // 詳細は下の hiZPassRuns のコメント
        const auto addHiZPass = [&]()
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "HiZ",
                .Reads = { m_Engine.m_RenderTargets.GBufferDepth.get() },
                .Writes = { m_Engine.m_RenderTargets.HiZTexture.get() },
                .Execute = [this, renderWidth, renderHeight](RHI::IRHICommandList* cmd)
                {
                    HiZConstants hizConstants{};
                    hizConstants.SrcSize = { renderWidth, renderHeight };
                    hizConstants.DstSize = { renderWidth, renderHeight };
                    cmd->UpdateBuffer(m_Engine.m_HiZConstantBuffer.get(), &hizConstants, sizeof(hizConstants));

                    cmd->SetComputePipelineState(m_Engine.m_HiZCopyPipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_Engine.m_HiZConstantBuffer.get());
                    cmd->SetComputeTexture(0, m_Engine.m_RenderTargets.GBufferDepth.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_Engine.m_RenderTargets.HiZTexture.get(), 0);
                    cmd->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);

                    cmd->SetComputePipelineState(m_Engine.m_HiZDownsamplePipelineState.get());
                    uint32_t hizSrcWidth = renderWidth;
                    uint32_t hizSrcHeight = renderHeight;
                    for (uint32_t mip = 1; mip < m_Engine.m_HiZMipLevels; ++mip)
                    {
                        const uint32_t hizDstWidth = std::max(1u, hizSrcWidth / 2);
                        const uint32_t hizDstHeight = std::max(1u, hizSrcHeight / 2);

                        hizConstants.SrcSize = { hizSrcWidth, hizSrcHeight };
                        hizConstants.DstSize = { hizDstWidth, hizDstHeight };
                        cmd->UpdateBuffer(m_Engine.m_HiZConstantBuffer.get(), &hizConstants, sizeof(hizConstants));
                        cmd->SetComputeConstantBuffer(0, m_Engine.m_HiZConstantBuffer.get());
                        cmd->SetComputeUnorderedAccessTexture(0, m_Engine.m_RenderTargets.HiZTexture.get(), mip - 1);
                        cmd->SetComputeUnorderedAccessTexture(1, m_Engine.m_RenderTargets.HiZTexture.get(), mip);
                        cmd->Dispatch((hizDstWidth + 7) / 8, (hizDstHeight + 7) / 8, 1);

                        hizSrcWidth = hizDstWidth;
                        hizSrcHeight = hizDstHeight;
                    }

                    // ここまで来たら全ミップに実データが入った。
                    // 【Executeの中で立てること】パスの登録だけでは実行されたことにならない
                    m_Engine.m_HiZValid = true;
                },
            });
        };

        // 深度プリパスぶんの判定。**プリパスより前**でなければ間接描画の引数が間に合わない。
        // 読めるHi-Zは前フレームのものなので、保守的に膨らませる従来の経路のまま
        addModelCullPass(
            "ModelCull", 0u,
            hiZFromDepthPrepass ? m_Engine.m_ModelCullPrepassCandidateCount : m_Engine.m_ModelCullCandidateCount,
            /*initializeBuffers=*/true, /*useCurrentFrameHiZ=*/false, occlusionPrevFrameEnabled);

        if (depthPrepassRuns)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "DepthPrepass",
                // 増幅シェーダーのHi-Zオクルージョンカリングが読む
                // (G-Bufferパスと同じ理由で循環にはならない)
                .Reads = { m_Engine.m_RenderTargets.HiZTexture.get() },
                // レンダーターゲットは持たない(深度だけを書く)
                .DepthTarget = m_Engine.m_RenderTargets.GBufferDepth.get(),
                // 間接描画の引数(直前のModelCullパスが書いたもの)
                .BufferReads = { m_Engine.m_ModelCullDrawArgsBuffer.get() },
                .Execute = [this, gbufferViewport, &viewProj, modelCullIndirectActive, occlusionCullingActive, frameConstantBuffer, objectConstantBuffer, materialSamplers](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(gbufferViewport);
                    // Reverse-Zのため遠平面側(NDC z=0.0)。G-Bufferパスの代わりにここでクリアする
                    cmd->ClearDepth(0.0f);

                    // 増幅シェーダーのHi-Zオクルージョンカリング用(t8。GBufferMeshlet.hlsl)。
                    //
                    // 【プリパスでも張ること】このパスはG-Bufferとまったく同じ増幅シェーダーを
                    // 使うので、同じ判定を同じ入力で行わなければならない。
                    // **張り忘れてもエラーは出ない** ―― SRVのバインドはコマンドリスト側が
                    // シャドウで保持しており、別のパスが8番へ張ったテクスチャがそのまま残る。
                    // そうなるとプリパスとG-Bufferで間引くメッシュレットが食い違い、
                    // プリパスだけが描いた面は「深度はあるのに色が無い」穴になる
                    if (occlusionCullingActive)
                    {
                        cmd->SetTextureAllStages(8, m_Engine.m_RenderTargets.HiZTexture.get());
                    }

                    RHI::IRHIPipelineState* currentPipelineState = nullptr;

                    // 1モデル1ドロー経路ぶんは、GPUが間引いた結果をそのまま発行する。
                    // 【下のCPUループより先に出す】どちらが先でも深度テストが前後関係を
                    // 決めるので絵は変わらないが、件数の多いこちらを先に流すほうが
                    // 後続のCPUループがGPUの実行と重なる
                    if (modelCullIndirectActive)
                    {
                        if (m_Engine.IssueModelCullIndirect(
                                cmd, kModelCullRegionPrepassOpaque, m_Engine.m_DepthPrepassMeshletPipelineState.get(),
                                currentPipelineState))
                        {
                            ++m_Engine.m_DrawCallsDepthPrepass;
                        }
                        if (m_Engine.IssueModelCullIndirect(
                                cmd, kModelCullRegionPrepassOpaqueMirrored,
                                m_Engine.m_DepthPrepassMeshletPipelineStateMirrored.get(), currentPipelineState))
                        {
                            ++m_Engine.m_DrawCallsDepthPrepass;
                        }
                        if (m_Engine.IssueModelCullIndirect(
                                cmd, kModelCullRegionPrepassCutout,
                                m_Engine.m_DepthPrepassMeshletCutoutPipelineState.get(), currentPipelineState))
                        {
                            ++m_Engine.m_DrawCallsDepthPrepass;
                        }
                        if (m_Engine.IssueModelCullIndirect(
                                cmd, kModelCullRegionPrepassCutoutMirrored,
                                m_Engine.m_DepthPrepassMeshletCutoutPipelineStateMirrored.get(), currentPipelineState))
                        {
                            ++m_Engine.m_DrawCallsDepthPrepass;
                        }
                    }
                    // G-Bufferと同じカメラなので、間引かれるモデルも同じになる
                    const Rendering::FrustumPlanes prepassFrustum = ExtractFrustumPlanes(viewProj);

                    // 【G-Bufferとまったく同じ組を描く】列挙・錐台カリング・段の選択・
                    // メッシュ単位のカリングはForEachGeometryDrawが行い、G-Bufferパスは
                    // まったく同じ引数で同じ関数を呼ぶ。組が食い違うと「深度は書かれているのに
                    // 色が書かれない」穴が開くが、それは絵を見ても気づけない ――
                    // だから手で揃えるのをやめ、同じ関数を通ることで揃うようにしてある
                    Rendering::GeometryDrawLoopDesc prepassLoop;
                    prepassLoop.Frustum = &prepassFrustum;
                    prepassLoop.LODMode = Rendering::GeometryLODMode::Fade;
                    prepassLoop.MeshFilter = Rendering::GeometryMeshFilter::Opaque;

                    m_Engine.ForEachGeometryDraw(
                        prepassLoop,
                        [&](const Rendering::InstanceDrawUnit& unit, const Assets::Model& lodModel,
                            float lodDitherFade)
                        {
                            const Assets::ModelInstance& instance = *unit.Instance;

                            // G-Bufferが1ドローで描くモデルは、プリパスも同じ増幅/メッシュシェーダーで
                            // 描く。**同じ判断関数(ShouldUseModelMeshletPath)で経路を選ぶことが要点**で、
                            // 片方だけがメッシュシェーダーになると深度が一致しない。
                            //
                            // 不透明とカットアウトでピクセルシェーダーの有無が変わるため、
                            // カットアウトのマテリアルを持つモデルだけ2回に分ける。
                            // 持たないモデル(PLATEAUのタイルがそう)は1回で済む
                            if (!m_Engine.ShouldUseModelMeshletPath(instance, lodModel))
                            {
                                return false;
                            }

                            // 間接描画が有効なら、この経路のドローはこのループの前に
                            // まとめて発行済み。**ここで描くと二重になる**
                            if (modelCullIndirectActive)
                            {
                                return true;
                            }

                            // 【フェード中は1ドロー経路のプリパスを外す】不透明用のPSOは
                            // ピクセルシェーダーを持たないためApplyLODDitherを通せず、
                            // 捨てるはずの画素まで深度を書いてG-Bufferとの食い違いで穴が開く。
                            // 早期Zが効かなくなるだけで、G-Buffer側が深度を書くので絵は壊れない
                            if (lodDitherFade < 1.0f)
                            {
                                return true;
                            }

                            if (!m_Engine.m_DepthPrepassMeshletPipelineState)
                            {
                                // メッシュレット版のPSOが無い。このモデルはプリパスから外す
                                // (早期Zが効かないだけで、G-Buffer側が深度を書くので絵は壊れない)
                                return true;
                            }

                            const uint32_t groupCount =
                                (lodModel.TotalMeshletCount
                                 + ShaderInterop::kAmplificationGroupSize - 1)
                                / ShaderInterop::kAmplificationGroupSize;

                            const auto dispatchMeshletPrepass =
                                [&](RHI::IRHIPipelineState* pipelineState, uint32_t rejectMask, uint32_t requireMask)
                            {
                                if (!pipelineState)
                                {
                                    return;
                                }
                                if (pipelineState != currentPipelineState)
                                {
                                    cmd->SetPipelineState(pipelineState);
                                    cmd->SetConstantBuffer(0, frameConstantBuffer);
                                    cmd->SetSamplerSet(materialSamplers);
                                    currentPipelineState = pipelineState;
                                }

                                const ObjectConstants objectConstants = MakeModelObjectConstants(
                                    instance, lodModel, m_Engine.m_EmissiveLightSettings.Intensity, m_Engine.m_AmbientOcclusionSettings.OcclusionMapEnabled, rejectMask, requireMask,
                                    m_Engine.m_MeshletLODFrame);
                                cmd->UpdateBuffer(
                                    objectConstantBuffer, &objectConstants, sizeof(objectConstants));
                                cmd->SetConstantBuffer(1, objectConstantBuffer);
                                cmd->DispatchMesh(groupCount, 1, 1);
                                ++m_Engine.m_DrawCallsDepthPrepass;
                            };

                            // 不透明ぶん(ピクセルシェーダー無し)。半透明とカットアウトを落とす
                            dispatchMeshletPrepass(
                                instance.IsMirrored ? m_Engine.m_DepthPrepassMeshletPipelineStateMirrored.get()
                                                    : m_Engine.m_DepthPrepassMeshletPipelineState.get(),
                                Assets::kGpuMaterialFlagTransparent | Assets::kGpuMaterialFlagCutout, 0);

                            // カットアウトぶん(clipを通す)。持たないモデルではこの回は発行しない
                            if (lodModel.HasCutoutMaterial)
                            {
                                dispatchMeshletPrepass(
                                    instance.IsMirrored ? m_Engine.m_DepthPrepassMeshletCutoutPipelineStateMirrored.get()
                                                        : m_Engine.m_DepthPrepassMeshletCutoutPipelineState.get(),
                                    Assets::kGpuMaterialFlagTransparent, Assets::kGpuMaterialFlagCutout);
                            }
                            return true;
                        },
                        [&](const Rendering::InstanceDrawUnit& unit, const Assets::Model& lodModel,
                            const Assets::Mesh& mesh, float lodDitherFade)
                        {
                            const Assets::ModelInstance& instance = *unit.Instance;

                            // カットアウトは切り抜きを反映しないと深度に嘘が入る。
                            // ミラーリングは表裏判定が逆のPSOでないとカリングされる面が入れ替わり、
                            // G-Bufferと違う深度になってしまう。
                            // 【LODのフェード中もピクセルシェーダーが要る】カットアウトが無くても
                            // クロスディザで捨てる画素があるため、PS無しのPSOでは抜けない
                            const bool cutout = mesh.AlphaCutoff > 0.0f || lodDitherFade < 1.0f;
                            RHI::IRHIPipelineState* const wanted =
                                cutout ? (instance.IsMirrored ? m_Engine.m_DepthPrepassCutoutPipelineStateMirrored.get()
                                                              : m_Engine.m_DepthPrepassCutoutPipelineState.get())
                                       : (instance.IsMirrored ? m_Engine.m_DepthPrepassPipelineStateMirrored.get()
                                                              : m_Engine.m_DepthPrepassPipelineState.get());
                            if (wanted != currentPipelineState)
                            {
                                cmd->SetPipelineState(wanted);
                                cmd->SetConstantBuffer(0, frameConstantBuffer);
                                cmd->SetSamplerSet(materialSamplers);
                                currentPipelineState = wanted;
                            }

                            ObjectConstants objectConstants =
                                MakeObjectConstants(instance, lodModel, mesh, m_Engine.m_EmissiveLightSettings.Intensity, m_Engine.m_AmbientOcclusionSettings.OcclusionMapEnabled, m_Engine.m_MeshletLODFrame, lodDitherFade);
                            objectConstants.InstanceBase = unit.InstanceBase;
                            objectConstants.InstancingEnabled = unit.IsBatch() ? 1u : 0u;
                            cmd->UpdateBuffer(objectConstantBuffer, &objectConstants, sizeof(objectConstants));
                            cmd->SetConstantBuffer(1, objectConstantBuffer);

                            // カットアウト以外はピクセルシェーダーを持たないためテクスチャも要らない
                            if (cutout)
                            {
                                cmd->SetTexture(0, mesh.BaseColorTexture);
                            }

                            // 【毎回張り直す】頂点シェーダー用SRVはt0の1本しかなく、
                            // ドローンショーが同じスロットを使う
                            if (unit.IsBatch())
                            {
                                cmd->SetVertexShaderResourceBuffer(0, m_Engine.m_ModelInstanceBuffer.get());
                            }

                            cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                            cmd->SetIndexBuffer(mesh.IndexBuffer.get());
                            cmd->DrawIndexed(mesh.IndexCount, 0, 0, unit.InstanceCount);
                            ++m_Engine.m_DrawCallsDepthPrepass;
                            return true;
                        });
                },
            });
        }

        // 深度プリパスが書いた深度からHi-Zを作り、**今フレームのHi-Z**でG-Bufferぶんを判定する。
        // ここまで来ればプリパスは全不透明ジオメトリの深度を書き終えている ――
        // 1フレーム遅れも保守的な膨張も要らず、判定はそのまま正確になる。
        //
        // 【プリパスの深度は G-Buffer が描くものの部分集合】LODのフェード中のモデルなど、
        // プリパスから外れるものがある。遮蔽物が減る方向なので間引きが甘くなるだけで、
        // 見えているものを消す側へは倒れない
        if (hiZFromDepthPrepass)
        {
            addHiZPass();
            addModelCullPass(
                "ModelCullGBuffer", m_Engine.m_ModelCullPrepassCandidateCount,
                m_Engine.m_ModelCullCandidateCount - m_Engine.m_ModelCullPrepassCandidateCount,
                /*initializeBuffers=*/false, /*useCurrentFrameHiZ=*/true, occlusionCullingActive);
        }

        // --- ジオメトリパス: G-Bufferへ書き込む(常に指定した内部解像度) ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "GBuffer",
            // 増幅シェーダーのHi-Zオクルージョンカリングが読む(Stage 5-2)。
            //
            // 【循環にならない】Hi-Zパスは「G-Buffer深度を読んでHi-Zを書く」ので、依存だけ見ると
            // 互いを参照しているように見える。しかしRenderGraphのReadsは「それより前に登録された
            // 書き手」がいるときにだけ辺を張る規則で、Hi-Zパスの登録はこのパスより後なので
            // 辺は張られない(RenderGraph::ResolveExecutionOrder)。実行順も登録順のまま、
            // 読むのは前フレームに書かれた内容になる ―― それがこの判定の前提そのもの
            .Reads = { m_Engine.m_RenderTargets.HiZTexture.get() },
            // 深度プリパス(直前に登録される)を通したときは、ここへ来る時点で深度が埋まっており、
            // PSOのDepthAllowEqual(GREATER_EQUAL)によって最前面の断片だけがテストを通る。
            //
            // 6枚目のbent normalまで含め、並びはGBuffer.hlslのPSOutputおよび
            // CreatePrecisionDependentPipelineStatesのRenderTargetFormatsと一致させること
            .RenderTargets = { m_Engine.m_RenderTargets.GBufferAlbedo.get(), m_Engine.m_RenderTargets.GBufferNormal.get(), m_Engine.m_RenderTargets.GBufferMaterial.get(),
                               m_Engine.m_RenderTargets.GBufferEmissive.get(), m_Engine.m_RenderTargets.GBufferVelocity.get(), m_Engine.m_RenderTargets.GBufferBentNormal.get() },
            .DepthTarget = m_Engine.m_RenderTargets.GBufferDepth.get(),
            // 間接描画の引数を読む(ModelCullパスが書いたもの)
            .BufferReads = { m_Engine.m_ModelCullDrawArgsBuffer.get() },
            .Execute = [this, gbufferViewport, depthPrepassRuns, &viewProj, occlusionCullingActive, meshletCullStatsActive, modelCullIndirectActive, frameConstantBuffer, objectConstantBuffer, materialSamplers](RHI::IRHICommandList* cmd)
            {
                // カリング統計のカウンタを0へ戻す。増幅シェーダーは加算しかしないので、
                // 戻さないとフレームをまたいで積み上がる。
                //
                // 【このパスの中で行う理由】数えるのも読み戻すのもこのパスなので、
                // 「クリア→数える→コピー」を1か所にまとめたほうが順序を追いやすい。
                // 別パスに分けるとRenderGraphの登録順への依存が1本増える
                if (meshletCullStatsActive)
                {
                    cmd->ClearUnorderedAccessBufferUint(m_Engine.m_MeshletCullStatsBuffer.get(), 0);
                }

                cmd->SetViewport(gbufferViewport);
                // ClearRenderTargetはバインド済みの全レンダーターゲットを同じ色でクリアするため、
                // 速度バッファもここで0(=動いていない)になる。ジオメトリが描かれない画素
                // (空)の速度は0のまま残るが、空はカメラ回転で動くのでTAA側で別途補う(TAA.hlsl参照)
                cmd->ClearRenderTarget({ 0.0f, 0.0f, 0.0f, 0.0f });
                // Reverse-Zのため遠平面側(NDC z=0.0)にクリアする(GBuffer.hlsl参照)。
                // 【深度プリパスを通したときはクリアしない】プリパスが既に正しい深度を
                // 書いており、ここで消すとGREATER_EQUALのテストが全断片を通してしまい
                // プリパスが無意味になる(クリアはプリパス側が行う)
                if (!depthPrepassRuns)
                {
                    cmd->ClearDepth(0.0f);
                }

                cmd->SetPipelineState(m_Engine.m_GBufferPipelineState.get());
                cmd->SetConstantBuffer(0, frameConstantBuffer);
                cmd->SetSamplerSet(materialSamplers);

                // 増幅シェーダーのHi-Zオクルージョンカリング用(t8。GBufferMeshlet.hlsl)。
                //
                // 【SetTextureではなくSetTextureAllStagesを使う】SetTextureはリソースを
                // PIXEL_SHADER_RESOURCEへしか遷移させず、増幅シェーダーからは読めない。
                //
                // 【パスの先頭で1回だけでよい】SRVのバインドはDX12CommandList側がシャドウで
                // 保持しており、SetPipelineStateで無効化されるのはルート引数だけで、
                // 次のDrawの直前にFlushPendingSrvWritesが張り直す。
                // ここでPSOを切り替えても、下のbindPipelineStateがCBVとサンプラーを
                // 張り直すのと違って、テクスチャは張り直す必要が無い。
                //
                // 【判定しないフレームではバインドもしない】不要な状態遷移を1つ減らすと同時に、
                // 「バインドされていないのに間引き率が出た」という取り違えを起こせなくする
                if (occlusionCullingActive)
                {
                    cmd->SetTextureAllStages(8, m_Engine.m_RenderTargets.HiZTexture.get());
                }

                // ミラーリング(Worldの行列式が負)されたインスタンス・水面(ModelInstance::IsWater)
                // インスタンスの組み合わせ(4通り)に応じてパイプラインを切り替える。上で通常の
                // パイプラインを先にバインドしてあるため、どちらも含まないシーンでは以下のラムダは
                // 一度も切り替えを行わず、発行されるコマンド列はこの機能の追加前と完全に同一になる。
                // DX12のSetPipelineStateはルートシグネチャを張り直して既存のバインドを
                // 無効化するので、切り替えたときはパス共通のバインドもやり直す
                //
                // メッシュレット経路(useMeshlet)はさらにその上の分岐。頂点シェーダー版と
                // 同じG-Bufferへ同じ内容を書くので、切り替えても見た目は一致する
                RHI::IRHIPipelineState* currentPipelineState = m_Engine.m_GBufferPipelineState.get();
                const auto bindPipelineState = [&](bool mirrored, bool water, bool useMeshlet)
                {
                    RHI::IRHIPipelineState* wanted = nullptr;
                    if (useMeshlet)
                    {
                        // デバッグ表示が有効ならメッシュレットごとの色分けPSOを使う。
                        // 用意できていない場合(作成失敗)は通常のメッシュレットPSOへ落とす
                        const bool debugView = m_Engine.m_GeometrySettings.MeshletDebugViewEnabled && m_Engine.m_GBufferMeshletDebugPipelineState;
                        wanted = debugView
                            ? (mirrored ? m_Engine.m_GBufferMeshletDebugPipelineStateMirrored.get()
                                        : m_Engine.m_GBufferMeshletDebugPipelineState.get())
                            : (mirrored ? m_Engine.m_GBufferMeshletPipelineStateMirrored.get()
                                        : m_Engine.m_GBufferMeshletPipelineState.get());
                    }
                    else
                    {
                        wanted = water
                            ? (mirrored ? m_Engine.m_GBufferWaterPipelineStateMirrored.get() : m_Engine.m_GBufferWaterPipelineState.get())
                            : (mirrored ? m_Engine.m_GBufferPipelineStateMirrored.get() : m_Engine.m_GBufferPipelineState.get());
                    }
                    if (wanted == currentPipelineState)
                    {
                        return;
                    }
                    cmd->SetPipelineState(wanted);
                    cmd->SetConstantBuffer(0, frameConstantBuffer);
                    cmd->SetSamplerSet(materialSamplers);
                    currentPipelineState = wanted;
                };

                // 視錐台の外にあるモデルは丸ごと飛ばし、通ったモデルの中でさらに
                // 視錐台の外にあるメッシュを飛ばす(下のメッシュのループ)。
                //
                // 【2段になっている理由】モデル単位だけだと、1モデルに多数のメッシュを持つ
                // アセット(Bistro、Emerald Square、PLATEAUのLOD2タイル)では1つも間引けない
                // ―― モデル全体のAABBが視錐台と交差する限り全メッシュを描くしかないため。
                // .kmodel v10がメッシュ単位のAABBを持つようになったので、もう一段入れてある。
                // 効くシーンが逆(モデル単位は.kmodelを多数並べるシーンで効く)なので、
                // 統計も別のカウンタで数える
                const FrustumPlanes frustum = ExtractFrustumPlanes(viewProj);

                // 1モデル1ドロー経路ぶんは、GPUが間引いた結果をそのまま発行する。
                // 【下のCPUループより先に出す】どちらが先でも深度テストが前後関係を
                // 決めるので絵は変わらないが、件数の多いこちらを先に流すほうが
                // 後続のCPUループがGPUの実行と重なる。
                // 選ぶPSOはbindPipelineState(mirrored, false, true)と同じもの
                if (modelCullIndirectActive)
                {
                    const bool meshletDebug = m_Engine.m_GeometrySettings.MeshletDebugViewEnabled && m_Engine.m_GBufferMeshletDebugPipelineState;
                    if (m_Engine.IssueModelCullIndirect(
                            cmd, kModelCullRegionGBuffer,
                            meshletDebug ? m_Engine.m_GBufferMeshletDebugPipelineState.get()
                                         : m_Engine.m_GBufferMeshletPipelineState.get(),
                            currentPipelineState))
                    {
                        ++m_Engine.m_DrawCallsGBuffer;
                    }
                    if (m_Engine.IssueModelCullIndirect(
                            cmd, kModelCullRegionGBufferMirrored,
                            meshletDebug ? m_Engine.m_GBufferMeshletDebugPipelineStateMirrored.get()
                                         : m_Engine.m_GBufferMeshletPipelineStateMirrored.get(),
                            currentPipelineState))
                    {
                        ++m_Engine.m_DrawCallsGBuffer;
                    }
                }

                // インスタンシングのバッチと、まとめられなかった1体を同じ形で回す。
                // 【深度プリパスとまったく同じ組を使う】引数が同じなら同じ組になる。
                // まとめ方が食い違うと穴が開くので、ここは必ずプリパス側と同じ形で書くこと
                Rendering::GeometryDrawLoopDesc gbufferLoop;
                gbufferLoop.Frustum = &frustum;
                gbufferLoop.LODMode = Rendering::GeometryLODMode::Fade;
                gbufferLoop.MeshFilter = Rendering::GeometryMeshFilter::Opaque;

                m_Engine.ForEachGeometryDraw(
                    gbufferLoop,
                    [&](const Rendering::InstanceDrawUnit& unit, const Assets::Model& lodModel, float lodDitherFade)
                    {
                        const Assets::ModelInstance& instance = *unit.Instance;

                        // モデル全体を1回のDispatchMeshで描ける場合はメッシュのループへ入らない。
                        // マテリアルはメッシュシェーダーが出力した番号でピクセルシェーダーが引くため、
                        // メッシュごとのSetTextureも定数バッファの更新も要らない。
                        //
                        // 【BLENDだけは増幅シェーダーが落とす】半透明はG-Bufferに書かず専用の
                        // Transparentパスでフォワードシェーディングする。ドローを分けられない以上、
                        // メッシュレット単位のふるい分けでしか除外できない
                        if (!m_Engine.ShouldUseModelMeshletPath(instance, lodModel))
                        {
                            return false;
                        }

                        // 間接描画が有効なら、この経路のドローはこのループの前に
                        // まとめて発行済み。**ここで描くと二重になる**
                        if (modelCullIndirectActive)
                        {
                            return true;
                        }

                        bindPipelineState(instance.IsMirrored, false, true);

                        const ObjectConstants objectConstants = MakeModelObjectConstants(
                            instance, lodModel, m_Engine.m_EmissiveLightSettings.Intensity, m_Engine.m_AmbientOcclusionSettings.OcclusionMapEnabled,
                            Assets::kGpuMaterialFlagTransparent, 0, m_Engine.m_MeshletLODFrame,
                            /*countCullStats=*/true, lodDitherFade);
                        cmd->UpdateBuffer(objectConstantBuffer, &objectConstants, sizeof(objectConstants));
                        cmd->SetConstantBuffer(1, objectConstantBuffer);

                        // 起動するのは「モデル全体のメッシュレット数 ÷ 増幅シェーダーのグループサイズ」。
                        // 実際にラスタライズされるのはカリングとふるい分けを生き延びたぶんに絞られる
                        const uint32_t groupCount = (lodModel.TotalMeshletCount
                            + ShaderInterop::kAmplificationGroupSize - 1) / ShaderInterop::kAmplificationGroupSize;
                        cmd->DispatchMesh(groupCount, 1, 1);
                        ++m_Engine.m_DrawCallsGBuffer;
                        return true;
                    },
                    [&](const Rendering::InstanceDrawUnit& unit, const Assets::Model& lodModel,
                        const Assets::Mesh& mesh, float lodDitherFade)
                    {
                        const Assets::ModelInstance& instance = *unit.Instance;

                        // 【ここへ来た時点でメッシュレット経路は使わない】上のモデル単位の
                        // 判定を通らなかったインスタンス(水面、メッシュレットを持たない
                        // メッシュが混ざるモデル、メッシュレット描画が無効)なので、
                        // モデル全体を従来の頂点シェーダーで描く。
                        // **メッシュ単位でメッシュレット経路へ入れてはいけない** ――
                        // 深度プリパスは同じ判断関数(ShouldUseModelMeshletPath)で経路を選ぶため、
                        // ここで食い違うとプリパスの深度とG-Bufferの深度が一致しなくなる
                        bindPipelineState(instance.IsMirrored, instance.IsWater, false);

                        ObjectConstants objectConstants =
                            MakeObjectConstants(instance, lodModel, mesh, m_Engine.m_EmissiveLightSettings.Intensity, m_Engine.m_AmbientOcclusionSettings.OcclusionMapEnabled, m_Engine.m_MeshletLODFrame, lodDitherFade);
                        objectConstants.InstanceBase = unit.InstanceBase;
                        objectConstants.InstancingEnabled = unit.IsBatch() ? 1u : 0u;
                        cmd->UpdateBuffer(objectConstantBuffer, &objectConstants, sizeof(objectConstants));
                        cmd->SetConstantBuffer(1, objectConstantBuffer);

                        cmd->SetTexture(0, mesh.BaseColorTexture);
                        cmd->SetTexture(1, mesh.NormalTexture);
                        cmd->SetTexture(2, mesh.MetallicRoughnessTexture);
                        cmd->SetTexture(3, mesh.EmissiveTexture);
                        cmd->SetTexture(5, mesh.OcclusionTexture);
                        cmd->SetTexture(6, mesh.BentNormalTexture);
                        if (instance.IsWater)
                        {
                            // Water.hlslのPSMainだけが読むt7。通常のGBuffer PSOはt7を宣言していないため
                            // 水面以外のインスタンスではバインドしない。
                            // 【t6は使えない】t6はbent normal(34章)が使う
                            cmd->SetTexture(7, m_Engine.m_WaterNormalMapTexture.get());
                        }

                        // 【毎回張り直す】頂点シェーダー用SRVはt0の1本しかなく、
                        // ドローンショーが同じスロットを使う
                        if (unit.IsBatch())
                        {
                            cmd->SetVertexShaderResourceBuffer(0, m_Engine.m_ModelInstanceBuffer.get());
                        }

                        cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                        cmd->SetIndexBuffer(mesh.IndexBuffer.get());
                        cmd->DrawIndexed(mesh.IndexCount, 0, 0, unit.InstanceCount);
                        ++m_Engine.m_DrawCallsGBuffer;
                        return true;
                    });

                // 数え終わったカウンタを受け皿へ写す。読むのは数フレーム後(下のリングの説明参照)。
                //
                // 【全描画の後で行うこと】ここより前に置くと、まだ発行していない
                // DispatchMeshの寄与が入らない。コピーはUNORDERED_ACCESS→COPY_SOURCEの
                // 状態遷移を伴い、その遷移が増幅シェーダーの書き込み完了も保証する
                if (meshletCullStatsActive)
                {
                    cmd->CopyBufferToReadback(
                        m_Engine.m_MeshletCullStatsReadback[m_Engine.m_MeshletCullStatsRingIndex].get(),
                        m_Engine.m_MeshletCullStatsBuffer.get(),
                        static_cast<uint32_t>(sizeof(uint32_t)) * kMeshletCullStatsCount);
                }
            },
        });

        // --- モデル単位GPUカリングのカウンタを受け皿へ写すパス ---
        //
        // 【ディスパッチと同じパスに置かない・すぐ後ろにも置かない】
        // UNORDERED_ACCESS→COPY_SOURCE の遷移は直前のUAV書き込みを流し切る。
        // ディスパッチの直後に置くと、その待ちがModelCullパスのGPU時間に乗って
        // 「671スレッドの判定に1ms」というありえない値に見える。
        // G-Bufferパスより後ろに置けば、待ちは描画と重なって消える
        // (メッシュレット統計のコピーが重いG-Bufferパスの末尾にあって
        //  表面化していないのと同じ構図)。
        //
        // **別パスにしてあるので、その待ち自体も独立した数値として見られる**
        if (modelCullReady)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "ModelCullReadback",
                .BufferReads = { m_Engine.m_ModelCullCounterBuffer.get() },
                .Execute = [this](RHI::IRHICommandList* cmd)
                {
                    cmd->CopyBufferToReadback(
                        m_Engine.m_ModelCullReadback[m_Engine.m_ModelCullRingIndex].get(), m_Engine.m_ModelCullCounterBuffer.get(),
                        static_cast<uint32_t>(sizeof(uint32_t)) * kModelCullCounterCount);
                },
            });
        }

        // --- 自前ソフトウェアラスタライザパス(46章): 三角形をコンピュートシェーダーで
        //     ラスタライズし、専用のバッファへ深度・法線・フラット陰影を書く ---
        //
        // 【既存の描画には一切寄与しない】比較用の独立した経路で、出力を読むのは
        // DebugView::SoftwareRaster* だけ。GBufferパスの直後に登録しているのは、
        // 依存関係が無いパス同士の実行順が登録順で決まるため(プロファイラの並びを揃える)。
        //
        // 【なぜハードウェアと比べられるのか】GBufferパスとまったく同じjitteredProjを渡すため、
        // 深度は丸め誤差とフィルルールの差を除いて一致するはず。差が面全体に出たら
        // 座標変換の間違いで、シルエットの±1画素ならフィルルールの差(想定内)
        const bool softwareRasterPassRuns = frame.Settings.Geometry.SoftwareRasterEnabled && m_Engine.m_RenderCapabilities.SoftwareRasterAvailable &&
                                            m_Engine.m_SoftwareRasterVisibilityBuffer && !m_Engine.m_Scene.Instances.empty();
        bb.SoftwareRasterPassRuns = softwareRasterPassRuns;
        if (softwareRasterPassRuns)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "SWRaster",
                .Writes = { m_Engine.m_SoftwareRasterColor.get(), m_Engine.m_SoftwareRasterDepth.get(), m_Engine.m_SoftwareRasterNormal.get() },
                .BufferReads = { m_Engine.m_SoftwareRasterMeshInfoBuffer.get() },
                .BufferWrites = { m_Engine.m_SoftwareRasterVisibilityBuffer.get(),
                                  m_Engine.m_SoftwareRasterLargeEntriesBuffer.get(),
                                  m_Engine.m_SoftwareRasterIndirectArgsBuffer.get() },
                .Execute = [this, viewProj, sunLighting](RHI::IRHICommandList* cmd)
                {
                    m_Engine.ExecuteSoftwareRasterPass(cmd, viewProj, sunLighting.Direction);
                },
            });
        }

        // --- Hi-Zミップチェーン構築パス: G-Buffer深度から1x1までのミップチェーンをコンピュートシェーダーで
        //     構築する ---
        //
        // 【消費者がいるフレームだけ登録する】このパスは「コピー1回 + ミップ段数-1回のディスパッチ」
        // (1280x720なら計11回)で、Intel UHD 620での実測で1.19〜1.21ms、GPUフレーム時間30msの
        // 約4%を占める。以前は消費者がPresentパスのDebugView::HiZ表示しか無かったため、
        // その表示中だけに絞ってこの4%を削った経緯がある。
        //
        // Stage 5-2で2人目の消費者(増幅シェーダーのオクルージョンカリング)が付いたが、
        // **条件を丸ごと外してはいけない** ―― 外すと上の節約がそのまま戻る。
        // 「消費者がいるフレームか」へ条件を書き換えるのが正しい。メッシュシェーダー非対応の
        // 環境ではocclusionCullingActiveが常にfalseになり、従来どおり1msを払わずに済む。
        //
        // 【1フレーム遅れになる】このパスはGBufferパスより後に登録されるため、増幅シェーダーが
        // 読むのは前フレームのHi-Zになる。判定側はそれを前提に、前フレームのビュー射影行列
        // (FrameConstants::PrevViewProj)で投影し、球を保守的に膨らませて吸収している
        // (GBufferMeshlet.hlslのIsMeshletOccluded参照)
        // Hi-Zミップチェーンの構築。**このパスは条件付きで走らせる。**
        // 1280x720ならコピー1回+ミップ段数-1回のディスパッチ(計11回)で、Intel UHD 620での
        // 実測で1.19〜1.21ms、GPUフレーム時間30msの約4%を占める。消費者がいないフレームでは
        // その4%をまるごと捨てることになるので、条件を丸ごと外してはいけない。
        //
        // 【登録する場所は2つある】深度プリパスが走るなら**その直後**に登録済みで
        // (hiZFromDepthPrepass。今フレームの深度から作り、同じフレームのG-Bufferが読む)、
        // ここへは来ない。プリパスが走らないフレームだけ、従来どおりG-Bufferの後で作る ――
        // その場合に読めるのは次フレームで、増幅シェーダーは前フレームのビュー射影行列で
        // 投影し、球を保守的に膨らませて視差を吸収する(GBufferMeshlet.hlslのIsMeshletOccluded)
        const bool hiZPassRuns = (frame.Settings.DebugView.View == DebugView::HiZ) || occlusionCullingActive;
        if (!hiZPassRuns)
        {
            // このフレームで作らないなら、次フレームのHi-Zは「何フレームか前の、別のカメラ位置で
            // 撮った深度」になる。それで遮蔽を判定すると見えているものを消す。
            // 【トグルを往復させると必ず起きる】メッシュレット描画やオクルージョンを一度OFFにして
            // ONへ戻す操作で踏むので、"構築しなかった"を必ず記録しておく
            m_Engine.m_HiZValid = false;
        }
        if (hiZPassRuns && !hiZFromDepthPrepass)
        {
            addHiZPass();
        }
    }
}
