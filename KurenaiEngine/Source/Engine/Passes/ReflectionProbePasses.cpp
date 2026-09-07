#include "../KurenaiEngine3D.h"

#include <algorithm>
#include <vector>

#include "Core/Logger.h"
#include "Core/RenderGraph.h"
#include "ReflectionProbePasses.h"
#include "EnvironmentConstants.h"
#include "ReflectionProbeConstants.h"
#include "../Rendering/CubeFaceMath.h"
#include "../Rendering/GeometryDrawLoop.h"
#include "../Rendering/ObjectConstants.h"
#include "../Rendering/RenderBlackboard.h"
#include "../Rendering/RenderFrameContext.h"
#include "../ShaderInterop/CascadeConstants.h"
#include "../ShaderInterop/FrameConstants.h"
#include "../ShaderInterop/GroupSizes.h"

namespace Kurenai::Passes
{
    namespace
    {
        using Rendering::FrustumPlanes;
        using Rendering::ExtractFrustumPlanes;
        using ShaderInterop::CascadeConstants;
        using ShaderInterop::FrameConstants;
    }

    void ReflectionProbePasses::Register(
        Core::RenderGraph& graph,
        const Rendering::RenderFrameContext& frame,
        const Rendering::RenderBlackboard& bb)
    {
        // 【フレームの写しをローカルで受ける】frame自体はラムダへ捕捉しない
        const Rendering::RenderTargets* const targets = frame.Targets;

        // 【フレームの写しをローカルで受ける】frame自体はラムダへ捕捉しない
        RHI::IRHIBuffer* const lightBuffer = frame.Scene->LightBuffer.get();
        RHI::IRHIBuffer* const modelInstanceBuffer = frame.Scene->ModelInstanceBuffer.get();

        // 【フレームの写しをローカルで受ける】frame自体はラムダへ捕捉しない
        RHI::IRHITexture* const brdfLUTTexture = frame.IBL->BRDFLUTTexture.get();
        RHI::IRHIBuffer* const iblPrefilterConstantBuffer = frame.IBL->PrefilterConstantBuffer.get();
        RHI::IRHITexture* const irradianceTexture = frame.IBL->IrradianceTexture.get();
        RHI::IRHITexture* const prefilteredEnvTexture = frame.IBL->PrefilteredEnvTexture.get();

        // 【フレームの写しをローカルで受ける】ラムダへ値で渡すため
        const float effectiveExposureEV100 = frame.EffectiveExposureEV100;
        const MeshletLODFrameConstants meshletLOD = frame.MeshletLOD;

        // 【ラムダへ値で渡すためローカルへ受け直す】frame そのものは捕捉しない作法
        // (Rendering/RenderFrameContext.h の冒頭)。設定は POD なので写しは安い
        const AmbientOcclusionSettings ambientOcclusionSettings = frame.Settings.AmbientOcclusion;
        const EmissiveLightSettings emissiveLightSettings = frame.Settings.EmissiveLight;

        RHI::IRHIBuffer* const objectConstantBuffer = frame.ObjectConstantBuffer;
        RHI::IRHISamplerSet* const materialSamplers = frame.MaterialSamplers;

        // 【フレームの値をここで写し取る】以下はRender()から機械的に移した登録コードなので、
        // 参照している名前を変えずに済むよう同じ名前で受け直す。
        //
        // constants はRender()のローカルを指しており、参照捕捉している中間ラムダの
        // 束縛先はその実体である(graph.Execute()が終わるまで生きている)。
        // 一方、Registerのローカルであるラムダ(captureProbeFace 等)は**値で捕捉する**こと
        const FrameConstants& constants = *frame.Constants;
        RHI::IRHITexture* const skyTexture = frame.SkyTexture;
        const DirectX::XMMATRIX probeFaceProjection = frame.ProbeFaceProjection;
        const size_t bakedLightCount = frame.BakedLightCount;
        // プローブのキャプチャが読むテクスチャ一式。**DDGIのプローブ捕捉も同じ組を読む**ため
        // Render()側で1度だけ組み立て、フレームのスナップショット越しに配っている
        const std::vector<RHI::IRHITexture*>& probeCaptureReads = *frame.ProbeCaptureReads;

        // --- 反射プローブの更新(19章・19.10節) ---
        // 更新モードに応じて「フルベイク(全プローブの全面を1フレームで焼く)」か
        // 「時間分割(1フレームに1面だけ焼く)」のどちらかを実行する。両者はスクラッチの
        // キューブマップ(m_ProbeRadianceCube)を共有するため、同じフレームで両方を走らせてはならない

        // プローブ1面ぶんのキャプチャ(フォワード描画 → スクラッチのキューブ面へコピー)。
        // フルベイクと時間分割の両方から呼ぶためラムダへ切り出してある
        const auto captureProbeFace =
            [this, targets, lightBuffer, modelInstanceBuffer, brdfLUTTexture, iblPrefilterConstantBuffer, irradianceTexture, prefilteredEnvTexture, meshletLOD, ambientOcclusionSettings, emissiveLightSettings, &constants, probeFaceProjection, skyTexture, bakedLightCount, materialSamplers, objectConstantBuffer](RHI::IRHICommandList* cmd, size_t probeIndex, uint32_t face)
        {
            const Assets::ReflectionProbe& probe = m_Engine.m_ReflectionProbes[probeIndex];
            const DirectX::XMFLOAT3 probePosition{ probe.Position[0], probe.Position[1], probe.Position[2] };

            RHI::Viewport probeViewport;
            probeViewport.Width = static_cast<float>(kProbeCaptureSize);
            probeViewport.Height = static_cast<float>(kProbeCaptureSize);
            // 2枚目は距離(19.12節)。ProbeCapture.hlslのPSOutputと並びを一致させること
            RHI::IRHITexture* const captureTargets[] = { m_Engine.m_ProbeCaptureColor.get(), m_Engine.m_ProbeCaptureDistance.get() };

            // 太陽・カスケード・ライト数・IBL設定は共有のFrameConstantsをそのまま使い、
            // 視点に関わる2つだけをプローブのものへ差し替える(ProbeCapture.hlsl冒頭参照)。
            // Viewはカメラのまま残す(カスケード選択の深度がカメラ視錐台基準のため)
            FrameConstants captureConstants = constants;
            const DirectX::XMMATRIX faceViewProj = ComputeCubeFaceView(probePosition, face) * probeFaceProjection;
            DirectX::XMStoreFloat4x4(&captureConstants.ViewProj, DirectX::XMMatrixTranspose(faceViewProj));
            captureConstants.CameraPosition = { probePosition.x, probePosition.y, probePosition.z, 0.0f };
            // TAA関連のフィールドはカメラ視点のものが入ったままなので、プローブ視点として意味を成すよう
            // 明示的に潰しておく(前フレーム=今フレーム、ジッター無し=速度0)。ProbeCapture.hlslは
            // 現状これらを読まないが、将来読んだときに黙ってカメラの値を拾うのを防ぐため
            captureConstants.PrevViewProj = captureConstants.ViewProj;
            captureConstants.TAAParams = { 0.0f, 0.0f, 0.0f, 0.0f };
            // Hi-Zオクルージョンカリングも潰す。Hi-Zはメインカメラ視点の深度で、
            // プローブ視点から見える範囲とは何の関係も無い。上でPrevViewProjを
            // 「前フレーム」でない値へ差し替えている以上、判定の前提そのものが崩れている
            captureConstants.OcclusionCullParams = { 0.0f, 0.0f, 0.0f, 0.0f };
            // ドローンの灯はライトリストの末尾に連結してあるので、数を戻すだけで外れる。
            // 編隊は毎フレーム動く一方、反射プローブはOnDemandで一度焼いたきりなので、
            // 入れると「焼いた瞬間の編隊」が環境キューブに固定で残り、以後ずっと映り込む
            captureConstants.ActiveLightCount.x = static_cast<float>(bakedLightCount);
            // 統計も止める。プローブ視点で数えた分がメインカメラの間引き率に混ざると、
            // 「1フレームあたりの判定数」がプローブを焼いたフレームだけ跳ね上がって読めなくなる
            captureConstants.MeshletCullStatsParams = { 0.0f, 0.0f, 0.0f, 0.0f };
            cmd->UpdateBuffer(m_Engine.m_ProbeCaptureConstantBuffer.get(), &captureConstants, sizeof(captureConstants));

            cmd->SetRenderTargets(captureTargets, 2, m_Engine.m_ProbeCaptureDepth.get());
            cmd->SetViewport(probeViewport);
            // 両方のレンダーターゲットが0でクリアされる。距離側の0は「ジオメトリ無し」を意味しないが、
            // コピー側は深度が書かれたかどうかで判定するためこれで問題ない
            cmd->ClearRenderTarget({ 0.0f, 0.0f, 0.0f, 0.0f });
            // Reverse-Zのため遠平面側(NDC z=0.0)にクリアする。コピー側はこの0を
            // 「何も描かれなかった=スカイ」の判定に使う
            cmd->ClearDepth(0.0f);

            cmd->SetPipelineState(m_Engine.m_ProbeCapturePipelineState.get());
            cmd->SetConstantBuffer(0, m_Engine.m_ProbeCaptureConstantBuffer.get());
            cmd->SetSamplerSet(materialSamplers);

            // メッシュによらず共通のバインドはループの外で1回だけ行う。テクスチャのバインドは
            // 上書きするまで維持される(IRHICommandList::SetTexture参照)。DX12もバインド状態の
            // シャドウコピーを持ち寿命がDX11と揃っているため、ここで先にバインドしたものが
            // ループ内の各Drawへ引き継がれる
            cmd->SetTexture(4, targets->ShadowCascadeArray.get());
            cmd->SetShaderResourceBuffer(8, lightBuffer);
            cmd->SetTexture(9, irradianceTexture);
            cmd->SetTexture(10, prefilteredEnvTexture);
            cmd->SetTexture(11, brdfLUTTexture);
            // DDGI(22章)の多重バウンス。ProbeCapture.hlslは拡散の環境光をここから引く。
            // 参照するのは「前フレームまでに焼けているアトラス」で、同じフレームの中でも
            // 既に更新済みのプローブぶんは新しい値になる。DDGIは元々ヒステリシスで
            // 時間収束させる手法なので、この程度の混在は問題にならない
            cmd->SetTexture(12, m_Engine.m_DDGIIrradianceAtlas.get());
            cmd->SetTexture(13, m_Engine.m_DDGIDistanceAtlas.get());

            // このキューブ面の錐台で間引く。6面それぞれで判定するので、どこかの面には入る
            // モデルが全部消えることはない
            const FrustumPlanes faceFrustum = ExtractFrustumPlanes(faceViewProj);

            // インスタンシングのバッチと、まとめられなかった1体を同じ形で回す。
            // 【プローブも最も粗い段】焼き込むのは間接光で、細部は残らない。
            // ストリーミング中で未読み込みなら描かない
            Rendering::GeometryDrawLoopDesc probeLoop;
            probeLoop.Frustum = &faceFrustum;
            probeLoop.LODMode = Rendering::GeometryLODMode::Coarsest;
            // 半透明メッシュはプローブへ焼かない。ProbeCapture.hlslは不透明として描くため、
            // ガラスを焼き込むと「向こう側が見えるはずの面」が不透明の壁としてキューブに
            // 残り、その裏にある本来映るべき景色が欠ける。半透明を正しく焼くには
            // キャプチャ側にも奥から手前への描画順とブレンドが要り、コストに見合わない
            // (プローブへ半透明を含めないのは一般的な割り切り)
            probeLoop.MeshFilter = Rendering::GeometryMeshFilter::Opaque;

            m_Engine.ForEachGeometryDraw(
                probeLoop,
                // このパスは1ドロー経路(メッシュレット)を持たない。常にメッシュのループへ入る
                [](const Rendering::InstanceDrawUnit&, const Assets::Model&, float) { return false; },
                [&](const Rendering::InstanceDrawUnit& unit, const Assets::Model& coarsestModel,
                    const Assets::Mesh& mesh, float)
                {
                    const Assets::ModelInstance& instance = *unit.Instance;

                    ObjectConstants objectConstants = MakeObjectConstants(instance, coarsestModel, mesh, emissiveLightSettings.Intensity, ambientOcclusionSettings.OcclusionMapEnabled, meshletLOD);
                    objectConstants.InstanceBase = unit.InstanceBase;
                    objectConstants.InstancingEnabled = unit.IsBatch() ? 1u : 0u;
                    cmd->UpdateBuffer(objectConstantBuffer, &objectConstants, sizeof(objectConstants));
                    cmd->SetConstantBuffer(1, objectConstantBuffer);

                    // 【毎回張り直す】頂点シェーダー用SRVはt0の1本しかない
                    if (unit.IsBatch())
                    {
                        cmd->SetVertexShaderResourceBuffer(0, modelInstanceBuffer);
                    }

                    cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                    cmd->SetIndexBuffer(mesh.IndexBuffer.get());

                    // メッシュごとに変わるマテリアルのテクスチャだけをここでバインドする
                    cmd->SetTexture(0, mesh.BaseColorTexture);
                    cmd->SetTexture(1, mesh.NormalTexture);
                    cmd->SetTexture(2, mesh.MetallicRoughnessTexture);
                    cmd->SetTexture(3, mesh.EmissiveTexture);
                    // t4はカスケードシャドウマップ配列が占めているため遮蔽マップはt5、
                    // bent normalはその次のt6(GBuffer.hlsl/ProbeCapture.hlslで共通)
                    cmd->SetTexture(5, mesh.OcclusionTexture);
                    cmd->SetTexture(6, mesh.BentNormalTexture);

                    cmd->DrawIndexed(mesh.IndexCount, 0, 0, unit.InstanceCount);
                    return true;
                });

            // 描き終えたカラー/深度をコンピュートシェーダーからSRVとして読むため、
            // 先にレンダーターゲットのバインドを外す(D3D11は同一リソースの
            // RTV/DSVとSRVの同時バインドを許さず、SRV側がnullに落とされる)
            cmd->SetRenderTargets(nullptr, 0, nullptr);

            Passes::IBLFaceConstants faceConstants{};
            faceConstants.Face = face;
            cmd->SetComputePipelineState(m_Engine.m_ProbeCubeCopyPipelineState.get());
            cmd->UpdateBuffer(iblPrefilterConstantBuffer, &faceConstants, sizeof(faceConstants));
            cmd->SetComputeConstantBuffer(0, iblPrefilterConstantBuffer);
            cmd->SetComputeSamplerSet(materialSamplers);
            // ジオメトリが描かれなかったテクセルを埋める空。手続き空が有効なフレームでは
            // そちらを使わないと、プローブにだけ古いDDSの空が焼き込まれて本編と食い違う
            // (このフレームで使う空はRender冒頭のskyTextureに確定させてある)
            cmd->SetComputeTexture(0, skyTexture);
            cmd->SetComputeTexture(1, m_Engine.m_ProbeCaptureColor.get());
            cmd->SetComputeTexture(2, m_Engine.m_ProbeCaptureDepth.get());
            cmd->SetComputeTexture(3, m_Engine.m_ProbeCaptureDistance.get());
            cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_Engine.m_ProbeRadianceCube.get(), face, 0, 0);
            // 距離は畳み込まないため、スクラッチのキューブを経由せずプローブのスライスへ直接書く
            cmd->SetComputeUnorderedAccessTextureCubeFace(
                1, m_Engine.m_ProbeDistanceArray.get(), face, 0, static_cast<uint32_t>(probeIndex));
            cmd->Dispatch((kProbeCaptureSize + 7) / 8, (kProbeCaptureSize + 7) / 8, 1);
        };

        // 組み上がったスクラッチのキューブマップを、IBLとまったく同じ手順で畳み込んで
        // プローブのスライスへ書き込む。入力が違うだけでシェーダーはIBLBakeパスと共通
        // プローブのプリフィルタ済み鏡面の畳み込み。
        // 反射プローブは鏡面専任なので拡散イラディアンス側の畳み込みは持たない
        // (DDGIが拡散を担う。ReflectionProbe.hlsli冒頭のコメント参照)。
        // (mip, face)1組ぶんだけディスパッチする。SetComputePipelineState/SetComputeTexture/
        // SetComputeSamplerSetは呼び出し側が先に1回済ませておくこと(同じプローブの複数ステップを
        // 1パスにまとめて呼ぶ場合、毎回張り直す必要が無いため。Realtimeの時間分割参照)
        const auto convolveProbePrefilterStep =
            [this, iblPrefilterConstantBuffer](RHI::IRHICommandList* cmd, size_t probeIndex, uint32_t mip, uint32_t face)
        {
            const uint32_t cubeIndex = static_cast<uint32_t>(probeIndex);
            const uint32_t mipSize = std::max(1u, kIBLPrefilterBaseSize >> mip);
            const float roughness = static_cast<float>(mip) / static_cast<float>(kIBLPrefilterMipLevels - 1);

            Passes::IBLFaceConstants faceConstants{};
            faceConstants.Face = face;
            faceConstants.Roughness = roughness;
            cmd->UpdateBuffer(iblPrefilterConstantBuffer, &faceConstants, sizeof(faceConstants));
            cmd->SetComputeConstantBuffer(0, iblPrefilterConstantBuffer);
            cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_Engine.m_ProbePrefilteredArray.get(), face, mip, cubeIndex);
            cmd->Dispatch((mipSize + 7) / 8, (mipSize + 7) / 8, 1);
        };

        // 6ミップ×6面ぶん全部を1回で焼く(フルベイク用。Realtimeの時間分割はconvolveProbePrefilterStepを
        // 直接、複数フレームに分けて呼ぶ。下のRealtimeブロック参照)
        const auto convolveProbePrefilter = [this, convolveProbePrefilterStep, materialSamplers](RHI::IRHICommandList* cmd, size_t probeIndex)
        {
            cmd->SetComputePipelineState(m_Engine.m_PrefilterPipelineState.get());
            cmd->SetComputeTexture(0, m_Engine.m_ProbeRadianceCube.get());
            cmd->SetComputeSamplerSet(materialSamplers);
            for (uint32_t mip = 0; mip < kIBLPrefilterMipLevels; ++mip)
            {
                for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                {
                    convolveProbePrefilterStep(cmd, probeIndex, mip, face);
                }
            }
        };

        // キャプチャパスがReadsにシャドウマップとグローバルの畳み込み結果を挙げることで、
        // レンダーグラフがこれらをシャドウパス・IBLBakeパスより後ろへ順序付ける。
        // 空はm_SkyboxTextureではなくこのフレームで実際に使うskyTextureを挙げる。手続き空のときは
        // SkyGenerateパスがそれのWriterなので、これによりベイクが空の焼き直しより後ろへ順序付けられる
        const size_t probeCount = m_Engine.m_ReflectionProbes.size();

        // OnDemandは、焼き上がりに影響する状態(時刻・太陽・ライト)が変わったフレームだけ焼き直す。
        // 一度も焼けていない間はシーン読み込み時の要求が既に立っているのでここでは何もしない
        if (frame.Settings.ReflectionProbe.UpdateMode == ProbeUpdateMode::OnDemand && probeCount > 0 && m_Engine.m_ProbeBaked &&
            m_Engine.ComputeProbeBakeSignature() != m_Engine.m_ProbeBakeSignature)
        {
            m_Engine.m_ProbeBakeRequested = true;
        }

        if (m_Engine.m_ProbeBakeRequested && probeCount > 0)
        {
            // --- フルベイク: 全プローブの6面を1フレームで焼く ---
            // プローブごとに、さらにキャプチャ/プリフィルタ畳み込みで別パスへ分けることで、
            // GPUプロファイラでそれぞれのコストを個別に読める(19.10節の実測)。
            // 各パスがm_ProbeRadianceCubeを読み書きするため、レンダーグラフのWrite-after-Write /
            // Read-after-Write依存で登録順に直列化される(スクラッチを共有しても取り違えは起きない)
            for (size_t probeIndex = 0; probeIndex < probeCount; ++probeIndex)
            {
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "ProbeBakeCapture" + std::to_string(probeIndex),
                    .Reads = probeCaptureReads,
                    .Writes = {
                        m_Engine.m_ProbeCaptureColor.get(), m_Engine.m_ProbeCaptureDistance.get(), m_Engine.m_ProbeCaptureDepth.get(),
                        m_Engine.m_ProbeRadianceCube.get(), m_Engine.m_ProbeDistanceArray.get(),
                    },
                    .Execute = [captureProbeFace, probeIndex](RHI::IRHICommandList* cmd)
                    {
                        for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                        {
                            captureProbeFace(cmd, probeIndex, face);
                        }
                    },
                });
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "ProbeBakeConvolvePrefilter" + std::to_string(probeIndex),
                    .Reads = { m_Engine.m_ProbeRadianceCube.get() },
                    .Writes = { m_Engine.m_ProbePrefilteredArray.get() },
                    .Execute = [convolveProbePrefilter, probeIndex](RHI::IRHICommandList* cmd)
                    {
                        convolveProbePrefilter(cmd, probeIndex);
                    },
                });
            }

            m_Engine.m_ProbeBakeRequested = false;
            // このフレームの描画時点ではまだ焼き上がっていない(同じコマンドリスト内でこの後の
            // Lightingパスが読むのは問題ないが、gpuProbesは既に確定済み)。次フレームから
            // プローブが有効になるよう、ここでフラグだけ立てる
            m_Engine.m_ProbeBaked = true;
            m_Engine.m_ProbeBakeSignature = m_Engine.ComputeProbeBakeSignature();
            // このフレームの実効プリ露出で焼かれるので、読み出し側の換算倍率もここで更新する
            m_Engine.m_ProbeBakedExposureEV100 = effectiveExposureEV100;
            // 全プローブが今焼けたので、時間分割は先頭から仕切り直す
            m_Engine.m_ProbeRealtimeProbeIndex = 0;
            m_Engine.m_ProbeRealtimeFace = 0;
            m_Engine.m_ProbeRealtimePrefilterStep = kProbePrefilterStepCount;
        }
        else if (frame.Settings.ReflectionProbe.UpdateMode == ProbeUpdateMode::Realtime && probeCount > 0 && m_Engine.m_ProbeBaked)
        {
            // --- 時間分割: キャプチャフェーズ(1フレーム1面、6フレーム)→ プリフィルタフェーズ
            //     (1フレームkProbeRealtimePrefilterStepsPerFrame個の(mip,face)、6フレーム)を
            //     交互に繰り返す。
            //
            // **プリフィルタを6面揃った瞬間に36ディスパッチまとめて発行してはいけない** ――
            // これが「6フレームに1回のスパイク」になる。1フレームあたり数ステップへ分割することで、
            // どのフレームもほぼ均等なコストになる。
            //
            // プリフィルタフェーズの間はキャプチャを止める(m_ProbeRadianceCubeがそのプローブの
            // ぶんのまま変わらないことを保証するため)。そのプローブのスライスは、旧キャプチャ→
            // 旧キューブ→新スライスの畳み込みが終わるまで前回の内容のまま表示され続ける
            // (描きかけの中間状態が映り込むことはない)
            if (m_Engine.m_ProbeRealtimeProbeIndex >= probeCount)
            {
                m_Engine.m_ProbeRealtimeProbeIndex = 0;
                m_Engine.m_ProbeRealtimeFace = 0;
                m_Engine.m_ProbeRealtimePrefilterStep = kProbePrefilterStepCount;
            }

            if (m_Engine.m_ProbeRealtimePrefilterStep < kProbePrefilterStepCount)
            {
                // --- プリフィルタフェーズ ---
                const size_t realtimeProbe = m_Engine.m_ProbeRealtimeProbeIndex;
                const uint32_t startStep = m_Engine.m_ProbeRealtimePrefilterStep;
                const uint32_t stepsThisFrame =
                    std::min(kProbeRealtimePrefilterStepsPerFrame, kProbePrefilterStepCount - startStep);

                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "ProbeRealtimeConvolvePrefilterStep",
                    .Reads = { m_Engine.m_ProbeRadianceCube.get() },
                    .Writes = { m_Engine.m_ProbePrefilteredArray.get() },
                .Execute = [this, convolveProbePrefilterStep, realtimeProbe, startStep, stepsThisFrame, materialSamplers](
                        RHI::IRHICommandList* cmd)
                    {
                        cmd->SetComputePipelineState(m_Engine.m_PrefilterPipelineState.get());
                        cmd->SetComputeTexture(0, m_Engine.m_ProbeRadianceCube.get());
                        cmd->SetComputeSamplerSet(materialSamplers);
                        for (uint32_t s = 0; s < stepsThisFrame; ++s)
                        {
                            const uint32_t step = startStep + s;
                            // 【ステップ番号→(面, ミップ)の割り当て】面を外側・ミップを内側にする。
                            // ミップの解像度は段ごとに1/4になるので、テクセル数は
                            //   ミップ0: 128² / 1:64² / 2:32² / 3:16² / 4:8² / 5:4²
                            // で、1面ぶん21,840テクセルのうちミップ0だけで16,384(75%)を占める。
                            //
                            // これを mip=step/6, face=step%6 と割り当てると「1フレーム目が
                            // ミップ0の6面をまとめて引き受ける」ことになり、畳み込み全体の75%が
                            // 1フレームへ集中する。個数は6ステップずつ均等でもコストは均等にならない
                            // (実測: この割り当てでは9.4msのスパイクが残っていた)。
                            //
                            // 面を外側にすると1フレーム = 1面ぶんの全ミップ = 21,840テクセルとなり、
                            // 6フレームすべてが厳密に同じ量になる。ピークは16,384+残り → 21,840、
                            // つまりミップ0の6面ぶんに対して約1/4.5になる。
                            // なお1フレームの下限は「ミップ0の1面」であり、これ以上細かくするには
                            // 1つの面をさらに矩形へ分割する必要がある(そこまではやっていない)
                            const uint32_t face = step / kIBLPrefilterMipLevels;
                            const uint32_t mip = step % kIBLPrefilterMipLevels;
                            convolveProbePrefilterStep(cmd, realtimeProbe, mip, face);
                        }
                    },
                });

                m_Engine.m_ProbeRealtimePrefilterStep = startStep + stepsThisFrame;
                if (m_Engine.m_ProbeRealtimePrefilterStep >= kProbePrefilterStepCount)
                {
                    // このプローブの畳み込みが完了。次のプローブのキャプチャへ進む
                    m_Engine.m_ProbeRealtimePrefilterStep = kProbePrefilterStepCount;
                    m_Engine.m_ProbeRealtimeProbeIndex = static_cast<uint32_t>((realtimeProbe + 1) % probeCount);
                    m_Engine.m_ProbeRealtimeFace = 0;
                }
            }
            else
            {
                // --- キャプチャフェーズ ---
                const size_t realtimeProbe = m_Engine.m_ProbeRealtimeProbeIndex;
                const uint32_t realtimeFace = m_Engine.m_ProbeRealtimeFace;

                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "ProbeRealtimeCapture",
                    .Reads = probeCaptureReads,
                    .Writes = {
                        m_Engine.m_ProbeCaptureColor.get(), m_Engine.m_ProbeCaptureDistance.get(), m_Engine.m_ProbeCaptureDepth.get(),
                        m_Engine.m_ProbeRadianceCube.get(), m_Engine.m_ProbeDistanceArray.get(),
                    },
                    .Execute = [captureProbeFace, realtimeProbe, realtimeFace](RHI::IRHICommandList* cmd)
                    {
                        captureProbeFace(cmd, realtimeProbe, realtimeFace);
                    },
                });

                m_Engine.m_ProbeRealtimeFace = realtimeFace + 1;
                if (m_Engine.m_ProbeRealtimeFace >= kCubeFaceCount)
                {
                    // 6面揃った。次フレームからこのプローブのプリフィルタフェーズへ入る
                    // (プローブ番号はプリフィルタが完了するまで進めない。上のプリフィルタフェーズ参照)
                    m_Engine.m_ProbeRealtimeFace = 0;
                    m_Engine.m_ProbeRealtimePrefilterStep = 0;
                }
            }

            // 常に焼き直しているのでOnDemandの署名も追随させておく。こうしておかないと
            // Realtimeから切り替えた直後に不要なフルベイクが1回走る
            m_Engine.m_ProbeBakeSignature = m_Engine.ComputeProbeBakeSignature();
            // 露出の換算倍率も追随させる。1ステップずつ焼くため厳密には面・ミップごとに焼いた
            // 露出が違うが、実効プリ露出の変化は毎秒2倍程度(m_PostProcessSettings.EffectiveExposureAdaptSpeed)なので
            // 1周(最大12フレーム)ぶんのずれは数%にとどまり、常時焼き直している以上すぐ解消する
            m_Engine.m_ProbeBakedExposureEV100 = effectiveExposureEV100;
        }
    }
}
