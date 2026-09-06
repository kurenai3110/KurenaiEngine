#include "../KurenaiEngine3D.h"

#include <algorithm>
#include <vector>

#include "Core/Logger.h"
#include "Core/RenderGraph.h"
#include "LightingPasses.h"
#include "../Rendering/GPULight.h"
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

    void LightingPasses::RegisterDirectAndAO(
        Core::RenderGraph& graph,
        const Rendering::RenderFrameContext& frame,
        Rendering::RenderBlackboard& bb)
    {
        const uint32_t renderWidth = frame.RenderWidth;
        const uint32_t renderHeight = frame.RenderHeight;
        RHI::IRHIBuffer* const frameConstantBuffer = frame.FrameConstantBuffer;
        RHI::IRHISamplerSet* const materialSamplers = frame.MaterialSamplers;
        RHI::IRHISamplerSet* const screenSpaceSamplers = frame.ScreenSpaceSamplers;

        // 【フレームの値をここで写し取る】以下はRender()から機械的に移した登録コードなので、
        // 参照している名前を変えずに済むよう同じ名前で受け直す。
        //
        // ポインタ越しに受けているものは Render() のローカルの実体を指しており、
        // graph.Execute() が終わるまで生きている。したがって参照捕捉のままでよい。
        // 一方、この関数のローカルになるもの(ビューポート)は**値で捕捉する**こと
        const FrameConstants& constants = *frame.Constants;
        const LightingConstants& lightingConstants = *frame.Lighting;
        const std::vector<GPULight>& gpuLights = *frame.Lights;
        const RHI::Viewport gbufferViewport = frame.GBufferViewport;
        const bool usingProceduralSky = frame.UsingProceduralSky;
        const bool megaLightsDenoiseRuns = bb.MegaLightsDenoiseRuns;
        RHI::IRHITexture* const skyTexture = bb.SkyTexture;

        // 直接光パスがt6へバインドする可視率テクスチャ。DirectLighting.hlslは
        // LightCount.zがRaytracedのときしか読まないが、DX12はSetPipelineStateのたびに
        // ルート引数が無効化されるため、シェーダが宣言しているリソースは必ず何かをバインドする
        // 必要がある(nullptrはSetTextureが受け付けない)。非対応環境では読まれないダミーとして
        // 深度テクスチャを張る(Presentのデバッグ用t1/t2/t4に既定値を持たせているのと同じ理由)
        RHI::IRHITexture* const rtShadowTextureForBinding =
            m_Engine.m_RenderTargets.RTShadowTexture ? m_Engine.m_RenderTargets.RTShadowTexture.get() : m_Engine.m_RenderTargets.GBufferDepth.get();

        // 直接光パスがt7へバインドするMegaLightsの寄与。上と同じ理由で、読まれないフレームでも
        // 何かを張る必要がある(非対応環境ではそもそもテクスチャを確保していない)
        // デノイズを通したフレームはその出力を、通さないフレームは生出力を読む。
        // **DirectLighting.hlsl 側は変わらない**(同じ t7)ので、非MegaLights経路には影響しない
        RHI::IRHITexture* megaLightsTextureForBinding =
            m_Engine.m_MegaLightsTexture ? m_Engine.m_MegaLightsTexture.get() : m_Engine.m_RenderTargets.GBufferDepth.get();
        if (megaLightsDenoiseRuns && m_Engine.m_MegaLightsDenoisedTexture)
        {
            megaLightsTextureForBinding = m_Engine.m_MegaLightsDenoisedTexture.get();
        }

        // --- 直接光パス: G-Buffer+シャドウマップ(またはRTシャドウの可視率)からPBRの直接光
        //     (拡散+鏡面反射、シャドウ適用済み)を計算しHDRで書き出す(常に指定した内部解像度)。
        //     DeferredLighting/SSILの両方から読まれる ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "DirectLight",
            .Reads =
            {
                m_Engine.m_RenderTargets.GBufferAlbedo.get(), m_Engine.m_RenderTargets.GBufferNormal.get(), m_Engine.m_RenderTargets.GBufferMaterial.get(), m_Engine.m_RenderTargets.GBufferDepth.get(),
                m_Engine.m_RenderTargets.ShadowCascadeArray.get(),
                // RTシャドウの可視率。RTシャドウパスを実行しないフレームではm_RenderTargets.GBufferDepthと
                // 同じポインタになるが、RenderGraphは同じ書き手への多重エッジを弾くため無害
                rtShadowTextureForBinding,
                // MegaLightsの寄与。MegaLightsパスはこれより前に登録してあるので、
                // ここに挙げることでRAWの辺が張られる(実行しないフレームでは
                // m_RenderTargets.GBufferDepthと同じポインタになるが、多重エッジは無害)
                megaLightsTextureForBinding,
                // スペキュラのエネルギー補正(14.9節)でEss=brdf.x+brdf.yを引くためBRDF積分LUTを読む。
                // Readsに挙げることでRenderGraphがBRDFLUTBakeパス(このLUTのWriter)より後に順序付ける
                m_Engine.m_BRDFLUTTexture.get(),
            },
            .RenderTargets = { m_Engine.m_RenderTargets.DirectLightTexture.get() },
            .Execute = [this, gbufferViewport, &gpuLights, &lightingConstants, rtShadowTextureForBinding, megaLightsTextureForBinding, frameConstantBuffer, screenSpaceSamplers](RHI::IRHICommandList* cmd)
            {
                cmd->SetViewport(gbufferViewport);

                cmd->SetPipelineState(m_Engine.m_DirectLightPipelineState.get());
                cmd->SetConstantBuffer(0, frameConstantBuffer);

                // UpdateBufferはSetConstantBufferより前に呼ぶ必要がある。DX12の定数バッファは
                // リングバッファで、GetGPUVirtualAddress()が現在のリングスロットのアドレスを返すため
                cmd->UpdateBuffer(m_Engine.m_LightingConstantBuffer.get(), &lightingConstants, sizeof(lightingConstants));
                cmd->SetConstantBuffer(1, m_Engine.m_LightingConstantBuffer.get());

                cmd->SetSamplerSet(screenSpaceSamplers);
                cmd->SetTexture(0, m_Engine.m_RenderTargets.GBufferAlbedo.get());
                cmd->SetTexture(1, m_Engine.m_RenderTargets.GBufferNormal.get());
                cmd->SetTexture(2, m_Engine.m_RenderTargets.GBufferMaterial.get());
                cmd->SetTexture(3, m_Engine.m_RenderTargets.GBufferDepth.get());
                cmd->SetTexture(4, m_Engine.m_RenderTargets.ShadowCascadeArray.get());
                // RTシャドウの可視率。LightCount.zがRaytracedのときだけ読まれる
                cmd->SetTexture(6, rtShadowTextureForBinding);
                // MegaLightsが求めたポイント/スポットの直接光。LightCount.wが1のときだけ読まれる
                cmd->SetTexture(7, megaLightsTextureForBinding);

                // ライトが1つも無いフレームでもSetShaderResourceBufferは必ず呼ぶ(SetPipelineStateが
                // 毎回ルート引数を無効化するため、シェーダが宣言しているリソースを未バインドのまま
                // Drawすることになってしまう)。バッファの中身の更新はグラフ構築前に1回だけ済ませてある
                cmd->SetShaderResourceBuffer(8, m_Engine.m_LightBuffer.get());
                // タイルライトカリングが書いたライトグリッド。カリング無効時もシェーダが宣言している
                // リソースは必ずバインドする(上と同じ理由)
                cmd->SetShaderResourceBuffer(5, m_Engine.m_LightTileBuffer.get());
                // スペキュラのエネルギー補正(14.9節)用のBRDF積分LUT。t8はライトリスト
                // (StructuredBuffer)が占有しているためt9に置く
                cmd->SetTexture(9, m_Engine.m_BRDFLUTTexture.get());

                cmd->Draw(3, 0);
            },
        });

        // --- AO/GIパス: 選択中の手法(SSAO / SSIL / RTAO)で遮蔽率(・間接拡散光)を計算し、
        //     ブラーで均す(常に指定した内部解像度)。出力フォーマットはどれもrgb=間接拡散光, a=遮蔽率で共通 ---
        if (frame.Settings.AmbientOcclusion.Enabled)
        {
            RHI::IRHITexture* const aoRawTexture = m_Engine.GetActiveAORawTexture();
            RHI::IRHITexture* const aoBlurredTexture = m_Engine.GetActiveAOTexture();
            const bool useSSIL = !m_Engine.ShouldRunRaytracedAO() && frame.Settings.AmbientOcclusion.Technique == AOTechnique::SSILVisibilityBitmask;

            if (m_Engine.ShouldRunRaytracedAO())
            {
                // RTAOパス。SSAO/SSILと違いコンピュートでUAVへ書くため、レンダーターゲットではなく
                // Writesで宣言する。レジスタ割り当てはRTAO.hlsl側の宣言と一致させること
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "RTAO",
                    // 直接光バッファは、バウンス面が画面に映っているときの再放射の放射輝度として読む
                    // (SSILと同じ理由でDirectLightパスより後に順序付けられる。RTAO.hlsl参照)
                    .Reads = { m_Engine.m_RenderTargets.GBufferNormal.get(), m_Engine.m_RenderTargets.GBufferDepth.get(), m_Engine.m_RenderTargets.DirectLightTexture.get() },
                    .Writes = { aoRawTexture },
                    .Execute = [this, renderWidth, renderHeight, frameConstantBuffer, materialSamplers](RHI::IRHICommandList* cmd)
                    {
                        Passes::RTAOConstants rtAOConstants{};
                        rtAOConstants.Params0 = {
                            static_cast<float>(renderWidth), static_cast<float>(renderHeight),
                            m_Engine.m_AmbientOcclusionSettings.RTAOMaxDistance, m_Engine.m_AmbientOcclusionSettings.RTAOPower
                        };
                        rtAOConstants.Params1 = {
                            static_cast<float>(std::max(1, m_Engine.m_AmbientOcclusionSettings.RTAOSampleCount)), m_Engine.m_AmbientOcclusionSettings.RTAOIntensity,
                            m_Engine.m_AmbientOcclusionSettings.RTAOBounceShadowRayEnabled ? 1.0f : 0.0f, 0.0f
                        };
                        cmd->UpdateBuffer(m_Engine.m_RTAOConstantBuffer.get(), &rtAOConstants, sizeof(rtAOConstants));

                        cmd->SetComputePipelineState(m_Engine.m_RTAOPipelineState.get());
                        // ヒット面のマテリアルテクスチャをbindlessで引くためs0にWrapが要る
                        // (理由はRT反射パスの同じ呼び出しのコメント参照)。
                        // このパスは以前サンプラーセットを一度もバインドしておらず、
                        // 直前のパスが残したセットに依存していた
                        cmd->SetComputeSamplerSet(materialSamplers);
                        cmd->SetComputeConstantBuffer(0, frameConstantBuffer);
                        cmd->SetComputeConstantBuffer(1, m_Engine.m_RTAOConstantBuffer.get());

                        cmd->SetComputeAccelerationStructure(0, m_Engine.m_RaytracingScene.GetTopLevelAS());
                        cmd->SetComputeTexture(1, m_Engine.m_RenderTargets.GBufferNormal.get());
                        cmd->SetComputeTexture(2, m_Engine.m_RenderTargets.GBufferDepth.get());
                        cmd->SetComputeShaderResourceBuffer(3, m_Engine.m_RaytracingScene.GetVertexAttributeBuffer());
                        cmd->SetComputeShaderResourceBuffer(4, m_Engine.m_RaytracingScene.GetIndexBuffer());
                        cmd->SetComputeShaderResourceBuffer(5, m_Engine.m_RaytracingScene.GetMeshInfoBuffer());
                        cmd->SetComputeShaderResourceBuffer(6, m_Engine.m_RaytracingScene.GetInstanceInfoBuffer());
                        cmd->SetComputeShaderResourceBuffer(7, m_Engine.m_RaytracingScene.GetMaterialBuffer());
                        // メッシュレット表(t9)。RTAO.hlsl自体は引かないが、共有ヘッダーの
                        // RaytracingScene.hlsliが宣言を持つためバインドしておく。
                        // メッシュレットを持つメッシュが1つも無いシーンではバッファ自体が無いので
                        // バインドしない(未バインドのスロットは0を返す。RTMeshInfo::MeshletCountも
                        // 0になっているため、シェーダーがここを引くことはない)
                        if (RHI::IRHIBuffer* meshletBuffer = m_Engine.m_RaytracingScene.GetMeshletTriangleOffsetBuffer())
                        {
                            cmd->SetComputeShaderResourceBuffer(9, meshletBuffer);
                        }
                        cmd->SetComputeTexture(8, m_Engine.m_RenderTargets.DirectLightTexture.get());

                        // UAVはDispatch直後に解除されるため毎回バインドし直す(IRHICommandList.h参照)
                        cmd->SetComputeUnorderedAccessTexture(0, m_Engine.m_RTAORawTexture.get());
                        cmd->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);
                    },
                });
            }
            else
            {
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "AO",
                    .Reads = useSSIL
                        ? std::vector<RHI::IRHITexture*>{ m_Engine.m_RenderTargets.GBufferNormal.get(), m_Engine.m_RenderTargets.GBufferDepth.get(), m_Engine.m_RenderTargets.DirectLightTexture.get() }
                        : std::vector<RHI::IRHITexture*>{ m_Engine.m_RenderTargets.GBufferNormal.get(), m_Engine.m_RenderTargets.GBufferDepth.get() },
                    .RenderTargets = { aoRawTexture },
                    .Execute = [this, gbufferViewport, useSSIL, frameConstantBuffer, screenSpaceSamplers](RHI::IRHICommandList* cmd)
                    {
                        cmd->SetViewport(gbufferViewport);
                        cmd->SetConstantBuffer(0, frameConstantBuffer);
                        cmd->SetSamplerSet(screenSpaceSamplers);

                        if (useSSIL)
                        {
                            Passes::SSILConstants ssilConstants{};
                            ssilConstants.Params0 = { m_Engine.m_AmbientOcclusionSettings.SSILRadius, m_Engine.m_AmbientOcclusionSettings.SSILThickness, m_Engine.m_AmbientOcclusionSettings.SSILIntensity, m_Engine.m_AmbientOcclusionSettings.SSILPower };
                            ssilConstants.Params1 = { m_Engine.m_AmbientOcclusionSettings.SSILSliceCount, m_Engine.m_AmbientOcclusionSettings.SSILStepCount, 0u, 0u };
                            cmd->UpdateBuffer(m_Engine.m_SSILConstantBuffer.get(), &ssilConstants, sizeof(ssilConstants));

                            cmd->SetPipelineState(m_Engine.m_SSILPipelineState.get());
                            cmd->SetConstantBuffer(1, m_Engine.m_SSILConstantBuffer.get());
                            cmd->SetTexture(0, m_Engine.m_RenderTargets.GBufferNormal.get());
                            cmd->SetTexture(1, m_Engine.m_RenderTargets.GBufferDepth.get());
                            cmd->SetTexture(2, m_Engine.m_RenderTargets.DirectLightTexture.get());
                            cmd->Draw(3, 0);
                        }
                        else
                        {
                            // UIやプリセットで段数が変わったらカーネルを作り直す。
                            // 先頭N本を流用してはいけない理由はm_AmbientOcclusionSettings.SSAOKernelSizeのコメント参照。
                            // 生成は16回のRNGだけなので毎フレーム比較しても問題にならない
                            const uint32_t kernelSize =
                                std::clamp(m_Engine.m_AmbientOcclusionSettings.SSAOKernelSize, 1u, Passes::kSSAOKernelSizeMax);
                            if (m_Engine.m_SSAOKernel.size() != kernelSize)
                            {
                                m_Engine.m_SSAOKernel = Passes::GenerateSSAOKernel(kernelSize);
                            }

                            // 使わない残りの要素は0のまま(シェーダはsampleCountまでしか読まない)
                            Passes::SSAOConstants ssaoConstants{};
                            std::copy(m_Engine.m_SSAOKernel.begin(), m_Engine.m_SSAOKernel.end(), ssaoConstants.Samples);
                            ssaoConstants.Params = {
                                m_Engine.m_AmbientOcclusionSettings.SSAORadius, m_Engine.m_AmbientOcclusionSettings.SSAORadius * 0.05f, m_Engine.m_AmbientOcclusionSettings.SSAOPower, static_cast<float>(kernelSize) };
                            cmd->UpdateBuffer(m_Engine.m_SSAOConstantBuffer.get(), &ssaoConstants, sizeof(ssaoConstants));

                            cmd->SetPipelineState(m_Engine.m_SSAOPipelineState.get());
                            cmd->SetConstantBuffer(1, m_Engine.m_SSAOConstantBuffer.get());
                            cmd->SetTexture(0, m_Engine.m_RenderTargets.GBufferNormal.get());
                            cmd->SetTexture(1, m_Engine.m_RenderTargets.GBufferDepth.get());
                            cmd->Draw(3, 0);
                        }
                    },
                });
            }

            // ブラーパス: 遮蔽率・間接拡散光のタイル状ノイズをボックスブラーで均す(SSAO/SSIL共通シェーダ)
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "AOBlur",
                .Reads = { aoRawTexture },
                .RenderTargets = { aoBlurredTexture },
                .Execute = [this, gbufferViewport, aoRawTexture, screenSpaceSamplers](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(gbufferViewport);
                    cmd->SetPipelineState(m_Engine.m_AOBlurPipelineState.get());
                    // ブラーはカーネルのタップが画面端で[0,1]を出るため、Wrapのサンプラーが
                    // 1つも入っていないこのセットを明示的にバインドする(直前のパスのバインドが
                    // そのまま残るのに依存してはいけない)
                    cmd->SetSamplerSet(screenSpaceSamplers);
                    cmd->SetTexture(0, aoRawTexture);
                    cmd->Draw(3, 0);
                },
            });
        }

        // デバッグ表示(ブラー前確認用)のため、ブラー前の生バッファへの参照も別途保持しておく。
        // 上のパスが書いた先と必ず一致させるため、どちらも同じアクセサから取る
        RHI::IRHITexture* const activeAOTexture = m_Engine.GetActiveAOTexture();
        RHI::IRHITexture* const activeAORawTexture = m_Engine.GetActiveAORawTexture();
        bb.ActiveAOTexture = activeAOTexture;
        bb.ActiveAORawTexture = activeAORawTexture;

        // --- 雲パス: 積雲と巻雲だけを1/2解像度で評価し、透過率と事前乗算済みの散乱光を書く ---
        //
        // 【なぜ分離したか】雲の評価は背景1画素あたり値ノイズを数十回踏むため極端に重く、
        // Intel UHD Graphics 620 / 1280x720 / DX11 / Release の実測ではLightingパス19.4msのうち
        // 積雲14.5ms + 巻雲1.3msを占めていた。雲は空間周波数が低いので低解像度で評価しても
        // 見た目の劣化が小さく、面積1/4で評価すればそのぶん素直に安くなる。
        // 太陽・星のような高周波成分はLighting側(SkyColorWithoutClouds)に残るためにじまない。
        // 合成が事前乗算のover合成であることを使った厳密な分離である(SkyCloud.hlsl冒頭参照)。
        //
        // 【手続き空が無効なら登録しない】.ksceneでDDSスカイボックスを使う場合、Lightingパスは
        // キューブマップをサンプルする経路(SkyParams.y <= 0.5)へ入り、この結果を一切読まない
        const bool skyCloudPassRuns = m_Engine.m_SkyCloudTexture && (frame.Settings.Sky.AnalyticBackground && usingProceduralSky);
        if (skyCloudPassRuns)
        {
            RHI::Viewport skyCloudViewport;
            skyCloudViewport.Width = static_cast<float>(m_Engine.m_SkyCloudWidth);
            skyCloudViewport.Height = static_cast<float>(m_Engine.m_SkyCloudHeight);

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "SkyCloud",
                // SkyViewBakeより後に順序付けさせる(SkyCloudLayers自体はLUTを引かないが、
                // Sky.hlsliの宣言上バインドが必要で、パスの前後関係も揃えておく)
                .Reads = {
                    m_Engine.m_SkyViewLUT.get(), m_Engine.m_CloudShapeNoiseTexture.get(), m_Engine.m_CloudDetailNoiseTexture.get(),
                    // 焼いたウェザーマップ(H3)。レイマーチの1歩を約8倍安くするためのもので、
                    // ボリューム経路を持つこのパスだけが引く
                    m_Engine.m_CloudWeatherNoiseTexture.get(),
                },
                // 2枚出す。0=散乱光rgb+透過率a、1=fogInFront(P18b。SkyCloud.hlslのPSOutput参照)
                .RenderTargets = { m_Engine.m_SkyCloudTexture.get(), m_Engine.m_SkyCloudFogTexture.get() },
                // 空パラメータ。SkyIntegrateパスより後に順序付けさせる
                .BufferReads = { m_Engine.m_SkyParametersBuffer.get() },
                .Execute = [this, skyCloudViewport, frameConstantBuffer, screenSpaceSamplers](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(skyCloudViewport);
                    cmd->SetPipelineState(m_Engine.m_SkyCloudPipelineState.get());
                    cmd->SetConstantBuffer(0, frameConstantBuffer);
                    cmd->SetSamplerSet(screenSpaceSamplers);
                    cmd->SetTexture(0, m_Engine.m_SkyViewLUT.get());
                    cmd->SetTexture(1, m_Engine.m_CloudShapeNoiseTexture.get());
                    cmd->SetTexture(2, m_Engine.m_CloudDetailNoiseTexture.get());
                    cmd->SetShaderResourceBuffer(3, m_Engine.m_SkyParametersBuffer.get());
                    cmd->SetTexture(4, m_Engine.m_CloudWeatherNoiseTexture.get());
                    cmd->Draw(3, 0);
                },
            });
        }
    }

    void LightingPasses::RegisterSceneLighting(
        Core::RenderGraph& graph,
        const Rendering::RenderFrameContext& frame,
        const Rendering::RenderBlackboard& bb)
    {
        RHI::IRHIBuffer* const frameConstantBuffer = frame.FrameConstantBuffer;
        RHI::IRHIBuffer* const objectConstantBuffer = frame.ObjectConstantBuffer;
        RHI::IRHISamplerSet* const materialSamplers = frame.MaterialSamplers;
        RHI::IRHISamplerSet* const screenSpaceSamplers = frame.ScreenSpaceSamplers;

        // 【フレームの値をここで写し取る】以下はRender()から機械的に移した登録コードなので、
        // 参照している名前を変えずに済むよう同じ名前で受け直す。
        //
        // ポインタ越しに受けているものは Render() のローカルの実体を指しており、
        // graph.Execute() が終わるまで生きている。したがって参照捕捉のままでよい。
        // 一方、この関数のローカルになるもの(ビューポート)は**値で捕捉する**こと
        const FrameConstants& constants = *frame.Constants;
        const LightingConstants& lightingConstants = *frame.Lighting;
        const std::vector<GPULight>& gpuLights = *frame.Lights;
        const DirectX::XMFLOAT3& cameraPosition = frame.CameraPosition;
        const DirectX::XMMATRIX& viewProj = frame.ViewProj;
        const RHI::Viewport gbufferViewport = frame.GBufferViewport;
        const bool usingProceduralSky = frame.UsingProceduralSky;
        RHI::IRHITexture* const skyTexture = bb.SkyTexture;
        RHI::IRHITexture* const activeAOTexture = bb.ActiveAOTexture;

        // --- ライティングパス: G-Bufferを読み、SceneColorへ出力(常に指定した内部解像度) ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Lighting",
            .Reads = {
                m_Engine.m_RenderTargets.GBufferAlbedo.get(), m_Engine.m_RenderTargets.DirectLightTexture.get(), m_Engine.m_RenderTargets.GBufferMaterial.get(), m_Engine.m_RenderTargets.GBufferDepth.get(),
                skyTexture, activeAOTexture, m_Engine.m_RenderTargets.GBufferEmissive.get(), m_Engine.m_RenderTargets.GBufferNormal.get(),
                m_Engine.m_IrradianceTexture.get(), m_Engine.m_PrefilteredEnvTexture.get(), m_Engine.m_BRDFLUTTexture.get(),
                m_Engine.m_RenderTargets.GBufferBentNormal.get(),
                // ProbeBakeパスより後に順序付けさせるために挙げる(実際のバインドはExecute内)。
                // 反射プローブは鏡面専任なので拡散イラディアンス側の配列は無い
                m_Engine.m_ProbePrefilteredArray.get(), m_Engine.m_ProbeDistanceArray.get(),
                // 同じくDDGIUpdateパスより後に順序付けさせるために挙げる(22章)
                m_Engine.m_DDGIIrradianceAtlas.get(), m_Engine.m_DDGIDistanceAtlas.get(),
                // 大気散乱のSkyView LUT。背景の空をここから引くため、
                // SkyViewBakeパスより後に順序付けさせる
                m_Engine.m_SkyViewLUT.get(),
                // 低解像度で評価済みの雲。SkyCloudパスより後に順序付けさせるために挙げる
                // (パスが登録されないフレームでは書き手が居ないので依存も張られない)
                m_Engine.m_SkyCloudTexture.get(), m_Engine.m_SkyCloudFogTexture.get(),
                // 同じく低解像度で評価済みのDDGI。DDGIResolveパスより後に順序付けさせる
                m_Engine.m_DDGIResolveTexture.get(), m_Engine.m_DDGIResolveDepthTexture.get(),
            },
            .RenderTargets = { m_Engine.m_RenderTargets.SceneColor.get() },
            // 空パラメータ。SkyIntegrateパスより後に順序付けさせるために挙げる
            // (実際のバインドはExecute内)
            .BufferReads = { m_Engine.m_SkyParametersBuffer.get() },
            .Execute = [this, gbufferViewport, activeAOTexture, skyTexture, frameConstantBuffer, screenSpaceSamplers](RHI::IRHICommandList* cmd)
            {
                cmd->SetViewport(gbufferViewport);
                // 深度テストに失敗した(=何も描かれていない)ピクセル用の背景色。discardされた箇所に前フレームのデータが
                // 残らないよう、フルスクリーン三角形を描く前に明示的にクリアしておく
                cmd->ClearRenderTarget({ 0.05f, 0.05f, 0.08f, 1.0f });

                cmd->SetPipelineState(m_Engine.m_LightingPipelineState.get());
                cmd->SetConstantBuffer(0, frameConstantBuffer);
                cmd->SetSamplerSet(screenSpaceSamplers);
                cmd->SetTexture(0, m_Engine.m_RenderTargets.GBufferAlbedo.get());
                cmd->SetTexture(1, m_Engine.m_RenderTargets.DirectLightTexture.get());
                cmd->SetTexture(2, m_Engine.m_RenderTargets.GBufferMaterial.get());
                cmd->SetTexture(3, m_Engine.m_RenderTargets.GBufferDepth.get());
                cmd->SetTexture(4, skyTexture);
                cmd->SetTexture(5, activeAOTexture);
                cmd->SetTexture(6, m_Engine.m_RenderTargets.GBufferEmissive.get());
                cmd->SetTexture(7, m_Engine.m_RenderTargets.GBufferNormal.get());
                cmd->SetTexture(8, m_Engine.m_IrradianceTexture.get());
                cmd->SetTexture(9, m_Engine.m_PrefilteredEnvTexture.get());
                cmd->SetTexture(10, m_Engine.m_BRDFLUTTexture.get());
                // 反射プローブ(19章、鏡面専任なので拡散イラディアンス側のスロットは無い)。
                // FrameConstants.ProbeParams.xが0のとき(未ベイク・無効時)はシェーダー側が
                // 選択ループを回さないため中身は参照されないが、DX12はディスクリプタテーブルに
                // 未初期化のスロットが残ると動作が未定義になるため常にバインドする
                cmd->SetTexture(12, m_Engine.m_ProbePrefilteredArray.get());
                cmd->SetShaderResourceBuffer(13, m_Engine.m_ProbeBuffer.get());
                cmd->SetTexture(14, m_Engine.m_ProbeDistanceArray.get());
                // DDGI(22章)。反射プローブと同じ理由で、無効時も含めて常にバインドする
                cmd->SetTexture(15, m_Engine.m_DDGIIrradianceAtlas.get());
                cmd->SetTexture(16, m_Engine.m_DDGIDistanceAtlas.get());
                // 空パラメータ。t11に置く(t17はbent normalが使う)
                cmd->SetShaderResourceBuffer(11, m_Engine.m_SkyParametersBuffer.get());
                // bent normal(34章)
                cmd->SetTexture(17, m_Engine.m_RenderTargets.GBufferBentNormal.get());
                // 低解像度で評価済みの雲(rgb=事前乗算済みの散乱光、a=透過率)。
                // このシェーダーは雲を自前で評価しなくなったため、3Dノイズが使っていたt18を
                // そのまま流用している(DeferredLighting.hlsl冒頭のコメント参照)
                cmd->SetTexture(18, m_Engine.m_SkyCloudTexture.get());
                // 同じパスが書いた fogInFront(P18b)。雲の手前の霞の色を晴天から曇天へ直す
                // 補正にだけ使う。t19/t21と同じ理由で、雲パスが走らないフレームでも常にバインドする
                cmd->SetTexture(22, m_Engine.m_SkyCloudFogTexture.get());
                // 低解像度で評価済みのDDGI(rgb=イラディアンス、a=insideWeight)。
                // 【無効時も常にバインドする】シェーダーはDDGIParams4.yで読むかどうかを分けるが、
                // DX12のディスクリプタテーブルは21スロットぶんをまとめてコピーするため、
                // 未初期化のスロットを残せない(反射プローブ・DDGIアトラスと同じ理由)。
                // 以前はここへ雲の3Dノイズを差していたが、Texture2Dの宣言と型が食い違うため
                // このテクスチャへ置き換えた
                cmd->SetTexture(19, m_Engine.m_DDGIResolveTexture.get());
                // 低解像度の深度(41.24節)。UpsampleDDGIがGatherRed 1回で4テクセルぶんを取る
                cmd->SetTexture(21, m_Engine.m_DDGIResolveDepthTexture.get());
                // 大気散乱のSkyView LUT。日中の空の色はここから引く
                cmd->SetTexture(20, m_Engine.m_SkyViewLUT.get());
                cmd->Draw(3, 0);
            },
        });

        // --- 半透明フォワードパス: glTFのalphaMode=BLENDのメッシュ(mesh.IsTransparent)だけを、
        //     LightingパスのSceneColorの上にカメラから遠い順(奥から手前)でアルファブレンド合成する。
        //     深度テストはGBuffer深度に対して行うが書き込みは行わない(半透明パイプラインステートの
        //     DepthWriteEnabled=false)ため、不透明物体には隠れる一方、半透明同士は常に描画順で
        //     正しく重なる。RenderTargets/DepthTargetにSceneColor/GBuffer深度を指定しているだけで
        //     ClearRenderTarget/ClearDepthは呼ばないため、Lightingパスが書いた内容の上に描き足す形になる ---
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Transparent",
            // ProbeBakeパスより後に順序付けさせるために挙げる(実際のバインドはExecute内)。
            // 半透明パスもLightingパスと同じ環境ソース(反射プローブ+グローバルIBL)を使うため、
            // 焼き上がる前のプローブを読まないようにする必要がある。
            // DDGIアトラスもReadsへ挙げ、DDGIProbeUpdateパスより後ろへ順序付ける
            .Reads = {
                m_Engine.m_ProbePrefilteredArray.get(), m_Engine.m_ProbeDistanceArray.get(),
                m_Engine.m_DDGIIrradianceAtlas.get(), m_Engine.m_DDGIDistanceAtlas.get(),
            },
            .RenderTargets = { m_Engine.m_RenderTargets.SceneColor.get() },
            .DepthTarget = m_Engine.m_RenderTargets.GBufferDepth.get(),
            .Execute = [this, gbufferViewport, &gpuLights, &cameraPosition, &viewProj, frameConstantBuffer, objectConstantBuffer, materialSamplers](RHI::IRHICommandList* cmd)
            {
                // 半透明メッシュをインスタンス単位でカメラからの距離降順(奥から手前)に並べる。
                // instance.WorldはHLSL(mul(vec, World))に合わせて転置済みのため、ワールド座標の
                // 平行移動成分は行ではなく列(_14/_24/_34)に入っている
                struct TransparentDraw
                {
                    const Assets::ModelInstance* Instance;
                    // 【段も覚える】meshが属する段のメッシュレット表を指す必要がある
                    const Assets::Model* Model;
                    const Assets::Mesh* Mesh;
                    float DistanceSq;
                };
                std::vector<TransparentDraw> draws;
                // 半透明もカメラの錐台で間引く。ここは描画リストの構築なので、
                // 間引いた分はソートの対象からも外れる。
                // 【このパスはクロスディザ非対応】なのでフェード中でも段は1つに決め打つ。
                // 【バッチは使わない】奥から手前へ並べ替える必要があり、まとめられない
                const Rendering::FrustumPlanes transparentFrustum = ExtractFrustumPlanes(viewProj);
                Rendering::GeometryDrawLoopDesc transparentLoop;
                transparentLoop.Frustum = &transparentFrustum;
                transparentLoop.UseDrawUnits = false;
                transparentLoop.LODMode = Rendering::GeometryLODMode::Current;
                transparentLoop.MeshFilter = Rendering::GeometryMeshFilter::Transparent;

                m_Engine.ForEachGeometryDraw(
                    transparentLoop,
                    [](const Rendering::InstanceDrawUnit&, const Assets::Model&, float) { return false; },
                    [&](const Rendering::InstanceDrawUnit& unit, const Assets::Model& currentModel,
                        const Assets::Mesh& mesh, float)
                    {
                        const Assets::ModelInstance& instance = *unit.Instance;
                        const float dx = instance.World._14 - cameraPosition.x;
                        const float dy = instance.World._24 - cameraPosition.y;
                        const float dz = instance.World._34 - cameraPosition.z;
                        const float distanceSq = dx * dx + dy * dy + dz * dz;
                        draws.push_back({ &instance, &currentModel, &mesh, distanceSq });
                        return true;
                    });
                if (draws.empty())
                {
                    return;
                }
                std::sort(
                    draws.begin(), draws.end(),
                    [](const TransparentDraw& a, const TransparentDraw& b) { return a.DistanceSq > b.DistanceSq; });

                cmd->SetViewport(gbufferViewport);
                cmd->SetPipelineState(m_Engine.m_TransparentPipelineState.get());
                cmd->SetConstantBuffer(0, frameConstantBuffer);
                cmd->SetSamplerSet(materialSamplers);

                // ライトバッファの中身の更新はグラフ構築前に1回だけ済ませてある
                // (タイルライトカリングパスがこのパスより先に読むため、パス内で更新できない)

                // メッシュによらずパス全体で共通のテクスチャはここで一度だけバインドする。
                // テクスチャのバインドは上書きするまで維持されるため(IRHICommandList::SetTexture参照)、
                // メッシュごとのループ内で張り直す必要はない
                cmd->SetTexture(4, m_Engine.m_RenderTargets.ShadowCascadeArray.get());
                cmd->SetShaderResourceBuffer(8, m_Engine.m_LightBuffer.get());
                // IBL(14章)。このパスにはSSRが適用されないため、半透明サーフェスの環境の
                // 映り込みはこの環境ソースだけが担う
                cmd->SetTexture(9, m_Engine.m_IrradianceTexture.get());
                cmd->SetTexture(10, m_Engine.m_PrefilteredEnvTexture.get());
                cmd->SetTexture(11, m_Engine.m_BRDFLUTTexture.get());
                // 反射プローブ(19章、鏡面専任)。Lightingパスと同じReflectionProbe.hlsliを
                // 共有しており、半透明サーフェスも室内なら室内の環境が映るようになる。
                // t0〜t4とt8〜t11が埋まっているため、このパスではt5・t7を割り当てている
                // (Transparent.hlsl冒頭。t6は使わない)。
                // マテリアルの遮蔽マップ(OcclusionTexture)はt5〜t7と衝突するためt13を使う
                // ProbeParams.xが0でも常にバインドするのはLightingパスと同じ理由
                cmd->SetTexture(5, m_Engine.m_ProbePrefilteredArray.get());
                cmd->SetShaderResourceBuffer(7, m_Engine.m_ProbeBuffer.get());
                cmd->SetTexture(12, m_Engine.m_ProbeDistanceArray.get());
                // DDGI(22章)。Lighting/ProbeCaptureパスと同じアトラスを共有する。
                // ProbeParams同様、DDGIParams0.wが0でも常にバインドする。
                // t14はメッシュごとのbent normal(34章)が使うためt15/t16へ置く
                // ——ここを14/15のままにするとメッシュのループが毎回上書きしてしまう
                cmd->SetTexture(15, m_Engine.m_DDGIIrradianceAtlas.get());
                cmd->SetTexture(16, m_Engine.m_DDGIDistanceAtlas.get());

                // 半透明は奥から手前への描画順そのものが正しさの前提なので並べ替えられない。
                // そのため必要になった時点でパイプラインを切り替える(GBufferパスと同じ方式)
                RHI::IRHIPipelineState* currentPipelineState = m_Engine.m_TransparentPipelineState.get();
                const auto bindPipelineState = [&](bool mirrored)
                {
                    RHI::IRHIPipelineState* const wanted =
                        mirrored ? m_Engine.m_TransparentPipelineStateMirrored.get() : m_Engine.m_TransparentPipelineState.get();
                    if (wanted == currentPipelineState)
                    {
                        return;
                    }
                    cmd->SetPipelineState(wanted);
                    cmd->SetConstantBuffer(0, frameConstantBuffer);
                    cmd->SetSamplerSet(materialSamplers);
                    currentPipelineState = wanted;
                };

                for (const TransparentDraw& draw : draws)
                {
                    bindPipelineState(draw.Instance->IsMirrored);

                    const ObjectConstants objectConstants =
                        MakeObjectConstants(
                            *draw.Instance, *draw.Model, *draw.Mesh, m_Engine.m_EmissiveLightSettings.Intensity,
                            m_Engine.m_AmbientOcclusionSettings.OcclusionMapEnabled, m_Engine.m_MeshletLODFrame);
                    cmd->UpdateBuffer(objectConstantBuffer, &objectConstants, sizeof(objectConstants));
                    cmd->SetConstantBuffer(1, objectConstantBuffer);

                    cmd->SetVertexBuffer(draw.Mesh->VertexBuffer.get());
                    cmd->SetIndexBuffer(draw.Mesh->IndexBuffer.get());
                    // メッシュごとに変わるマテリアルテクスチャのみ差し替える
                    // (t4のシャドウとt8以降のライト/IBLはループ前に一度バインドしたものがそのまま残る。
                    // t13だけはマテリアルの遮蔽マップなのでメッシュごとに差し替える)
                    cmd->SetTexture(0, draw.Mesh->BaseColorTexture);
                    cmd->SetTexture(1, draw.Mesh->NormalTexture);
                    cmd->SetTexture(2, draw.Mesh->MetallicRoughnessTexture);
                    cmd->SetTexture(3, draw.Mesh->EmissiveTexture);
                    cmd->SetTexture(13, draw.Mesh->OcclusionTexture);
                    // bent normal(34章)。このパスはt0〜t13を使い切っているためt14
                    cmd->SetTexture(14, draw.Mesh->BentNormalTexture);

                    cmd->DrawIndexed(draw.Mesh->IndexCount, 0, 0);
                }
            },
        });
    }
}
