#include "../KurenaiEngine3D.h"

#include <algorithm>

#include "Core/RenderGraph.h"
#include "PostProcessPasses.h"
#include "../Rendering/ExposureMath.h"
#include "../Rendering/RenderBlackboard.h"
#include "../Rendering/RenderFrameContext.h"

namespace Kurenai::Passes
{
    void PostProcessPasses::Register(
        Core::RenderGraph& graph,
        const Rendering::RenderFrameContext& frame,
        Rendering::RenderBlackboard& bb)
    {
        // 【フレームの値をここで写し取る】以下の登録コードはRender()から機械的に移した
        // ものなので、参照している名前を変えずに済むよう同じ名前のローカルへ受ける。
        //
        // 【写すのであって参照しないこと】Executeラムダはこの関数を抜けたあとに走る。
        // Render()では一部を参照捕捉していたが(gbufferViewport)、ここでは値でなければ浮く
        const RHI::Viewport gbufferViewport = frame.GBufferViewport;
        const DirectX::XMMATRIX viewMatrix = frame.ViewMatrix;
        const DirectX::XMMATRIX jitteredProj = frame.JitteredProj;
        const DirectX::XMMATRIX invViewProj = frame.InvViewProj;
        const DirectX::XMFLOAT2 jitterUv = frame.JitterUv;
        const float effectiveExposure = frame.EffectiveExposure;
        const float manualExposureScale = frame.ManualExposureScale;
        const float keyReferenceEV100 = frame.KeyReferenceEV100;
        const bool usingProceduralSky = frame.UsingProceduralSky;
        const bool fogPassRuns = frame.FogPassRuns;

        // --- 大気遠近パス: 反射パス(SSR/RT反射)の後、TAAパスの直前に置く。
        //     Lightingパスの中に入れない理由・TAAより前へ置く理由はShaders/3D/AerialPerspective.hlsl
        //     冒頭のコメント参照。無効時はパス自体を登録せず、reflectionOutputがそのまま
        //     TAA(またはTonemap)への入力になる ---
        RHI::IRHITexture* const reflectionOutput = m_Engine.GetActiveReflectionOutput();
        if (fogPassRuns)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "AerialPerspective",
                .Reads = { reflectionOutput, m_Engine.m_GBufferDepth.get(), m_Engine.m_SkyViewLUT.get() },
                .RenderTargets = { m_Engine.m_AerialPerspectiveTexture.get() },
                // 空パラメータ。SkyIntegrateパスの後へ順序付けさせるために挙げる
                // (実際のバインドはExecute内。SSRパスの同じ宣言と同じ理由)
                .BufferReads = { m_Engine.m_SkyParametersBuffer.get() },
                .Execute = [this, gbufferViewport, reflectionOutput](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(gbufferViewport);
                    cmd->SetPipelineState(m_Engine.m_AerialPerspectivePipelineState.get());
                    cmd->SetConstantBuffer(0, m_Engine.m_FrameConstantBuffer.get());
                    cmd->SetSamplerSet(m_Engine.m_ScreenSpaceSamplers.get());
                    cmd->SetTexture(0, reflectionOutput);
                    cmd->SetTexture(1, m_Engine.m_GBufferDepth.get());
                    cmd->SetShaderResourceBuffer(2, m_Engine.m_SkyParametersBuffer.get());
                    // 大気散乱のSkyView LUT。in-scatter項に背景と同じ空の色を
                    // 使うのがこのパスの要点なので、当然同じLUTを読む
                    cmd->SetTexture(3, m_Engine.m_SkyViewLUT.get());
                    cmd->Draw(3, 0);
                },
            });
        }

        // --- TAAパス: 前フレームのTAA結果をモーションベクターで再投影し、今フレームの色へ蓄積する。
        //     ジッターで散らしたサンプルがここで平均され、実質的なスーパーサンプリングになる。
        //     トーンマップ前のHDRの段階で行うのは、露出・ブルームがTAAで安定した絵を入力に
        //     できるようにするため(逆順にするとブルームがフレームごとのちらつきを拾う)。
        //     入力はGetActiveReflectionOutput()(反射Off/SSR/RT反射のいずれか、または大気遠近が
        //     有効ならその出力)で、SSRだけを見ていた従来の判定ではRT反射有効時にTAAが古い
        //     SceneColorを拾ってしまうため、ここも合わせて直す ---
        RHI::IRHITexture* const taaInputColor = fogPassRuns ? m_Engine.m_AerialPerspectiveTexture.get() : reflectionOutput;

        // --- ドローンショーパス: 夜空の機体を発光ビルボードとして加算合成で描く ---
        //
        // 【なぜここなのか(大気遠近より後・TAAより前)】
        //  ・大気遠近より後: 機体は深度を書かないため、先に描くとAerialPerspectiveが
        //    「背後の空の距離」で霞を掛けてしまい、光点が washout する
        //  ・TAAより前: ここに置くとRenderGraphがtaaInputColorのRead-after-Write依存で
        //    自動的にTAAの前へ順序付ける。機体がTAA・自動露出・ブルーム・トーンマップを
        //    一貫して通るため、シーンの他の発光物とまったく同じ扱いになる。
        //    TAAの後(=m_Engine.m_TAAHistoryへ直接加算)にしてはいけない ―― 履歴を汚し、
        //    次フレーム以降に尾を引く
        //
        // 書き込み先をtaaInputColorにしているのは、反射やフォグの有無でHDRシーン色の実体が
        // 移り変わるため。「今のHDRシーン色」を指す変数へ描くことでどの組み合わせでも成立する
        if (m_Engine.m_DroneShowEnabled && !m_Engine.m_DroneInstances.empty())
        {
            const uint32_t droneCount = static_cast<uint32_t>(m_Engine.m_DroneInstances.size());
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "DroneShow",
                .RenderTargets = { taaInputColor },
                // 島や地形の後ろに回った機体を隠すために深度テストを行う(書き込みはしない)
                .DepthTarget = m_Engine.m_GBufferDepth.get(),
                .BufferReads = { m_Engine.m_DroneBuffer.get() },
                .Execute = [this, gbufferViewport, viewMatrix, jitteredProj, effectiveExposure, droneCount](
                               RHI::IRHICommandList* cmd)
                {
                    DirectX::XMFLOAT4X4 projection;
                    DirectX::XMStoreFloat4x4(&projection, jitteredProj);

                    DroneShowConstants droneConstants{};
                    DirectX::XMStoreFloat4x4(&droneConstants.View, DirectX::XMMatrixTranspose(viewMatrix));
                    DirectX::XMStoreFloat4x4(&droneConstants.Proj, DirectX::XMMatrixTranspose(jitteredProj));
                    droneConstants.Params0 = {
                        // 実効プリ露出を掛ける。HDRバッファの中身はすべてプリ露出済みの値なので、
                        // ここで掛けないと機体だけが露出に追従しない浮いた明るさになる
                        m_Engine.m_DroneShow.Data().Brightness * effectiveExposure,
                        m_Engine.m_DroneShowMinScreenRadius,
                        // 射影行列の[0][0]。シェーダ側で最小画面サイズを世界半径へ逆算するのに使う
                        projection._11,
                        0.0f,
                    };
                    droneConstants.ClipPlane = { 0.0f, 1.0f, 0.0f, 0.0f };
                    // メイン描画ではクリップしない(平面反射パスだけが使う)
                    droneConstants.Params1 = { 0.0f, 0.0f, 0.0f, 0.0f };
                    cmd->UpdateBuffer(m_Engine.m_DroneShowConstantBuffer.get(), &droneConstants, sizeof(droneConstants));

                    cmd->SetViewport(gbufferViewport);
                    cmd->SetPipelineState(m_Engine.m_DroneShowPipelineState.get());
                    cmd->SetConstantBuffer(1, m_Engine.m_DroneShowConstantBuffer.get());
                    // 機体データは頂点シェーダーが読む。通常のSetShaderResourceBufferが使う
                    // SRVテーブルはピクセルシェーダーからしか見えないため専用の経路を使う
                    cmd->SetVertexShaderResourceBuffer(0, m_Engine.m_DroneBuffer.get());
                    // 1機につき2三角形。頂点バッファもインデックスバッファも要らない
                    cmd->Draw(droneCount * 6u, 0);
                },
            });
        }

        if (m_Engine.m_PostProcessSettings.TAAEnabled)
        {
            // 今フレームの書き込み先と、前フレームの結果(履歴)。Render()の末尾で役割が入れ替わる
            const uint32_t historyWriteIndex = m_Engine.m_TAAHistoryIndex;
            const uint32_t historyReadIndex = 1u - historyWriteIndex;
            RHI::IRHITexture* const historyTexture = m_Engine.m_TAAHistory[historyReadIndex].get();

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "TAA",
                // 履歴(historyTexture)は今フレーム誰も書かないので依存の辺は張られないが、
                // 実際にバインドするテクスチャはReadsにも宣言しておくというRenderGraphの規約に従う
                .Reads = { taaInputColor, historyTexture, m_Engine.m_GBufferVelocity.get(), m_Engine.m_GBufferDepth.get() },
                .RenderTargets = { m_Engine.m_TAAHistory[historyWriteIndex].get() },
                .Execute = [this, gbufferViewport, taaInputColor, historyTexture, invViewProj, jitterUv,
                            effectiveExposure](RHI::IRHICommandList* cmd)
                {
                    TAAConstants taaConstants{};
                    DirectX::XMStoreFloat4x4(&taaConstants.InvViewProj, DirectX::XMMatrixTranspose(invViewProj));
                    taaConstants.PrevViewProj = m_Engine.m_TAAPrevViewProjValid ? m_Engine.m_TAAPrevViewProj : DirectX::XMFLOAT4X4{};
                    taaConstants.JitterUv = { jitterUv.x, jitterUv.y, m_Engine.m_TAAPrevJitterUv.x, m_Engine.m_TAAPrevJitterUv.y };
                    taaConstants.ScreenParams = {
                        static_cast<float>(m_Engine.m_RenderWidth),
                        static_cast<float>(m_Engine.m_RenderHeight),
                        1.0f / static_cast<float>(m_Engine.m_RenderWidth),
                        1.0f / static_cast<float>(m_Engine.m_RenderHeight),
                    };

                    // 履歴が無効な間は「サンプルすらするな」をシェーダへ伝える(TAA.hlsl参照)。
                    // 作りたてのfp16バッファはNaNを含みうるため、混ぜる割合を0にするだけでは足りない
                    const bool historyValid = m_Engine.m_TAAHistoryValid.load(std::memory_order_relaxed);

                    // プリ露出はm_EffectiveExposureEV100の時間順応で毎フレーム変わる。履歴は前フレームの
                    // 露出で焼かれた明るさのままなので、比率を掛けて今の露出へ揃える。
                    // 揃えないと露出が動いている間ずっと明るさの尾を引く
                    const float previousExposure = ComputeExposure(m_Engine.m_TAAPrevEffectiveExposureEV100);
                    const float exposureRescale =
                        (historyValid && previousExposure > 0.0f) ? (effectiveExposure / previousExposure) : 1.0f;

                    taaConstants.Params0 = {
                        m_Engine.m_PostProcessSettings.TAABlendWeight,
                        m_Engine.m_PostProcessSettings.TAAClipGamma,
                        historyValid ? 1.0f : 0.0f,
                        exposureRescale,
                    };
                    taaConstants.Params1 = {
                        static_cast<float>(m_Engine.m_PostProcessSettings.TAAClip), m_Engine.m_PostProcessSettings.TAAAntiFlicker, 0.0f, 0.0f
                    };
                    cmd->UpdateBuffer(m_Engine.m_TAAConstantBuffer.get(), &taaConstants, sizeof(taaConstants));

                    cmd->SetViewport(gbufferViewport);
                    cmd->SetPipelineState(m_Engine.m_TAAPipelineState.get());
                    cmd->SetConstantBuffer(1, m_Engine.m_TAAConstantBuffer.get());
                    cmd->SetSamplerSet(m_Engine.m_ScreenSpaceSamplers.get());
                    // t0〜t3はすべて必ずバインドすること。SRVのバインドは上書きするまで維持されるため、
                    // 省くと直前のパスが張ったテクスチャを読んでしまう
                    cmd->SetTexture(0, taaInputColor);
                    cmd->SetTexture(1, historyTexture);
                    cmd->SetTexture(2, m_Engine.m_GBufferVelocity.get());
                    cmd->SetTexture(3, m_Engine.m_GBufferDepth.get());
                    cmd->Draw(3, 0);
                },
            });
        }

        // --- Tonemapパス: HDRのSceneColor(反射パス有効時はその出力、TAA有効時はさらにTAA適用後)を
        //     LDRへ変換する。反射等のHDR演算がすべて完了した後、Present直前の独立したステージとして
        //     常に実行する ---
        // この行はTAAパスのAddPassより後に置くこと。ラムダは値キャプチャなので、先に差し替えると
        // TAAが自分の出力を入力として読む形になる(RenderGraphが循環を検出して例外を投げる)
        RHI::IRHITexture* hdrSceneColor = m_Engine.m_PostProcessSettings.TAAEnabled ? m_Engine.m_TAAHistory[m_Engine.m_TAAHistoryIndex].get() : taaInputColor;
        // 【TAAパスの登録より後で確定させること】上のコメントの理由がそのまま効くため、
        // ブラックボードへ載せるのもこの位置にする
        bb.HdrSceneColor = hdrSceneColor;

        // --- 自動露出パス: SceneColorの輝度ヒストグラムから目標EV100を求め、時間方向に順応させる。
        //     結果はm_ExposureTextureへ書かれ、後段のTonemapパスが読む(AutoExposure.hlsl参照) ---
        if (m_Engine.m_PostProcessSettings.AutoExposureEnabled)
        {
            // シーン切り替え直後の1回だけ順応を飛ばす。パスを積んだ時点で消費しておくことで、
            // Executeが呼ばれる保証(グラフの枝刈り)に依存せず必ず1回で消える
            const bool resetAdaptation = m_Engine.m_AutoExposureResetRequested;
            m_Engine.m_AutoExposureResetRequested = false;

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "AutoExposure",
                .Reads = { hdrSceneColor, m_Engine.m_GBufferDepth.get() },
                .Writes = { m_Engine.m_ExposureTexture.get() },
                .Execute = [this, hdrSceneColor, keyReferenceEV100, usingProceduralSky, resetAdaptation](
                    RHI::IRHICommandList* cmd)
                {
                    AutoExposureConstants autoExposureConstants{};
                    autoExposureConstants.InputSize = { m_Engine.m_RenderWidth, m_Engine.m_RenderHeight };
                    // Min>Maxのような不正な範囲だとヒストグラムのビン割りが破綻するため順序を保証する
                    autoExposureConstants.MinEV100 = std::min(m_Engine.m_PostProcessSettings.AutoExposureMinEV100, m_Engine.m_PostProcessSettings.AutoExposureMaxEV100);
                    autoExposureConstants.MaxEV100 = std::max(m_Engine.m_PostProcessSettings.AutoExposureMinEV100, m_Engine.m_PostProcessSettings.AutoExposureMaxEV100);
                    autoExposureConstants.PreExposureEV100 = m_Engine.m_EffectiveExposureEV100;
                    // 一時停止やシーン読み込み直後の巨大なdtで順応が飛ばないよう上限を設ける
                    autoExposureConstants.DeltaTime = std::clamp(m_Engine.m_RenderDeltaTime, 0.0f, 0.1f);
                    autoExposureConstants.AdaptationSpeedUp = m_Engine.m_PostProcessSettings.AutoExposureSpeedUp;
                    autoExposureConstants.AdaptationSpeedDown = m_Engine.m_PostProcessSettings.AutoExposureSpeedDown;
                    autoExposureConstants.LowPercentile = std::min(m_Engine.m_PostProcessSettings.AutoExposureLowPercentile, m_Engine.m_PostProcessSettings.AutoExposureHighPercentile);
                    autoExposureConstants.HighPercentile = std::max(m_Engine.m_PostProcessSettings.AutoExposureLowPercentile, m_Engine.m_PostProcessSettings.AutoExposureHighPercentile);
                    autoExposureConstants.ExposureCompensation = m_Engine.m_PostProcessSettings.AutoExposureCompensation;
                    autoExposureConstants.NightRolloffEV = m_Engine.m_PostProcessSettings.AutoExposureNightRolloffEV;
                    // 折れ点は必ずDark < Brightにする(逆転すると補正が不連続になる)
                    autoExposureConstants.NightRolloffDarkEV100 =
                        std::min(m_Engine.m_PostProcessSettings.AutoExposureNightRolloffDarkEV100, m_Engine.m_PostProcessSettings.AutoExposureNightRolloffBrightEV100);
                    autoExposureConstants.NightRolloffBrightEV100 =
                        std::max(m_Engine.m_PostProcessSettings.AutoExposureNightRolloffDarkEV100, m_Engine.m_PostProcessSettings.AutoExposureNightRolloffBrightEV100);
                    // 構図に依存しないシーンの基準EV。測光値の上限の足がかりになる。
                    //
                    // **手続き空を使っていないシーンではクランプを無効にする**。
                    // 基準EVはこのエンジンの太陽・月・空モデルが出す照度から求めているので、
                    // .ksceneが独自のスカイボックスを指定しているシーン(White Furnace Testなど)
                    // では、そのシーンを実際に照らしている光と無関係な値になってしまう。
                    // 実際、無効化前はWhite Furnace Testの一様グレーが107から208まで持ち上がり、
                    // 白飛びまで余裕が無くなっていた(一様性そのものは保たれていたが、
                    // 飽和させてしまうとエネルギー保存の検証が成立しなくなる)
                    autoExposureConstants.KeyReferenceEV100 = keyReferenceEV100;
                    autoExposureConstants.KeyCeilingEV =
                        usingProceduralSky ? m_Engine.m_PostProcessSettings.AutoExposureKeyCeilingEV : 1.0e4f;
                    autoExposureConstants.ResetAdaptation = resetAdaptation ? 1.0f : 0.0f;
                    cmd->UpdateBuffer(m_Engine.m_AutoExposureConstantBuffer.get(), &autoExposureConstants, sizeof(autoExposureConstants));

                    // 1) ヒストグラムをゼロクリア
                    cmd->SetComputePipelineState(m_Engine.m_AutoExposureClearPipelineState.get());
                    cmd->SetComputeConstantBuffer(1, m_Engine.m_AutoExposureConstantBuffer.get());
                    cmd->SetComputeUnorderedAccessBuffer(0, m_Engine.m_ExposureHistogramBuffer.get());
                    cmd->Dispatch(1, 1, 1);

                    // 2) SceneColorから輝度ヒストグラムを構築
                    //    (UAVはDispatch直後に解除されるため毎回バインドし直す。IRHICommandList.h参照)
                    cmd->SetComputePipelineState(m_Engine.m_AutoExposureHistogramPipelineState.get());
                    cmd->SetComputeConstantBuffer(1, m_Engine.m_AutoExposureConstantBuffer.get());
                    cmd->SetComputeTexture(0, hdrSceneColor);
                    // 空(背景)を測光から外すために深度を読む(AutoExposure.hlsl参照)
                    cmd->SetComputeTexture(1, m_Engine.m_GBufferDepth.get());
                    cmd->SetComputeUnorderedAccessBuffer(0, m_Engine.m_ExposureHistogramBuffer.get());
                    cmd->Dispatch((m_Engine.m_RenderWidth + 15) / 16, (m_Engine.m_RenderHeight + 15) / 16, 1);

                    // 3) 縮約して目標EV100を求め、前フレームの値から指数的に順応させて書き戻す
                    cmd->SetComputePipelineState(m_Engine.m_AutoExposureResolvePipelineState.get());
                    cmd->SetComputeConstantBuffer(1, m_Engine.m_AutoExposureConstantBuffer.get());
                    cmd->SetComputeUnorderedAccessBuffer(0, m_Engine.m_ExposureHistogramBuffer.get());
                    cmd->SetComputeUnorderedAccessTexture(1, m_Engine.m_ExposureTexture.get());
                    cmd->Dispatch(1, 1, 1);
                },
            });
        }

        // --- ブルームパス: SceneColorから半解像度のピラミッドを作り、段階的にダウンサンプル→
        //     3x3テントでアップサンプルしながら加算する。最終段(m_Engine.m_BloomUpTextures[0])をTonemapが読む ---
        if (m_Engine.m_PostProcessSettings.BloomEnabled && !m_Engine.m_BloomDownTextures.empty())
        {
            std::vector<RHI::IRHITexture*> bloomWrites;
            bloomWrites.reserve(m_Engine.m_BloomDownTextures.size() + m_Engine.m_BloomUpTextures.size());
            for (const auto& texture : m_Engine.m_BloomDownTextures)
            {
                bloomWrites.push_back(texture.get());
            }
            for (const auto& texture : m_Engine.m_BloomUpTextures)
            {
                bloomWrites.push_back(texture.get());
            }

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "Bloom",
                .Reads = { hdrSceneColor, m_Engine.m_ExposureTexture.get() },
                .Writes = std::move(bloomWrites),
                .Execute = [this, hdrSceneColor, manualExposureScale](RHI::IRHICommandList* cmd)
                {
                    const uint32_t levelCount = static_cast<uint32_t>(m_Engine.m_BloomDownTextures.size());

                    BloomConstants bloomConstants{};
                    bloomConstants.Threshold = m_Engine.m_PostProcessSettings.BloomThreshold;
                    bloomConstants.SoftKnee = m_Engine.m_PostProcessSettings.BloomSoftKnee;
                    // しきい値を「表示上の白」基準の直感的な値のままにするため、
                    // ピラミッドの入力段で露出を反映する(Bloom.hlsl ExposureScale()参照)。
                    // Tonemapと同じ倍率でなければ、ブルームだけ露出がずれて合成比が狂う
                    bloomConstants.UseAutoExposure = m_Engine.m_PostProcessSettings.AutoExposureEnabled ? 1.0f : 0.0f;
                    bloomConstants.PreExposureEV100 = m_Engine.m_EffectiveExposureEV100;
                    bloomConstants.ExposureScale = manualExposureScale;

                    // --- ダウンサンプル: SceneColor -> down[0] -> down[1] -> ... ---
                    cmd->SetComputePipelineState(m_Engine.m_BloomDownsamplePipelineState.get());
                    cmd->SetComputeSamplerSet(m_Engine.m_ScreenSpaceSamplers.get());
                    for (uint32_t level = 0; level < levelCount; ++level)
                    {
                        const bool isFirst = (level == 0);
                        RHI::IRHITexture* source = isFirst ? hdrSceneColor : m_Engine.m_BloomDownTextures[level - 1].get();
                        const DirectX::XMUINT2 srcSize = isFirst
                            ? DirectX::XMUINT2{ m_Engine.m_RenderWidth, m_Engine.m_RenderHeight }
                            : m_Engine.m_BloomLevelSizes[level - 1];
                        const DirectX::XMUINT2 dstSize = m_Engine.m_BloomLevelSizes[level];

                        bloomConstants.SrcSize = srcSize;
                        bloomConstants.DstSize = dstSize;
                        // 最初のダウンサンプルだけKaris平均としきい値を適用する(理由はBloom.hlsl冒頭)
                        bloomConstants.ApplyKarisAndThreshold = isFirst ? 1.0f : 0.0f;
                        cmd->UpdateBuffer(m_Engine.m_BloomConstantBuffer.get(), &bloomConstants, sizeof(bloomConstants));

                        cmd->SetComputeConstantBuffer(1, m_Engine.m_BloomConstantBuffer.get());
                        cmd->SetComputeTexture(0, source);
                        cmd->SetComputeTexture(2, m_Engine.m_ExposureTexture.get());
                        // UAVはDispatch直後に解除されるため毎回バインドし直す(IRHICommandList.h参照)
                        cmd->SetComputeUnorderedAccessTexture(0, m_Engine.m_BloomDownTextures[level].get());
                        cmd->Dispatch((dstSize.x + 7) / 8, (dstSize.y + 7) / 8, 1);
                    }

                    // --- アップサンプル: 最下段から上へ、down[level] + tent(1段下) を up[level] へ書く ---
                    cmd->SetComputePipelineState(m_Engine.m_BloomUpsamplePipelineState.get());
                    cmd->SetComputeSamplerSet(m_Engine.m_ScreenSpaceSamplers.get());
                    for (int32_t level = static_cast<int32_t>(levelCount) - 2; level >= 0; --level)
                    {
                        // 最下段の1つ上だけは、まだup[]が書かれていないのでdown[]の最下段を読む
                        const bool readsDownChain = (level == static_cast<int32_t>(levelCount) - 2);
                        RHI::IRHITexture* lower = readsDownChain
                            ? m_Engine.m_BloomDownTextures[level + 1].get()
                            : m_Engine.m_BloomUpTextures[level + 1].get();

                        const DirectX::XMUINT2 srcSize = m_Engine.m_BloomLevelSizes[level + 1];
                        const DirectX::XMUINT2 dstSize = m_Engine.m_BloomLevelSizes[level];

                        bloomConstants.SrcSize = srcSize;
                        bloomConstants.DstSize = dstSize;
                        bloomConstants.ApplyKarisAndThreshold = 0.0f;
                        cmd->UpdateBuffer(m_Engine.m_BloomConstantBuffer.get(), &bloomConstants, sizeof(bloomConstants));

                        cmd->SetComputeConstantBuffer(1, m_Engine.m_BloomConstantBuffer.get());
                        cmd->SetComputeTexture(0, m_Engine.m_BloomDownTextures[level].get());
                        cmd->SetComputeTexture(1, lower);
                        cmd->SetComputeUnorderedAccessTexture(0, m_Engine.m_BloomUpTextures[level].get());
                        cmd->Dispatch((dstSize.x + 7) / 8, (dstSize.y + 7) / 8, 1);
                    }
                },
            });
        }

        // Tonemapがブルームとして読むテクスチャ。無効時も有効なテクスチャを常にt2へバインドする
        // 必要があるため、その場合はピラミッド最上段(内容は前フレームのまま)を渡し、
        // BloomStrength=0で寄与しないようにする
        RHI::IRHITexture* bloomResultTexture =
            m_Engine.m_BloomUpTextures.empty() ? hdrSceneColor : m_Engine.m_BloomUpTextures[0].get();

        // このフレームで超解像パスを走らせるか。デバッグ表示中は内部解像度の中間バッファを
        // そのまま等倍で見たいので走らせない(拡大するとバッファの実際の解像度が分からなくなる)
        const bool upscaleActive = m_Engine.IsUpscaleActive() && m_Engine.m_DebugViewSettings.View == DebugView::Final;
        bb.UpscaleActive = upscaleActive;

        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Tonemap",
            .Reads = { hdrSceneColor, m_Engine.m_ExposureTexture.get(), bloomResultTexture },
            .RenderTargets = { m_Engine.m_TonemapTexture.get() },
            .Execute = [this, gbufferViewport, hdrSceneColor, bloomResultTexture, manualExposureScale,
                        keyReferenceEV100, upscaleActive](RHI::IRHICommandList* cmd)
            {
                TonemapConstants tonemapConstants{};
                tonemapConstants.Curve = static_cast<int32_t>(m_Engine.m_PostProcessSettings.Curve);
                // 手動露出時: プリ露出は時刻連動で変動するので、設定EV100との差分を割り戻して
                // 「設定EV100で固定した絵」へ戻す(manualExposureScaleの算出箇所のコメント参照)
                tonemapConstants.ExposureScale = manualExposureScale;
                tonemapConstants.DitherStrength = m_Engine.m_PostProcessSettings.DitherEnabled ? 1.0f : 0.0f;
                tonemapConstants.UseAutoExposure = m_Engine.m_PostProcessSettings.AutoExposureEnabled ? 1.0f : 0.0f;
                tonemapConstants.PreExposureEV100 = m_Engine.m_EffectiveExposureEV100;
                tonemapConstants.BloomStrength =
                    (m_Engine.m_PostProcessSettings.BloomEnabled && !m_Engine.m_BloomUpTextures.empty()) ? m_Engine.m_PostProcessSettings.BloomStrength : 0.0f;
                tonemapConstants.MesopicStrength = m_Engine.m_PostProcessSettings.MesopicStrength;
                // 目の順応は画面の構図ではなくシーンの明るさで決まるので、
                // 自動露出の測光値ではなくキー照度から求めた基準EVを使う
                tonemapConstants.MesopicAdaptationEV100 = keyReferenceEV100;
                // シャープネスはTAAの蓄積で失われた高域を戻すためのものなので、TAAが無効なら0。
                // そうしないとTAA導入前の絵と変わってしまう。
                //
                // 【超解像が有効なときも0にする】ここのシャープネスは内部レンダー解像度で効く。
                // その後EASUで拡大すると、戻した高域もオーバーシュートの縁も一緒に引き伸ばされて
                // 太い縁取りになる。超解像時のシャープ化は出力解像度で効くRCASへ一本化し、
                // ここは素直なトーンマップ出力をEASUへ渡すことに徹する
                tonemapConstants.Sharpness = (m_Engine.m_PostProcessSettings.TAAEnabled && !upscaleActive) ? m_Engine.m_PostProcessSettings.TAASharpness : 0.0f;
                tonemapConstants.InvRenderWidth = 1.0f / static_cast<float>(m_Engine.m_RenderWidth);
                tonemapConstants.InvRenderHeight = 1.0f / static_cast<float>(m_Engine.m_RenderHeight);
                tonemapConstants.BlackPoint = m_Engine.m_PostProcessSettings.TonemapBlackPoint;
                cmd->UpdateBuffer(m_Engine.m_TonemapConstantBuffer.get(), &tonemapConstants, sizeof(tonemapConstants));

                cmd->SetViewport(gbufferViewport);
                cmd->SetPipelineState(m_Engine.m_TonemapPipelineState.get());
                cmd->SetConstantBuffer(1, m_Engine.m_TonemapConstantBuffer.get());
                cmd->SetSamplerSet(m_Engine.m_ScreenSpaceSamplers.get());
                cmd->SetTexture(0, hdrSceneColor);
                cmd->SetTexture(1, m_Engine.m_ExposureTexture.get());
                // t2は必ずバインドすること。SRVのバインドは上書きするまで維持されるため、
                // ここを省くと直前のパスが張ったテクスチャをブルームとして読んでしまう
                // (省くとG-Bufferのバッファを読んで画面全体が緑に転ぶ)
                cmd->SetTexture(2, bloomResultTexture);
                cmd->Draw(3, 0);
            },
        });

        // --- 超解像パス(41.23節): Tonemapが出したLDR画像を出力解像度へ再構成する ---
        //
        // EASU(拡大)とRCAS(シャープ化)を別パスにしているのは、RCASがEASUの結果の
        // 十字5タップを読むため。1つにまとめると同一リソースのSRV/UAV同時バインドになる。
        //
        // ここより後(Present)は出力解像度、ここより前はすべて内部レンダー解像度である。
        // ImGuiはRenderGraphの外でバックバッファへ直接描かれるため、この拡大の影響を受けない
        if (upscaleActive)
        {
            const uint32_t upscaleOutputWidth = m_Engine.m_UpscaleTargetWidth;
            const uint32_t upscaleOutputHeight = m_Engine.m_UpscaleTargetHeight;

            UpscaleConstants upscaleConstants{};
            ComputeEasuConstants(
                upscaleConstants, m_Engine.m_RenderWidth, m_Engine.m_RenderHeight, upscaleOutputWidth, upscaleOutputHeight);
            upscaleConstants.OutputSize = { upscaleOutputWidth, upscaleOutputHeight };
            upscaleConstants.RcasSharpnessScale = m_Engine.ComputeRcasSharpnessScale(m_Engine.m_PostProcessSettings.UpscaleSharpness);

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "UpscaleEASU",
                .Reads = { m_Engine.m_TonemapTexture.get() },
                .Writes = { m_Engine.m_UpscaleTexture.get() },
                .Execute = [this, upscaleConstants, upscaleOutputWidth,
                            upscaleOutputHeight](RHI::IRHICommandList* cmd)
                {
                    cmd->SetComputePipelineState(m_Engine.m_UpscaleEASUPipelineState.get());
                    // Gather4のアドレスモードがClampであることがEASUの前提(Upscale.hlslのコメント参照)
                    cmd->SetComputeSamplerSet(m_Engine.m_ScreenSpaceSamplers.get());
                    cmd->UpdateBuffer(m_Engine.m_UpscaleConstantBuffer.get(), &upscaleConstants, sizeof(upscaleConstants));
                    cmd->SetComputeConstantBuffer(1, m_Engine.m_UpscaleConstantBuffer.get());
                    cmd->SetComputeTexture(0, m_Engine.m_TonemapTexture.get());
                    // UAVはDispatch直後に解除されるため毎回バインドし直す
                    cmd->SetComputeUnorderedAccessTexture(0, m_Engine.m_UpscaleTexture.get());
                    cmd->Dispatch((upscaleOutputWidth + 7) / 8, (upscaleOutputHeight + 7) / 8, 1);
                },
            });

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "UpscaleRCAS",
                .Reads = { m_Engine.m_UpscaleTexture.get() },
                .Writes = { m_Engine.m_UpscaleSharpTexture.get() },
                .Execute = [this, upscaleConstants, upscaleOutputWidth,
                            upscaleOutputHeight](RHI::IRHICommandList* cmd)
                {
                    cmd->SetComputePipelineState(m_Engine.m_UpscaleRCASPipelineState.get());
                    // RCASはLoadで整数座標を引くのでサンプラーは使わないが、シェーダーが
                    // Samplers.hlsliを取り込んで宣言している以上バインドはしておく
                    cmd->SetComputeSamplerSet(m_Engine.m_ScreenSpaceSamplers.get());
                    cmd->UpdateBuffer(m_Engine.m_UpscaleConstantBuffer.get(), &upscaleConstants, sizeof(upscaleConstants));
                    cmd->SetComputeConstantBuffer(1, m_Engine.m_UpscaleConstantBuffer.get());
                    cmd->SetComputeTexture(0, m_Engine.m_UpscaleTexture.get());
                    cmd->SetComputeUnorderedAccessTexture(0, m_Engine.m_UpscaleSharpTexture.get());
                    cmd->Dispatch((upscaleOutputWidth + 7) / 8, (upscaleOutputHeight + 7) / 8, 1);
                },
            });
        }
    }
}
