#include "../KurenaiEngine3D.h"

#include <algorithm>
#include <vector>

#include "Core/Logger.h"
#include "Core/RenderGraph.h"
#include "ReflectionPasses.h"
#include "../Rendering/GeometryDrawLoop.h"
#include "../Rendering/ObjectConstants.h"
#include "../Rendering/RenderBlackboard.h"
#include "../Rendering/RenderFrameContext.h"
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

    void ReflectionPasses::Register(
        Core::RenderGraph& graph,
        const Rendering::RenderFrameContext& frame,
        const Rendering::RenderBlackboard& bb)
    {
        const uint32_t renderWidth = frame.RenderWidth;
        const uint32_t renderHeight = frame.RenderHeight;
        RHI::IRHIBuffer* const frameConstantBuffer = frame.FrameConstantBuffer;
        RHI::IRHIBuffer* const objectConstantBuffer = frame.ObjectConstantBuffer;
        RHI::IRHISamplerSet* const materialSamplers = frame.MaterialSamplers;
        RHI::IRHISamplerSet* const screenSpaceSamplers = frame.ScreenSpaceSamplers;

        // 【フレームの値をここで写し取る】以下はRender()から機械的に移した登録コードなので、
        // 参照している名前を変えずに済むよう同じ名前で受け直す。
        //
        // constants はRender()のローカルを指す(graph.Execute()まで生きている)。
        // ビューポートはこの関数のローカルになるため、ラムダへは**値で捕捉する**こと
        const FrameConstants& constants = *frame.Constants;
        const RHI::Viewport gbufferViewport = frame.GBufferViewport;
        const DirectX::XMMATRIX viewMatrix = frame.ViewMatrix;
        const DirectX::XMMATRIX jitteredProj = frame.JitteredProj;
        const DirectX::XMMATRIX reflectMatrix = frame.ReflectMatrix;
        const DirectX::XMMATRIX reflectedViewProj = frame.ReflectedViewProj;
        const float effectiveExposure = frame.EffectiveExposure;
        const float waterPlaneY = frame.WaterPlaneY;
        const bool usingProceduralSky = frame.UsingProceduralSky;
        const bool planarReflectionPassRuns = frame.PlanarReflectionPassRuns;
        RHI::IRHITexture* const activeAOTexture = bb.ActiveAOTexture;

        // --- 平面反射パス: 水面に不透明ジオメトリの鏡像を映すフォワードパス ---
        // 水面が無いシーン・無効化時はパスを登録しない(SSR側のフラグも0になる。下のSSRパス参照)
        if (planarReflectionPassRuns)
        {
            RHI::Viewport planarReflectionViewport;
            planarReflectionViewport.Width = static_cast<float>(m_Engine.m_PlanarReflectionWidth);
            planarReflectionViewport.Height = static_cast<float>(m_Engine.m_PlanarReflectionHeight);

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "PlanarReflection",
                // ProbeCapture/captureProbeFaceと同じ理由でシャドウ・IBL・DDGIを挙げ、
                // これらを書くパスより後ろへ順序付ける(実際のバインドはExecute内)
                .Reads = {
                    m_Engine.m_ShadowCascadeArray.get(), m_Engine.m_IrradianceTexture.get(), m_Engine.m_PrefilteredEnvTexture.get(),
                    m_Engine.m_BRDFLUTTexture.get(), m_Engine.m_DDGIIrradianceAtlas.get(), m_Engine.m_DDGIDistanceAtlas.get(),
                    m_Engine.m_SkyViewLUT.get(),
                },
                .RenderTargets = { m_Engine.m_PlanarReflectionColor.get() },
                .DepthTarget = m_Engine.m_PlanarReflectionDepth.get(),
                // 大気遠近。空パラメータ(m_SkyParametersBuffer)をSkyIntegrateパスの後へ
                // 順序付けさせるために挙げる(実際のバインドはExecute内。SSRパスの同じ宣言と同じ理由)
                // m_DroneBufferはこのパス末尾でドローンショーの機体を描き足すために読む
                // (実際のバインドはExecute内)
                .BufferReads = { m_Engine.m_LightBuffer.get(), m_Engine.m_SkyParametersBuffer.get(), m_Engine.m_DroneBuffer.get() },
                .Execute = [this, &constants, planarReflectionViewport, reflectedViewProj, reflectMatrix, waterPlaneY, viewMatrix, jitteredProj, effectiveExposure, objectConstantBuffer, materialSamplers](RHI::IRHICommandList* cmd)
                {
                    // captureProbeFaceとまったく同じ作法(constants.ViewProj/CameraPosition/
                    // PrevViewProj/TAAParams/PlanarReflectionPlaneだけをこのパス用に差し替える)。
                    // Viewはカメラのビュー行列のままにする(PlanarReflection.hlsl冒頭の
                    // 【Viewをカメラのままにする理由】参照。ProbeCaptureとは異なる理由による)
                    FrameConstants reflectionConstants = constants;
                    DirectX::XMStoreFloat4x4(&reflectionConstants.ViewProj, DirectX::XMMatrixTranspose(reflectedViewProj));
                    const DirectX::XMVECTOR reflectedCameraPos =
                        DirectX::XMVector3Transform(DirectX::XMLoadFloat4(&constants.CameraPosition), reflectMatrix);
                    DirectX::XMFLOAT4 reflectedCameraPosFloat;
                    DirectX::XMStoreFloat4(&reflectedCameraPosFloat, reflectedCameraPos);
                    reflectionConstants.CameraPosition = { reflectedCameraPosFloat.x, reflectedCameraPosFloat.y, reflectedCameraPosFloat.z, 0.0f };
                    // TAA関連はカメラ視点のものが入ったままなので明示的に潰す(captureProbeFaceと同じ理由)
                    reflectionConstants.PrevViewProj = reflectionConstants.ViewProj;
                    reflectionConstants.TAAParams = { 0.0f, 0.0f, 0.0f, 0.0f };
                    // Hi-Zオクルージョンカリングも潰す(captureProbeFaceと同じ理由)。
                    // 鏡映カメラから見える範囲とメインカメラのHi-Zは無関係
                    reflectionConstants.OcclusionCullParams = { 0.0f, 0.0f, 0.0f, 0.0f };
                    // 統計も止める(captureProbeFaceと同じ理由)
                    reflectionConstants.MeshletCullStatsParams = { 0.0f, 0.0f, 0.0f, 0.0f };
                    reflectionConstants.PlanarReflectionPlane = { 0.0f, 1.0f, 0.0f, -waterPlaneY };
                    cmd->UpdateBuffer(m_Engine.m_PlanarReflectionConstantBuffer.get(), &reflectionConstants, sizeof(reflectionConstants));

                    // RenderTargets/DepthTargetはパス宣言(.RenderTargets/.DepthTarget)により
                    // RenderGraphが自動的にバインド済みのため、ここではビューポート設定と
                    // クリアだけでよい(GBuffer/Lightingパスと同じ流儀。captureProbeFaceは
                    // .Writesのみの宣言のため例外的に手動バインドしている)
                    cmd->SetViewport(planarReflectionViewport);
                    cmd->ClearRenderTarget({ 0.0f, 0.0f, 0.0f, 0.0f });
                    // Reverse-Zのため遠平面側(NDC z=0.0)にクリアする
                    cmd->ClearDepth(0.0f);

                    cmd->SetPipelineState(m_Engine.m_PlanarReflectionPipelineState.get());
                    cmd->SetConstantBuffer(0, m_Engine.m_PlanarReflectionConstantBuffer.get());
                    cmd->SetSamplerSet(materialSamplers);

                    // captureProbeFaceと同じ順・同じレジスタでバインドする(PlanarReflection.hlsl参照)
                    cmd->SetTexture(4, m_Engine.m_ShadowCascadeArray.get());
                    cmd->SetShaderResourceBuffer(8, m_Engine.m_LightBuffer.get());
                    cmd->SetTexture(9, m_Engine.m_IrradianceTexture.get());
                    cmd->SetTexture(10, m_Engine.m_PrefilteredEnvTexture.get());
                    cmd->SetTexture(11, m_Engine.m_BRDFLUTTexture.get());
                    cmd->SetTexture(12, m_Engine.m_DDGIIrradianceAtlas.get());
                    cmd->SetTexture(13, m_Engine.m_DDGIDistanceAtlas.get());
                    // 大気遠近のin-scatter項が読む空パラメータ(PlanarReflection.hlsl参照)
                    cmd->SetShaderResourceBuffer(14, m_Engine.m_SkyParametersBuffer.get());
                    // 大気散乱のSkyView LUT。in-scatter項の空の色はここから引く
                    cmd->SetTexture(15, m_Engine.m_SkyViewLUT.get());

                    // 鏡映カメラで描くとワインディングが全反転するため、PSOの切り替えは
                    // instance.IsMirroredの否定で行う(このファイル冒頭のPSO生成箇所のコメント参照)
                    RHI::IRHIPipelineState* currentPipelineState = m_Engine.m_PlanarReflectionPipelineState.get();
                    const auto bindPipelineState = [&](bool mirrored)
                    {
                        RHI::IRHIPipelineState* const wanted =
                            mirrored ? m_Engine.m_PlanarReflectionPipelineStateMirrored.get() : m_Engine.m_PlanarReflectionPipelineState.get();
                        if (wanted == currentPipelineState)
                        {
                            return;
                        }
                        cmd->SetPipelineState(wanted);
                        cmd->SetConstantBuffer(0, m_Engine.m_PlanarReflectionConstantBuffer.get());
                        cmd->SetSamplerSet(materialSamplers);
                        currentPipelineState = wanted;
                    };

                    // 鏡映カメラの錐台で間引く。カメラ本体の錐台とは別物なので、
                    // 画面には映っていないが水面には映るものが正しく残る
                    const FrustumPlanes reflectionFrustum = ExtractFrustumPlanes(reflectedViewProj);

                    // インスタンシングのバッチと、まとめられなかった1体を同じ形で回す。
                    // 深度プリパス/G-Bufferと同じ「そのフレームに選ばれた段」を描くが、
                    // 【このパスはクロスディザ非対応】なのでフェード中でも段は1つに決め打つ
                    // (GeometryLODMode::Current)。ストリーミング中で未読み込みなら描かない
                    KurenaiEngine3D::GeometryDrawLoopDesc planarLoop;
                    planarLoop.Frustum = &reflectionFrustum;
                    planarLoop.LODMode = KurenaiEngine3D::GeometryLODMode::Current;
                    // 半透明メッシュは反射に含めない(ProbeCaptureと同じ割り切り。
                    // PlanarReflection.hlsl冒頭参照)
                    planarLoop.MeshFilter = KurenaiEngine3D::GeometryMeshFilter::Opaque;

                    m_Engine.ForEachGeometryDraw(
                        planarLoop,
                        // このパスは1ドロー経路(メッシュレット)を持たない
                        [](const KurenaiEngine3D::InstanceDrawUnit&, const Assets::Model&, float) { return false; },
                        [&](const KurenaiEngine3D::InstanceDrawUnit& unit, const Assets::Model& currentModel,
                            const Assets::Mesh& mesh, float)
                        {
                            const Assets::ModelInstance& instance = *unit.Instance;

                            // 鏡映で巻きが反転するため、ミラーリングの有無に対して逆のPSOを選ぶ。
                            // バッチ内では IsMirrored が同一(グループ化のキー)なので代表で決めてよい
                            bindPipelineState(!instance.IsMirrored);

                            ObjectConstants objectConstants =
                                MakeObjectConstants(instance, currentModel, mesh, m_Engine.m_EmissiveLightSettings.Intensity, m_Engine.m_AmbientOcclusionSettings.OcclusionMapEnabled, m_Engine.m_MeshletLODFrame);
                            objectConstants.InstanceBase = unit.InstanceBase;
                            objectConstants.InstancingEnabled = unit.IsBatch() ? 1u : 0u;
                            cmd->UpdateBuffer(objectConstantBuffer, &objectConstants, sizeof(objectConstants));
                            cmd->SetConstantBuffer(1, objectConstantBuffer);

                            // 【毎回張り直す】このパスはモデルのあとにドローンショーを描き、
                            // そちらが同じ頂点シェーダー用SRV(t0)へ自分のバッファを張る。
                            // 張り直さないと全インスタンスがドローンの座標を行列として読む
                            if (unit.IsBatch())
                            {
                                cmd->SetVertexShaderResourceBuffer(0, m_Engine.m_ModelInstanceBuffer.get());
                            }

                            cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                            cmd->SetIndexBuffer(mesh.IndexBuffer.get());
                            cmd->SetTexture(0, mesh.BaseColorTexture);
                            cmd->SetTexture(1, mesh.NormalTexture);
                            cmd->SetTexture(2, mesh.MetallicRoughnessTexture);
                            cmd->SetTexture(3, mesh.EmissiveTexture);
                            cmd->SetTexture(5, mesh.OcclusionTexture);
                            cmd->DrawIndexed(mesh.IndexCount, 0, 0, unit.InstanceCount);
                            return true;
                        });

                    // --- 水面へ映すドローンショーの機体 ---
                    // 平面反射は「カメラを鏡映しただけで世界は動かしていない」ので、
                    // 機体もそのままのワールド座標で、鏡映済みのビュー行列で描き直せばよい。
                    // これを描かないと、空には編隊が出ているのに水面には何も映らない
                    // (SSRパスがm_PlanarReflectionColorを水面へ合成する)
                    if (m_Engine.m_DroneShowEnabled && !m_Engine.m_DroneInstances.empty())
                    {
                        DirectX::XMFLOAT4X4 projection;
                        DirectX::XMStoreFloat4x4(&projection, jitteredProj);

                        Passes::DroneShowConstants droneConstants{};
                        // 鏡映×カメラのビュー行列。reflectedViewProjの分解と同じ組み合わせで、
                        // Projはメインカメラのジッター済みProjをそのまま使う
                        DirectX::XMStoreFloat4x4(
                            &droneConstants.View, DirectX::XMMatrixTranspose(reflectMatrix * viewMatrix));
                        DirectX::XMStoreFloat4x4(&droneConstants.Proj, DirectX::XMMatrixTranspose(jitteredProj));
                        droneConstants.Params0 = {
                            m_Engine.m_DroneShow.Data().Brightness * effectiveExposure,
                            m_Engine.m_DroneShowMinScreenRadius,
                            projection._11,
                            0.0f,
                        };
                        // 水面より下にいる機体は反射に映してはいけない。ジオメトリ側の
                        // SV_ClipDistance0(FrameConstants.PlanarReflectionPlane)と同じ規約・同じ平面
                        droneConstants.ClipPlane = { 0.0f, 1.0f, 0.0f, -waterPlaneY };
                        droneConstants.Params1 = { 1.0f, 0.0f, 0.0f, 0.0f };
                        cmd->UpdateBuffer(m_Engine.m_DroneShowConstantBuffer.get(), &droneConstants, sizeof(droneConstants));

                        // メイン描画とまったく同じPSOでよい(ビルボードの四隅はビュー空間で
                        // 足しており鏡映行列を通らないため、巻きが反転しない。
                        // 詳しい理由はPSO生成箇所のコメント)
                        cmd->SetPipelineState(m_Engine.m_DroneShowPipelineState.get());
                        cmd->SetConstantBuffer(1, m_Engine.m_DroneShowConstantBuffer.get());
                        cmd->SetVertexShaderResourceBuffer(0, m_Engine.m_DroneBuffer.get());
                        cmd->Draw(static_cast<uint32_t>(m_Engine.m_DroneInstances.size()) * 6u, 0);
                    }
                },
            });
        }

        // --- 反射パス: Lightingパスが適用した鏡面IBLを、実際に追跡した反射で差し替える(20章)。
        //     ScreenSpaceならSSR(レイマーチ)、RaytracedならRT反射(RayQuery)。
        //     Offならスキップし、後段のTonemapが直接m_SceneColorを読む ---
        if (m_Engine.m_ReflectionSettings.Mode == ReflectionMode::ScreenSpace)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "SSR",
                // SSRはLightingパスが適用した鏡面IBLを「差し替える」ため、そのとき使ったものと
                // 同じ環境ソース(プローブ配列・グローバルのプリフィルタ済み鏡面)とBRDF LUT・AOを
                // 読む必要がある(20章)。
                // 手続き空はm_PrefilteredEnvTextureの焼き込み経由で入ってくるため、
                // 空のキューブマップをここで直接バインドする必要はない
                .Reads = {
                    m_Engine.m_SceneColor.get(), m_Engine.m_RenderTargets.GBufferNormal.get(), m_Engine.m_RenderTargets.GBufferMaterial.get(), m_Engine.m_RenderTargets.GBufferDepth.get(),
                    m_Engine.m_RenderTargets.GBufferAlbedo.get(), activeAOTexture, m_Engine.m_BRDFLUTTexture.get(), m_Engine.m_PrefilteredEnvTexture.get(),
                    m_Engine.m_ProbePrefilteredArray.get(), m_Engine.m_ProbeDistanceArray.get(),
                    // 平面反射。パスが登録されなかったフレームでもこのReadsは無害
                    // (今フレームのWriterが無いため単に依存辺が張られないだけ)
                    m_Engine.m_PlanarReflectionColor.get(),
                    // 大気散乱のSkyView LUT。水面に映る空をここから引く
                    m_Engine.m_SkyViewLUT.get(),
                    // bent normal(34章)。スペキュラ遮蔽をLightingパスと同じ規則で求めるために読む
                    m_Engine.m_RenderTargets.GBufferBentNormal.get(),
                },
                .RenderTargets = { m_Engine.m_SSRTexture.get() },
                // 空パラメータ。SkyIntegrateパスより後に順序付けさせるために挙げる
                // (実際のバインドはExecute内)
                .BufferReads = { m_Engine.m_SkyParametersBuffer.get() },
                .Execute = [this, gbufferViewport, activeAOTexture, usingProceduralSky, planarReflectionPassRuns, frameConstantBuffer, screenSpaceSamplers](RHI::IRHICommandList* cmd)
                {
                    // 水面の解析空フォールバック。手続き空が無効(.ksceneがDDSスカイボックスを
                    // 明示するシーン)なときは、m_WaterSettings.AnalyticSkyReflectionの値に関わらず必ず0にする
                    // ――DDSは任意の絵でPerezモデルとは無関係なため、SSR.hlsl側のSkyColorで
                    // 解析評価してはいけない(usingProceduralSkyはRender()前半で既に確定済み。
                    // DeferredLighting.hlsl向けのconstants.SkyParams.y代入と同じ判断)
                    const float waterAnalyticSkyFlag =
                        (m_Engine.m_WaterSettings.AnalyticSkyReflection && usingProceduralSky) ? 1.0f : 0.0f;
                    // 平面反射。このフレームでPlanarReflectionパスを実際に実行したときだけ
                    // 有効にする(登録されなかったフレームにm_PlanarReflectionColorの中身は
                    // 前フレーム/未定義の残骸なので、フラグをそのままSSR.hlsl側へ渡してはいけない)
                    const float planarReflectionFlag = planarReflectionPassRuns ? 1.0f : 0.0f;

                    SSRConstants ssrConstants{};
                    ssrConstants.Params0 =
                        { m_Engine.m_ReflectionSettings.SSRMaxDistance, m_Engine.m_ReflectionSettings.SSRThickness, m_Engine.m_ReflectionSettings.SSRRoughnessCutoff, waterAnalyticSkyFlag };
                    ssrConstants.Params1 = { planarReflectionFlag, m_Engine.m_ReflectionSettings.PlanarDistortion, 0.0f, 0.0f };
                    cmd->UpdateBuffer(m_Engine.m_SSRConstantBuffer.get(), &ssrConstants, sizeof(ssrConstants));

                    cmd->SetViewport(gbufferViewport);
                    cmd->SetPipelineState(m_Engine.m_SSRPipelineState.get());
                    cmd->SetConstantBuffer(0, frameConstantBuffer);
                    cmd->SetConstantBuffer(1, m_Engine.m_SSRConstantBuffer.get());
                    cmd->SetSamplerSet(screenSpaceSamplers);
                    cmd->SetTexture(0, m_Engine.m_SceneColor.get());
                    cmd->SetTexture(1, m_Engine.m_RenderTargets.GBufferNormal.get());
                    cmd->SetTexture(2, m_Engine.m_RenderTargets.GBufferMaterial.get());
                    cmd->SetTexture(3, m_Engine.m_RenderTargets.GBufferDepth.get());
                    cmd->SetTexture(4, m_Engine.m_RenderTargets.GBufferAlbedo.get());
                    cmd->SetTexture(5, activeAOTexture);
                    cmd->SetTexture(6, m_Engine.m_BRDFLUTTexture.get());
                    cmd->SetTexture(7, m_Engine.m_PrefilteredEnvTexture.get());
                    cmd->SetTexture(8, m_Engine.m_ProbePrefilteredArray.get());
                    cmd->SetShaderResourceBuffer(9, m_Engine.m_ProbeBuffer.get());
                    cmd->SetTexture(10, m_Engine.m_ProbeDistanceArray.get());
                    // 平面反射。DX12はディスクリプタテーブルに未初期化のスロットが残ると
                    // 動作が未定義になるため、パスが無効なフレームでも常にバインドする
                    // (反射プローブ・DDGIと同じ理由)
                    cmd->SetTexture(11, m_Engine.m_PlanarReflectionColor.get());
                    // 空パラメータ。SSR.hlsl側はt12(t0〜t11が既に使用済み)
                    cmd->SetShaderResourceBuffer(12, m_Engine.m_SkyParametersBuffer.get());
                    // ボリュメトリック積雲の3Dノイズ。水面に映る雲も背景とまったく同じ
                    // 立体にならなければ「空の雲と水面の雲が別物」になるため、ここにも同じものを渡す
                    cmd->SetTexture(13, m_Engine.m_CloudShapeNoiseTexture.get());
                    cmd->SetTexture(14, m_Engine.m_CloudDetailNoiseTexture.get());
                    // 大気散乱のSkyView LUT。雲と同じ理由で、水面に映る空も
                    // 背景とまったく同じものでなければならない
                    cmd->SetTexture(15, m_Engine.m_SkyViewLUT.get());
                    // 焼いたウェザーマップ(H3)。**Lightingパスと同じものを渡さないと、
                    // 水面に映る雲と空の雲が別の場所に立つ**
                    cmd->SetTexture(17, m_Engine.m_CloudWeatherNoiseTexture.get());
                    // bent normal(34章)。Lightingパスとまったく同じものを読まないと、
                    // SSRが適用される領域とされない領域の境界に段差が出る。
                    // **t11は平面反射が使っているためt16へ移した**
                    cmd->SetTexture(16, m_Engine.m_RenderTargets.GBufferBentNormal.get());
                    cmd->Draw(3, 0);
                },
            });
        }
        else if (m_Engine.ShouldRunRaytracedReflection())
        {
            // RT反射パス。読むものはSSRとほぼ同じ(同じ鏡面IBLを差し替えるため)で、
            // これに加えてTLASとシーンジオメトリの統合バッファを読む。
            // レジスタ割り当てはRTReflection.hlsl側の宣言と一致させること
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "RTReflection",
                .Reads = {
                    m_Engine.m_SceneColor.get(), m_Engine.m_RenderTargets.GBufferNormal.get(), m_Engine.m_RenderTargets.GBufferMaterial.get(), m_Engine.m_RenderTargets.GBufferDepth.get(),
                    m_Engine.m_RenderTargets.GBufferAlbedo.get(), activeAOTexture, m_Engine.m_BRDFLUTTexture.get(), m_Engine.m_PrefilteredEnvTexture.get(),
                    m_Engine.m_ProbePrefilteredArray.get(), m_Engine.m_RenderTargets.GBufferBentNormal.get(),
                },
                .Writes = { m_Engine.m_RTReflectionTexture.get() },
                .Execute = [this, activeAOTexture, renderWidth, renderHeight, frameConstantBuffer, materialSamplers](RHI::IRHICommandList* cmd)
                {
                    RTReflectionConstants rtConstants{};
                    rtConstants.Params0 = {
                        static_cast<float>(renderWidth), static_cast<float>(renderHeight),
                        m_Engine.m_ReflectionSettings.RTReflectionMaxDistance, m_Engine.m_ReflectionSettings.RTReflectionRoughnessCutoff
                    };
                    // yはメッシュレットのデバッグ表示。ラスタ側と同じトグルで駆動するので、
                    // 有効にすると「直接見えている面」と「反射に映る面」の両方が
                    // メッシュレット色になり、同じ塊が同じ色かを見比べられる
                    rtConstants.Params1 = {
                        m_Engine.m_ReflectionSettings.RTReflectionShadowRayEnabled ? 1.0f : 0.0f,
                        m_Engine.m_GeometrySettings.MeshletDebugViewEnabled ? 1.0f : 0.0f,
                        0.0f,
                        0.0f,
                    };
                    cmd->UpdateBuffer(m_Engine.m_RTReflectionConstantBuffer.get(), &rtConstants, sizeof(rtConstants));

                    cmd->SetComputePipelineState(m_Engine.m_RTReflectionPipelineState.get());
                    // 【スクリーン空間用ではなくマテリアル用のセット】ヒット面のマテリアル
                    // テクスチャをbindlessで引くようになったため、s0にWrapのサンプラーが要る。
                    // モデルのUVはタイリング前提で[0,1]の外へ出るので、Clampで引くと
                    // 端のテクセルが引き伸ばされて模様が崩れる。
                    // s1(色バッファ)・s2(データ)はどちらのセットでも中身が同じで、
                    // s0でこのシェーダーが他に引くのはキューブマップだけ(アドレスモードは
                    // 面をまたぐフィルタに使われないため無関係)なので、切り替えの影響はここだけ
                    cmd->SetComputeSamplerSet(materialSamplers);
                    cmd->SetComputeConstantBuffer(0, frameConstantBuffer);
                    cmd->SetComputeConstantBuffer(1, m_Engine.m_RTReflectionConstantBuffer.get());

                    cmd->SetComputeAccelerationStructure(0, m_Engine.m_RaytracingScene.GetTopLevelAS());
                    cmd->SetComputeTexture(1, m_Engine.m_SceneColor.get());
                    cmd->SetComputeTexture(2, m_Engine.m_RenderTargets.GBufferNormal.get());
                    cmd->SetComputeTexture(3, m_Engine.m_RenderTargets.GBufferMaterial.get());
                    cmd->SetComputeTexture(4, m_Engine.m_RenderTargets.GBufferDepth.get());
                    cmd->SetComputeTexture(5, m_Engine.m_RenderTargets.GBufferAlbedo.get());
                    cmd->SetComputeTexture(6, activeAOTexture);
                    cmd->SetComputeTexture(7, m_Engine.m_BRDFLUTTexture.get());
                    cmd->SetComputeTexture(8, m_Engine.m_PrefilteredEnvTexture.get());
                    cmd->SetComputeTexture(9, m_Engine.m_ProbePrefilteredArray.get());
                    cmd->SetComputeShaderResourceBuffer(10, m_Engine.m_ProbeBuffer.get());
                    cmd->SetComputeShaderResourceBuffer(11, m_Engine.m_RaytracingScene.GetVertexAttributeBuffer());
                    cmd->SetComputeShaderResourceBuffer(12, m_Engine.m_RaytracingScene.GetIndexBuffer());
                    cmd->SetComputeShaderResourceBuffer(13, m_Engine.m_RaytracingScene.GetMeshInfoBuffer());
                    cmd->SetComputeShaderResourceBuffer(14, m_Engine.m_RaytracingScene.GetInstanceInfoBuffer());
                    cmd->SetComputeShaderResourceBuffer(15, m_Engine.m_RaytracingScene.GetMaterialBuffer());
                    // メッシュレット表(t17)。RTReflection.hlslのKURENAI_RT_MESHLET_REGISTERと
                    // 一致させること。デバッグ表示でヒット面のメッシュレットを引くのに使う。
                    // 無いシーンでバインドしない理由はRTAO側と同じ。
                    // t8はプリフィルタ済み鏡面(上の16行目)が使っており空いていない
                    if (RHI::IRHIBuffer* meshletBuffer = m_Engine.m_RaytracingScene.GetMeshletTriangleOffsetBuffer())
                    {
                        cmd->SetComputeShaderResourceBuffer(17, meshletBuffer);
                    }
                    // bent normal(34章)。t0〜t15が埋まっているためt16。
                    // SSR.hlslと同じくスペキュラ遮蔽の方向依存を再現するために要る
                    cmd->SetComputeTexture(16, m_Engine.m_RenderTargets.GBufferBentNormal.get());

                    // UAVはDispatch直後に解除されるため毎回バインドし直す(IRHICommandList.h参照)
                    cmd->SetComputeUnorderedAccessTexture(0, m_Engine.m_RTReflectionTexture.get());
                    cmd->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);
                },
            });
        }
    }
}
