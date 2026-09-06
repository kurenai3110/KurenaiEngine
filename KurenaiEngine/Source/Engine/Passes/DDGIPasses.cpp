#include "../KurenaiEngine3D.h"

#include <algorithm>
#include <vector>

#include "Core/Logger.h"
#include "Core/RenderGraph.h"
#include "DDGIPasses.h"
#include "DDGIConstants.h"
#include "EnvironmentConstants.h"
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

    void DDGIPasses::RegisterProbeUpdate(
        Core::RenderGraph& graph,
        const Rendering::RenderFrameContext& frame,
        const Rendering::RenderBlackboard& bb)
    {
        // 【ラムダへ値で渡すためローカルへ受け直す】frame そのものは捕捉しない作法
        // (Rendering/RenderFrameContext.h の冒頭)。設定は POD なので写しは安い
        const AmbientOcclusionSettings ambientOcclusionSettings = frame.Settings.AmbientOcclusion;
        const DDGISettings ddgiSettings = frame.Settings.DDGI;
        const EmissiveLightSettings emissiveLightSettings = frame.Settings.EmissiveLight;

        RHI::IRHIBuffer* const frameConstantBuffer = frame.FrameConstantBuffer;
        RHI::IRHIBuffer* const objectConstantBuffer = frame.ObjectConstantBuffer;
        RHI::IRHISamplerSet* const materialSamplers = frame.MaterialSamplers;

        // 【フレームの値をここで写し取る】以下はRender()から機械的に移した登録コードなので、
        // 参照している名前を変えずに済むよう同じ名前で受け直す。
        //
        // constants はRender()のローカルを指しており、参照捕捉している中間ラムダの
        // 束縛先はその実体である(graph.Execute()が終わるまで生きている)。
        // 一方、Registerのローカルであるラムダ(captureDDGIProbeFace 等)は**値で捕捉する**こと
        const FrameConstants& constants = *frame.Constants;
        RHI::IRHITexture* const skyTexture = bb.SkyTexture;
        const std::vector<RHI::IRHITexture*>& probeCaptureReads = *frame.ProbeCaptureReads;
        const DirectX::XMMATRIX probeFaceProjection = frame.ProbeFaceProjection;
        const size_t bakedLightCount = frame.BakedLightCount;
        const float effectiveExposure = frame.EffectiveExposure;

        // --- DDGIのプローブ更新(22章) ---
        // 反射プローブとまったく同じキャプチャ経路を使い、解像度だけkDDGICaptureSizeへ落とす。
        // 6面×16×16 = 1536テクセルがそのままDDGIの「1536本のレイ」になる。
        // フルベイクは持たず、初回も含めて常に1フレームm_DDGISettings.ProbesPerFrame個ずつ時間分割で回す
        // (理由はKurenaiEngine3D.hのm_DDGIWarmingUpのコメント参照)

        // プローブ1面ぶんのキャプチャ → スクラッチのキューブ2本(放射輝度・距離)の該当面へコピー。
        // コピーCSはIBLConvolve.hlslのCSCopyCaptureToCubeFaceをそのまま使う。u1の宣言が
        // RWTexture2DArray<float>なので、キューブ配列だけでなく単体のキューブ(=6要素の2D配列)の
        // 面へもそのまま書ける
        const auto captureDDGIProbeFace =
            [this, ambientOcclusionSettings, emissiveLightSettings, &constants, probeFaceProjection, skyTexture, bakedLightCount, materialSamplers, objectConstantBuffer](RHI::IRHICommandList* cmd, uint32_t probeIndex, uint32_t face)
        {
            const DirectX::XMFLOAT3 probePosition = m_Engine.ComputeDDGIProbePosition(probeIndex);

            RHI::Viewport ddgiViewport;
            ddgiViewport.Width = static_cast<float>(kDDGICaptureSize);
            ddgiViewport.Height = static_cast<float>(kDDGICaptureSize);
            RHI::IRHITexture* const captureTargets[] = { m_Engine.m_DDGICaptureColor.get(), m_Engine.m_DDGICaptureDistance.get() };

            FrameConstants captureConstants = constants;
            const DirectX::XMMATRIX faceViewProj = ComputeCubeFaceView(probePosition, face) * probeFaceProjection;
            DirectX::XMStoreFloat4x4(&captureConstants.ViewProj, DirectX::XMMatrixTranspose(faceViewProj));
            captureConstants.CameraPosition = { probePosition.x, probePosition.y, probePosition.z, 0.0f };
            // 反射プローブと同じ理由でドローンの灯を外す。こちらは焼き直しではなく
            // ヒステリシスなので固定はされないが、動く光を追いかけ続けて収束しなくなる
            captureConstants.ActiveLightCount.x = static_cast<float>(bakedLightCount);
            cmd->UpdateBuffer(m_Engine.m_ProbeCaptureConstantBuffer.get(), &captureConstants, sizeof(captureConstants));

            cmd->SetRenderTargets(captureTargets, 2, m_Engine.m_DDGICaptureDepth.get());
            cmd->SetViewport(ddgiViewport);
            cmd->ClearRenderTarget({ 0.0f, 0.0f, 0.0f, 0.0f });
            // Reverse-Zのため遠平面側(NDC z=0.0)。コピー側はこの0を「空」の判定に使う
            cmd->ClearDepth(0.0f);

            // PSOは反射プローブと共通(同じシェーダー・同じレンダーターゲットフォーマット)
            cmd->SetPipelineState(m_Engine.m_ProbeCapturePipelineState.get());
            cmd->SetConstantBuffer(0, m_Engine.m_ProbeCaptureConstantBuffer.get());
            cmd->SetSamplerSet(materialSamplers);

            cmd->SetTexture(4, m_Engine.m_RenderTargets.ShadowCascadeArray.get());
            cmd->SetShaderResourceBuffer(8, m_Engine.m_LightBuffer.get());
            cmd->SetTexture(9, m_Engine.m_IrradianceTexture.get());
            cmd->SetTexture(10, m_Engine.m_PrefilteredEnvTexture.get());
            cmd->SetTexture(11, m_Engine.m_BRDFLUTTexture.get());
            // DDGI(22章)の多重バウンス。ProbeCapture.hlslは拡散の環境光をここから引く。
            // 参照するのは「前フレームまでに焼けているアトラス」で、同じフレームの中でも
            // 既に更新済みのプローブぶんは新しい値になる。DDGIは元々ヒステリシスで
            // 時間収束させる手法なので、この程度の混在は問題にならない
            cmd->SetTexture(12, m_Engine.m_DDGIIrradianceAtlas.get());
            cmd->SetTexture(13, m_Engine.m_DDGIDistanceAtlas.get());

            // 【ここにはフラスタムカリングを入れない】このループのドロー数は
            // ClampDDGIProbesPerFrameToConstantRingが「同じ条件で数えること」を前提に
            // 定数バッファリングの予算を決めている(同関数のコメント)。カリングは
            // プローブの位置ごとに結果が変わるため、予算計算と食い違う。
            // 定数バッファの予算超過は例外ではなくログ1行で続行し、描画が静かに壊れる
            // (DX12Buffer.h)ため、整合が取れるまでは入れないほうが安全
            // 【DDGIからだけ自発光を抜く】プロキシとして起こした発光は既にGPULight(型3)から
            // 入っているので、プローブが同じ面を「明るい面」として焼くと二重に数える。
            // **反射プローブでは抑止しない** ―― 同じProbeCapture.hlslを共有しているが、
            // 鏡面が光源を直接見ているのは二重計上ではなく、消すと看板が鏡に映らなくなる。
            // だから材質のフラグではなくCPUのパスごとに決めている
            const bool suppressEmissiveForDDGI = m_Engine.ShouldSuppressEmissiveForGI();
            uint32_t ddgiDrawnMeshes = 0;
            uint32_t ddgiEmissiveMeshes = 0;
            uint32_t ddgiSuppressedMeshes = 0;
            uint32_t ddgiLODMismatchMeshes = 0;
            // 【DDGIも最も粗い段】理由は反射プローブと同じ。
            // ストリーミング中で未読み込みなら描かない。
            // 【カリングを一切行わない】上のコメントのとおり、プローブの位置ごとに結果が
            // 変わるため定数バッファの予算計算と食い違う。錐台を渡さないことでそれを表す
            // (統計にも入らない)
            Rendering::GeometryDrawLoopDesc ddgiLoop;
            ddgiLoop.Frustum = nullptr;
            ddgiLoop.UseDrawUnits = false;
            ddgiLoop.LODMode = Rendering::GeometryLODMode::Coarsest;
            // 半透明メッシュを焼かない理由は反射プローブと同じ(不透明として描かれるため、
            // ガラスが壁になって裏の景色が欠ける)
            ddgiLoop.MeshFilter = Rendering::GeometryMeshFilter::Opaque;
            ddgiLoop.MeshCulling = false;

            m_Engine.ForEachGeometryDraw(
                ddgiLoop,
                [](const Rendering::InstanceDrawUnit&, const Assets::Model&, float) { return false; },
                [&](const Rendering::InstanceDrawUnit& unit, const Assets::Model& coarsestModel,
                    const Assets::Mesh& mesh, float)
                {
                    const Assets::ModelInstance& instance = *unit.Instance;
                    // 【診断にだけ使う】抑止するかどうかの判定には入れないこと ――
                    // レイトレ側(RaytracingMaterial::Flags)はインスタンスを見ないので、
                    // ここだけ条件を増やすと2経路で判定がずれる軸が1本増える。
                    // いまは「プロキシを作ったインスタンス」と「クラスタを持つメッシュ」が
                    // 必ず一致するため冗長でもあるが、将来プロキシ生成に条件が入ったときに
                    // 静かに乖離する形になる
                    const bool instanceHasProxy =
                        unit.InstanceIndex < m_Engine.m_EmissiveProxyInstances.size()
                        && m_Engine.m_EmissiveProxyInstances[unit.InstanceIndex];

                    // 【シェーダーには手を入れない】倍率を0にすればEmissiveFactorごと0になる。
                    // 判定をクラスタの有無で行うのは、係数が0でないのにテクスチャの平均が
                    // 真っ黒でクラスタが出ないメッシュがあり、そちらは光源になっていないため
                    // 【レイトレ側とまったく同じ述語にする】あちらは
                    // RaytracingScene.cpp が !mesh.EmissiveClusters.empty() だけで印を付ける。
                    // 条件が1つでも違うと、環境によって二重計上の有無が変わる
                    const bool meshIsProxySource = suppressEmissiveForDDGI && !mesh.EmissiveClusters.empty();
                    const float ddgiEmissiveIntensity = meshIsProxySource ? 0.0f : emissiveLightSettings.Intensity;
                    ++ddgiDrawnMeshes;
                    const float emissiveMax =
                        std::max({ mesh.EmissiveFactor[0], mesh.EmissiveFactor[1], mesh.EmissiveFactor[2] });
                    if (emissiveMax > 0.0f) { ++ddgiEmissiveMeshes; }
                    if (meshIsProxySource) { ++ddgiSuppressedMeshes; }
                    // 【2経路で判定がずれうる唯一の条件】レイトレ側の印は段0のクラスタで付くが、
                    // こちらが描くのは最も粗い段。粗い段でクラスタが消えている(簡略化で発光
                    // 三角形が落ちた等)と、レイトレは抑止するのにラスタは抑止せず二重に数える。
                    // **絵からは分からない**ので、条件に当たったことだけは残す
                    if (instanceHasProxy && emissiveMax > 0.0f && mesh.EmissiveClusters.empty())
                    {
                        ++ddgiLODMismatchMeshes;
                    }
                    const ObjectConstants objectConstants = MakeObjectConstants(instance, coarsestModel, mesh, ddgiEmissiveIntensity, ambientOcclusionSettings.OcclusionMapEnabled, m_Engine.m_MeshletLODFrame);
                    cmd->UpdateBuffer(objectConstantBuffer, &objectConstants, sizeof(objectConstants));
                    cmd->SetConstantBuffer(1, objectConstantBuffer);

                    cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                    cmd->SetIndexBuffer(mesh.IndexBuffer.get());

                    cmd->SetTexture(0, mesh.BaseColorTexture);
                    cmd->SetTexture(1, mesh.NormalTexture);
                    cmd->SetTexture(2, mesh.MetallicRoughnessTexture);
                    cmd->SetTexture(3, mesh.EmissiveTexture);

                    cmd->DrawIndexed(mesh.IndexCount, 0, 0);
                    return true;
                });

            if (!m_Engine.m_DDGIEmissiveSuppressLoggedRaster)
            {
                m_Engine.m_DDGIEmissiveSuppressLoggedRaster = true;
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    std::string("DDGI(ラスタ)の自発光: 抑止 ") + (suppressEmissiveForDDGI ? "する" : "しない") +
                        " / 描いたメッシュ " + std::to_string(ddgiDrawnMeshes) + "個(うち自発光 " +
                        std::to_string(ddgiEmissiveMeshes) + "個) / 0にしたメッシュ " +
                        std::to_string(ddgiSuppressedMeshes) + "個 / 自発光の強度 " +
                        std::to_string(emissiveLightSettings.Intensity));
                if (ddgiLODMismatchMeshes > 0)
                {
                    Core::Logger::Warning(
                        "KurenaiEngine3D",
                        "DDGI(ラスタ)で抑止できない自発光メッシュがあります: " +
                            std::to_string(ddgiLODMismatchMeshes) +
                            "個。粗いモデルLODでエミッシブのかたまりが消えているため、"
                            "レイトレ経路だけが抑止し、この経路では二重計上が残ります");
                }
            }

            // 描いたカラー/深度をコンピュートからSRVで読むため、先にRTVを外す(DX11の制約)
            cmd->SetRenderTargets(nullptr, 0, nullptr);

            IBLFaceConstants faceConstants{};
            faceConstants.Face = face;
            cmd->SetComputePipelineState(m_Engine.m_ProbeCubeCopyPipelineState.get());
            cmd->UpdateBuffer(m_Engine.m_IBLPrefilterConstantBuffer.get(), &faceConstants, sizeof(faceConstants));
            cmd->SetComputeConstantBuffer(0, m_Engine.m_IBLPrefilterConstantBuffer.get());
            cmd->SetComputeSamplerSet(materialSamplers);
            cmd->SetComputeTexture(0, skyTexture);
            cmd->SetComputeTexture(1, m_Engine.m_DDGICaptureColor.get());
            cmd->SetComputeTexture(2, m_Engine.m_DDGICaptureDepth.get());
            cmd->SetComputeTexture(3, m_Engine.m_DDGICaptureDistance.get());
            cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_Engine.m_DDGICaptureRadianceCube.get(), face, 0, 0);
            cmd->SetComputeUnorderedAccessTextureCubeFace(1, m_Engine.m_DDGICaptureDistanceCube.get(), face, 0, 0);
            cmd->Dispatch((kDDGICaptureSize + 7) / 8, (kDDGICaptureSize + 7) / 8, 1);
        };

        // captureDDGIProbeFaceのDXR版。ラスタライズとキューブへの書き写しをまとめて置き換え、
        // 同じスクラッチキューブ2本を1ディスパッチで直接埋める。
        //
        // 【面ごとに1ディスパッチなのはRHIの制約】キューブのUAVは面ごと(要素数1のTexture2DArray)に
        // しか張れないため、6面を1回のディスパッチへ畳むにはRHIへ「キューブ全スライスを
        // RWTexture2DArrayとして張る」メソッドをDX11/DX12の両方へ足す必要がある。
        // ドローとメッシュ走査が消えるのが本題なので、そこは測ってから決める
        const auto traceDDGIProbeFace =
            [this, ddgiSettings, emissiveLightSettings, skyTexture, bakedLightCount, materialSamplers, frameConstantBuffer](RHI::IRHICommandList* cmd, uint32_t probeIndex, uint32_t face)
        {
            const DirectX::XMFLOAT3 probePosition = m_Engine.ComputeDDGIProbePosition(probeIndex);

            DDGITraceConstants traceConstants{};
            traceConstants.Params0 = {
                probePosition.x, probePosition.y, probePosition.z, static_cast<float>(face)
            };
            // w = プロキシとして起こされたマテリアルの自発光倍率。0.0で抑止、1.0でそのまま。
            // ラスタ経路(ObjectConstantsの倍率を0にする)と**同じ判定**から決めること
            traceConstants.Params1 = {
                static_cast<float>(kDDGICaptureSize),
                emissiveLightSettings.Intensity,
                ddgiSettings.SunShadowRayEnabled ? 1.0f : 0.0f,
                m_Engine.ShouldSuppressEmissiveForGI() ? 0.0f : 1.0f
            };
            // 舐めるライトの数。ラスタ経路(ProbeCaptureのcaptureConstants)と同じ値にすること。
            // ここだけ揃っていないと、DX11(ラスタ)とDX12(レイトレ)でDDGIの結果が黙って食い違う
            traceConstants.Params2 = { static_cast<float>(bakedLightCount), 0.0f, 0.0f, 0.0f };

            if (!m_Engine.m_DDGIEmissiveSuppressLoggedTrace)
            {
                m_Engine.m_DDGIEmissiveSuppressLoggedTrace = true;
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    "DDGI(レイトレ)の自発光: Params1.y(強度) " + std::to_string(traceConstants.Params1.y) +
                        " / Params1.w(プロキシ材質の倍率) " + std::to_string(traceConstants.Params1.w) +
                        " / プロキシ印の付いた材質 " + std::to_string(m_Engine.m_RaytracingScene.GetEmissiveProxyMaterialCount()) +
                        "件 / 全メッシュ " + std::to_string(m_Engine.m_RaytracingScene.GetMeshCount()) + "件");
            }

            cmd->SetComputePipelineState(m_Engine.m_DDGIProbeTracePipelineState.get());
            // ヒット面のマテリアルテクスチャをbindlessで引くためs0にWrapが要る(RTAOと同じ理由)
            cmd->SetComputeSamplerSet(materialSamplers);
            cmd->UpdateBuffer(m_Engine.m_DDGITraceConstantBuffer.get(), &traceConstants, sizeof(traceConstants));
            // b0はこのフレームのFrameConstantsをそのまま使う。ラスタ経路と違い、
            // プローブ位置は専用の定数バッファ(b1)で渡すのでViewProjを差し替える必要が無い
            cmd->SetComputeConstantBuffer(0, frameConstantBuffer);
            cmd->SetComputeConstantBuffer(1, m_Engine.m_DDGITraceConstantBuffer.get());

            cmd->SetComputeAccelerationStructure(0, m_Engine.m_RaytracingScene.GetTopLevelAS());
            cmd->SetComputeShaderResourceBuffer(1, m_Engine.m_RaytracingScene.GetVertexAttributeBuffer());
            cmd->SetComputeShaderResourceBuffer(2, m_Engine.m_RaytracingScene.GetIndexBuffer());
            cmd->SetComputeShaderResourceBuffer(3, m_Engine.m_RaytracingScene.GetMeshInfoBuffer());
            cmd->SetComputeShaderResourceBuffer(4, m_Engine.m_RaytracingScene.GetInstanceInfoBuffer());
            cmd->SetComputeShaderResourceBuffer(5, m_Engine.m_RaytracingScene.GetMaterialBuffer());
            // メッシュレット表(t6)。このシェーダー自身は引かないが、共有ヘッダーの
            // RaytracingScene.hlsliが宣言を持つためバインドしておく(RTAOと同じ扱い)。
            // メッシュレットを持つメッシュが1つも無いシーンではバッファ自体が無い
            if (RHI::IRHIBuffer* meshletBuffer = m_Engine.m_RaytracingScene.GetMeshletTriangleOffsetBuffer())
            {
                cmd->SetComputeShaderResourceBuffer(6, meshletBuffer);
            }
            cmd->SetComputeShaderResourceBuffer(7, m_Engine.m_LightBuffer.get());
            cmd->SetComputeTexture(8, m_Engine.m_IrradianceTexture.get());
            cmd->SetComputeTexture(9, m_Engine.m_PrefilteredEnvTexture.get());
            cmd->SetComputeTexture(10, m_Engine.m_BRDFLUTTexture.get());
            cmd->SetComputeTexture(11, skyTexture);
            // DDGIの多重バウンス。ラスタ経路がt12/t13で引いているのと同じアトラス
            cmd->SetComputeTexture(12, m_Engine.m_DDGIIrradianceAtlas.get());
            cmd->SetComputeTexture(13, m_Engine.m_DDGIDistanceAtlas.get());

            // UAVはDispatch直後に解除されるため毎回バインドし直す(IRHICommandList.h参照)
            cmd->SetComputeUnorderedAccessTextureCubeFace(0, m_Engine.m_DDGICaptureRadianceCube.get(), face, 0, 0);
            cmd->SetComputeUnorderedAccessTextureCubeFace(1, m_Engine.m_DDGICaptureDistanceCube.get(), face, 0, 0);
            cmd->Dispatch((kDDGICaptureSize + 7) / 8, (kDDGICaptureSize + 7) / 8, 1);
        };

        // 組み上がったキューブ2本から、オクタヘドラルアトラスの該当セルを焼き直す。
        // 境界の複製は本体の書き込みが全て終わってからでないと正しい値を読めないので別ディスパッチ
        const auto updateDDGIProbe = [this, effectiveExposure, materialSamplers](RHI::IRHICommandList* cmd, uint32_t probeIndex, bool overwrite)
        {
            DDGIUpdateConstants updateConstants{};
            updateConstants.Params0 = {
                static_cast<float>(probeIndex),
                m_Engine.m_GIVolume.Hysteresis,
                m_Engine.m_GIVolume.MaxRayDistance,
                static_cast<float>(kDDGICaptureSize),
            };
            updateConstants.Params1 = {
                static_cast<float>(kDDGIIrradianceTexels),
                static_cast<float>(kDDGIDistanceTexels),
                static_cast<float>(kDDGIProbeBorder),
                overwrite ? 1.0f : 0.0f,
            };
            updateConstants.Params2 = {
                static_cast<float>(m_Engine.m_GIVolume.ProbeCounts[0]),
                static_cast<float>(m_Engine.m_GIVolume.ProbeCounts[1]),
                static_cast<float>(m_Engine.m_GIVolume.ProbeCounts[2]),
                effectiveExposure,
            };
            cmd->UpdateBuffer(m_Engine.m_DDGIUpdateConstantBuffer.get(), &updateConstants, sizeof(updateConstants));

            // 本体の書き込み。スレッドは2つの解像度の広いほうに合わせて起動し、
            // それぞれの範囲外はシェーダー側で弾く
            constexpr uint32_t kUpdateThreads = (kDDGIIrradianceTexels > kDDGIDistanceTexels)
                ? kDDGIIrradianceTexels : kDDGIDistanceTexels;
            cmd->SetComputePipelineState(m_Engine.m_DDGIProbeUpdatePipelineState.get());
            cmd->SetComputeConstantBuffer(0, m_Engine.m_DDGIUpdateConstantBuffer.get());
            cmd->SetComputeSamplerSet(materialSamplers);
            cmd->SetComputeTexture(0, m_Engine.m_DDGICaptureRadianceCube.get());
            cmd->SetComputeTexture(1, m_Engine.m_DDGICaptureDistanceCube.get());
            cmd->SetComputeUnorderedAccessTexture(0, m_Engine.m_DDGIIrradianceAtlas.get());
            cmd->SetComputeUnorderedAccessTexture(1, m_Engine.m_DDGIDistanceAtlas.get());
            cmd->Dispatch((kUpdateThreads + 7) / 8, (kUpdateThreads + 7) / 8, 1);

            // 境界の複製。セル全体(境界込み)を走査するので広いほうのセルサイズに合わせる
            constexpr uint32_t kBorderThreads = (kDDGIIrradianceCell > kDDGIDistanceCell)
                ? kDDGIIrradianceCell : kDDGIDistanceCell;
            cmd->SetComputePipelineState(m_Engine.m_DDGIBorderCopyPipelineState.get());
            cmd->SetComputeConstantBuffer(0, m_Engine.m_DDGIUpdateConstantBuffer.get());
            cmd->SetComputeUnorderedAccessTexture(0, m_Engine.m_DDGIIrradianceAtlas.get());
            cmd->SetComputeUnorderedAccessTexture(1, m_Engine.m_DDGIDistanceAtlas.get());
            cmd->Dispatch((kBorderThreads + 7) / 8, (kBorderThreads + 7) / 8, 1);
        };

        // 焼き上がりに影響する状態が変わったら、停止していた更新を再開する。
        // 【判定はm_DDGISettings.Enabled等のガードの外に置く】無効な間も署名を追い続けないと、
        // 無効中に時刻を動かして再度有効にしたとき「署名は同じ」と誤判定して止まったままになる
        if (m_Engine.m_HasGIVolume && m_Engine.m_DDGIProbeCount > 0)
        {
            const uint64_t bakeSignature = m_Engine.ComputeProbeBakeSignature();
            if (!m_Engine.m_DDGIBakeSignatureValid || bakeSignature != m_Engine.m_DDGIBakeSignature)
            {
                m_Engine.m_DDGIBakeSignature = bakeSignature;
                m_Engine.m_DDGIBakeSignatureValid = true;
                m_Engine.m_DDGIStableCycles = 0;
                m_Engine.m_DDGIUpdateSuspended = false;
            }
        }

        if (frame.Settings.DDGI.Enabled && m_Engine.m_HasGIVolume && m_Engine.m_DDGIProbeCount > 0 && !m_Engine.m_DDGIUpdateSuspended)
        {
            // レイの取得をどちらで行うか。パスの登録とキャプチャの実行で同じ判定を使う
            const bool useRaytracedTrace = m_Engine.ShouldRunRaytracedDDGITrace();

            // 【どちらの経路が実際に走ったかをログに残す】切り替えたつもりで切り替わっていない、
            // という取り違えをA/B比較の前に潰すため。切り替わったときだけ出す
            if (!m_Engine.m_DDGIRayModeReported || m_Engine.m_DDGIRayModeReportedRaytraced != useRaytracedTrace)
            {
                m_Engine.m_DDGIRayModeReported = true;
                m_Engine.m_DDGIRayModeReportedRaytraced = useRaytracedTrace;
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    useRaytracedTrace
                        ? std::string("DDGIのレイ取得: レイトレーシング(DXR)。太陽の影レイ: ") +
                              (frame.Settings.DDGI.SunShadowRayEnabled ? "有効" : "無効")
                        : std::string("DDGIのレイ取得: ラスタライズ"));
            }

            uint32_t perFrame = std::min<uint32_t>(
                static_cast<uint32_t>(std::max(frame.Settings.DDGI.ProbesPerFrame, 1)), m_Engine.m_DDGIProbeCount);
            if (!useRaytracedTrace)
            {
                // 1フレームの描画回数・定数書き込み回数の上限はラスタ経路だけの制約。
                // レイトレース経路はメッシュごとの描画をしないので抑える必要が無い
                perFrame = m_Engine.ClampDDGIProbesPerFrameToConstantRing(perFrame);
            }
            const bool warmingUp = m_Engine.m_DDGIWarmingUp;

            // 実効プリ露出が大きく動いたら、一巡ぶんだけ上書きへ切り替えて即座に追従させる
            // (理由はKurenaiEngine3D.hのm_DDGIOverwriteRemainingのコメント参照)。
            // 一巡目(warmingUp)は元から上書きなので何もしない
            if (!warmingUp)
            {
                if (!m_Engine.m_DDGILastExposureValid)
                {
                    m_Engine.m_DDGILastExposureEV100 = m_Engine.m_EffectiveExposureEV100;
                    m_Engine.m_DDGILastExposureValid = true;
                }
                else if (std::abs(m_Engine.m_EffectiveExposureEV100 - m_Engine.m_DDGILastExposureEV100) > kDDGIExposureRewarmEV)
                {
                    m_Engine.m_DDGIOverwriteRemaining = m_Engine.m_DDGIProbeCount;
                    m_Engine.m_DDGILastExposureEV100 = m_Engine.m_EffectiveExposureEV100;
                }
            }
            // このフレームで上書きするぶんを先に確定させる(ラムダへ値で渡すため)
            const uint32_t overwriteThisFrame = std::min(m_Engine.m_DDGIOverwriteRemaining, perFrame);
            m_Engine.m_DDGIOverwriteRemaining -= overwriteThisFrame;

            // 【止めるモードでは停止するまでの全巡回を上書きで焼く】理由はKurenaiEngine3D.hの
            // kDDGIBounceCyclesのコメント参照。露出追従のm_DDGIOverwriteRemainingとは
            // 独立に効かせたいので、残数を消費せず条件だけ合流させる
            const bool overwriteWholeCycle = !warmingUp && frame.Settings.DDGI.UpdateMode != DDGIUpdateMode::Always;

            // --- 格子のスクロールで未確定になったスロットを拾う ---
            //
            // カメラが動くと、各LODの原点がその段の格子へスナップし直される。スナップして
            // いるのでプローブのワールド座標そのものは動かないが、範囲から抜けた列のセルが
            // 反対側の新しい列へ回るため、そのセルは「別の場所を担当する」ようになる。
            // そこには前の場所のイラディアンスが残っているので、焼き直すまで使ってはいけない。
            m_Engine.m_DDGIDirtyProbeList.clear();
            if (m_Engine.m_DDGIProbeBakedCoord.size() == m_Engine.m_DDGIProbeCount)
            {
                for (uint32_t slot = 0; slot < m_Engine.m_DDGIProbeCount; ++slot)
                {
                    const DirectX::XMINT3 current = m_Engine.ComputeDDGIProbeWorldCoord(slot);
                    const DirectX::XMINT3& baked = m_Engine.m_DDGIProbeBakedCoord[slot];
                    if (current.x != baked.x || current.y != baked.y || current.z != baked.z)
                    {
                        m_Engine.m_DDGIDirtyProbeList.push_back(slot);
                    }
                }
            }

            // 【細かいLODを優先する】通し番号は LOD0 が先頭に並ぶので、番号順に詰めるだけで
            // 「細かい段から先に焼き直す」になる。見ている場所の間接光が先に確定する。
            //
            // 未確定が1フレームの予算を超えるときは、超えたぶんが次フレーム以降へ回る。
            // その間そのスロットはαの印(2.0)でサンプリングから外れているので、
            // 「別の場所の色を配る」ことは無い(遅れるだけで壊れない)
            const uint32_t dirtyThisFrame =
                std::min<uint32_t>(static_cast<uint32_t>(m_Engine.m_DDGIDirtyProbeList.size()), perFrame);

            // 【スナップが効いているかを数で見るためのログ】カメラが1セル未満しか動かなければ
            // どのプローブもワールド座標を変えないので、未確定は0のままでなければならない。
            // セル境界をまたぐと、その軸に垂直な面1枚ぶんが一度に未確定になる。
            // 追従が「スナップせず連続的に動く」実装になっていると毎フレーム全数が未確定になるので、
            // この数を見れば取り違えにすぐ気づける
            if (m_Engine.m_GIVolume.FollowCamera && !m_Engine.m_DDGIDirtyProbeList.empty())
            {
                // 【LOD0の基準格子座標も出す】これが同じなら格子は同じ場所にある。
                // 往復の検証で「カメラが同じセルへ戻ったか」を、絵ではなく数で確かめられる
                const DirectX::XMINT3 base0 = m_Engine.ComputeDDGILODBaseIndex(0);
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    "DDGIの格子がスクロールしました: 未確定 " +
                        std::to_string(m_Engine.m_DDGIDirtyProbeList.size()) + " / " +
                        std::to_string(m_Engine.m_DDGIProbeCount) + " スロット(このフレームで焼き直すのは " +
                        std::to_string(dirtyThisFrame) + " 個) LOD0基準=(" +
                        std::to_string(base0.x) + "," + std::to_string(base0.y) + "," +
                        std::to_string(base0.z) + ")");
            }

            // --- 未確定のスロットを、焼き直されるまでサンプリングから外す ---
            //
            // 【焼ける数より多くてもすべて外す】このフレームで焼けるのは perFrame 個までだが、
            // 印を付けるのは全部に対して行う。付けそこねたスロットは、焼き直されるまでの間
            // 「別の場所のイラディアンス」を配り続けることになる
            if (!m_Engine.m_DDGIDirtyProbeList.empty() && m_Engine.m_DDGIInvalidateProbesPipelineState && m_Engine.m_DDGIDirtyProbeBuffer)
            {
                const uint32_t dirtyCount =
                    std::min<uint32_t>(static_cast<uint32_t>(m_Engine.m_DDGIDirtyProbeList.size()), kDDGIMaxProbes);
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "DDGIInvalidate",
                    .Writes = { m_Engine.m_DDGIIrradianceAtlas.get() },
                    .Execute = [this, dirtyCount](RHI::IRHICommandList* cmd)
                    {
                        cmd->UpdateBuffer(
                            m_Engine.m_DDGIDirtyProbeBuffer.get(), m_Engine.m_DDGIDirtyProbeList.data(),
                            dirtyCount * static_cast<uint32_t>(sizeof(uint32_t)));

                        DDGIUpdateConstants invalidateConstants{};
                        // このパスだけ Params0.x は「無効化する個数」の意味で使う
                        invalidateConstants.Params0 = {
                            static_cast<float>(dirtyCount), 0.0f, 0.0f, static_cast<float>(kDDGICaptureSize)
                        };
                        invalidateConstants.Params1 = {
                            static_cast<float>(kDDGIIrradianceTexels), static_cast<float>(kDDGIDistanceTexels),
                            static_cast<float>(kDDGIProbeBorder), 0.0f
                        };
                        invalidateConstants.Params2 = {
                            static_cast<float>(m_Engine.m_GIVolume.ProbeCounts[0]),
                            static_cast<float>(m_Engine.m_GIVolume.ProbeCounts[1]),
                            static_cast<float>(m_Engine.m_GIVolume.ProbeCounts[2]), 1.0f
                        };
                        cmd->UpdateBuffer(
                            m_Engine.m_DDGIUpdateConstantBuffer.get(), &invalidateConstants, sizeof(invalidateConstants));

                        cmd->SetComputePipelineState(m_Engine.m_DDGIInvalidateProbesPipelineState.get());
                        cmd->SetComputeConstantBuffer(0, m_Engine.m_DDGIUpdateConstantBuffer.get());
                        cmd->SetComputeShaderResourceBuffer(2, m_Engine.m_DDGIDirtyProbeBuffer.get());
                        // UAVはDispatch直後に解除されるため毎回バインドし直す
                        cmd->SetComputeUnorderedAccessTexture(0, m_Engine.m_DDGIIrradianceAtlas.get());
                        // 1グループ = 1プローブのセル
                        cmd->Dispatch(dirtyCount, 1, 1);
                    },
                });
            }

            for (uint32_t i = 0; i < perFrame; ++i)
            {
                // 未確定のスロットを先に消化し、余った枠を通常のラウンドロビンへ回す
                const bool isDirtySlot = (i < dirtyThisFrame);
                const uint32_t probeIndex = isDirtySlot
                    ? m_Engine.m_DDGIDirtyProbeList[i]
                    : (m_Engine.m_DDGIUpdateCursor + (i - dirtyThisFrame)) % m_Engine.m_DDGIProbeCount;

                // 一巡目はヒステリシスを使わず上書きする(混ぜる相手の「前の値」が未初期化のため)。
                // 露出が急変した直後も同じく上書きで追従させる。
                // **未確定のスロットも必ず上書き** ―― 前の値は別の場所のものなので混ぜてはいけない
                const bool overwrite =
                    warmingUp || overwriteWholeCycle || isDirtySlot || (i < overwriteThisFrame);

                // このスロットを焼いたので、担当しているワールド格子座標を記録し直す
                if (m_Engine.m_DDGIProbeBakedCoord.size() == m_Engine.m_DDGIProbeCount)
                {
                    m_Engine.m_DDGIProbeBakedCoord[probeIndex] = m_Engine.ComputeDDGIProbeWorldCoord(probeIndex);
                }

                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "DDGIUpdate" + std::to_string(probeIndex),
                    .Reads = probeCaptureReads,
                    .Writes = {
                        m_Engine.m_DDGICaptureColor.get(), m_Engine.m_DDGICaptureDistance.get(), m_Engine.m_DDGICaptureDepth.get(),
                        m_Engine.m_DDGICaptureRadianceCube.get(), m_Engine.m_DDGICaptureDistanceCube.get(),
                        m_Engine.m_DDGIIrradianceAtlas.get(), m_Engine.m_DDGIDistanceAtlas.get(),
                    },
                    .Execute = [captureDDGIProbeFace, traceDDGIProbeFace, updateDDGIProbe, probeIndex, overwrite, useRaytracedTrace](RHI::IRHICommandList* cmd)
                    {
                        // レイの取得だけを差し替える。埋めるスクラッチキューブも、
                        // そのあとの更新CSも同じものを使う(A/Bの差分をレイ取得に限定するため)
                        for (uint32_t face = 0; face < kCubeFaceCount; ++face)
                        {
                            if (useRaytracedTrace)
                            {
                                traceDDGIProbeFace(cmd, probeIndex, face);
                            }
                            else
                            {
                                captureDDGIProbeFace(cmd, probeIndex, face);
                            }
                        }
                        updateDDGIProbe(cmd, probeIndex, overwrite);
                    },
                });
            }

            // 【未確定ぶんはカーソルを進めない】未確定のスロットは番号順ではなく飛び飛びに
            // 選ばれるので、その枠までカーソルを進めると通常の巡回に穴が空く
            const uint32_t nextCursor = m_Engine.m_DDGIUpdateCursor + (perFrame - dirtyThisFrame);
            const bool cycleCompleted = nextCursor >= m_Engine.m_DDGIProbeCount;

            // 一巡ぶん焼き終えるたびに数え、モードごとの巡回数に達したら止める。
            // 【上書きが残っている間は止めない】まだ焼き切っていないため。
            // 一巡目(warmingUp)はこの後の分岐で別に扱うのでここでは数えない
            if (cycleCompleted && !warmingUp && frame.Settings.DDGI.UpdateMode != DDGIUpdateMode::Always)
            {
                ++m_Engine.m_DDGIStableCycles;
                if (m_Engine.m_DDGIOverwriteRemaining == 0)
                {
                    const uint32_t requiredCycles = (frame.Settings.DDGI.UpdateMode == DDGIUpdateMode::OverwriteThenStop)
                        ? 1u
                        : kDDGIBounceCycles;
                    if (m_Engine.m_DDGIStableCycles >= requiredCycles)
                    {
                        m_Engine.m_DDGIUpdateSuspended = true;
                        Core::Logger::Info(
                            "KurenaiEngine3D",
                            "DDGIが収束したため更新を停止しました(" + std::to_string(m_Engine.m_DDGIStableCycles) +
                                "巡)。焼き上がりに影響する状態が変わると再開します");
                    }
                }
            }

            if (warmingUp && cycleCompleted)
            {
                // 全プローブが一度ずつ書かれた。ここから先はヒステリシスで滑らかに追従させ、
                // 同時にサンプリング側(DDGIParams0.w)を有効にする
                m_Engine.m_DDGIWarmingUp = false;
                m_Engine.m_DDGIBaked = true;
                m_Engine.m_DDGILastExposureEV100 = m_Engine.m_EffectiveExposureEV100;
                m_Engine.m_DDGILastExposureValid = true;
                Core::Logger::Info(
                    "KurenaiEngine3D",
                    "DDGIの初回一巡が完了しました(" + std::to_string(m_Engine.m_DDGIProbeCount) + "プローブ)");
            }
            m_Engine.m_DDGIUpdateCursor = nextCursor % m_Engine.m_DDGIProbeCount;
        }
    }

    void DDGIPasses::RegisterResolve(
        Core::RenderGraph& graph, const Rendering::RenderFrameContext& frame)
    {
        RHI::IRHIBuffer* const frameConstantBuffer = frame.FrameConstantBuffer;
        RHI::IRHISamplerSet* const screenSpaceSamplers = frame.ScreenSpaceSamplers;

        // 【フレームの値をここで写し取る】Render()から機械的に移した登録コードなので、
        // 参照している名前を変えずに済むよう同じ名前で受け直す
        const FrameConstants& constants = *frame.Constants;

        // --- DDGIの低解像度解決パス(有効なときだけ) ---
        // 拡散イラディアンスとinsideWeightを1/2解像度で求め、Lightingパスが深度を見て
        // アップサンプルする。雲と違い厳密ではない近似のため既定は無効(DDGIResolve.hlsl冒頭参照)
        const bool ddgiResolvePassRuns =
            frame.Settings.DDGI.HalfResolution && m_Engine.m_DDGIResolveTexture && frame.Settings.DDGI.Enabled && m_Engine.m_HasGIVolume && m_Engine.m_DDGIBaked;
        if (ddgiResolvePassRuns)
        {
            RHI::Viewport ddgiResolveViewport;
            ddgiResolveViewport.Width = static_cast<float>(m_Engine.m_DDGIResolveWidth);
            ddgiResolveViewport.Height = static_cast<float>(m_Engine.m_DDGIResolveHeight);

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "DDGIResolve",
                // アトラスはDDGIUpdateパスが書くので、それより後に順序付けさせる。
                // 深度と法線はG-Bufferパスより後
                .Reads = {
                    m_Engine.m_DDGIIrradianceAtlas.get(), m_Engine.m_DDGIDistanceAtlas.get(),
                    m_Engine.m_RenderTargets.GBufferDepth.get(), m_Engine.m_RenderTargets.GBufferNormal.get(),
                },
                // 2枚目は合成側のGatherRed用の低解像度深度(41.24節)。
                // 並びはDDGIResolve.hlslのPSOutputおよびPSOのRenderTargetFormatsと一致させること
                .RenderTargets = { m_Engine.m_DDGIResolveTexture.get(), m_Engine.m_DDGIResolveDepthTexture.get() },
                .Execute = [this, ddgiResolveViewport, frameConstantBuffer, screenSpaceSamplers](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(ddgiResolveViewport);
                    cmd->SetPipelineState(m_Engine.m_DDGIResolvePipelineState.get());
                    cmd->SetConstantBuffer(0, frameConstantBuffer);
                    cmd->SetSamplerSet(screenSpaceSamplers);
                    cmd->SetTexture(0, m_Engine.m_DDGIIrradianceAtlas.get());
                    cmd->SetTexture(1, m_Engine.m_DDGIDistanceAtlas.get());
                    cmd->SetTexture(2, m_Engine.m_RenderTargets.GBufferDepth.get());
                    cmd->SetTexture(3, m_Engine.m_RenderTargets.GBufferNormal.get());
                    cmd->Draw(3, 0);
                },
            });
        }
    }
}
