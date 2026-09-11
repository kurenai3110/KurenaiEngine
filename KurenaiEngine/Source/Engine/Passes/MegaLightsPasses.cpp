
#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

#include "Core/Logger.h"
#include "Core/RenderGraph.h"
#include "Core/StringUtil.h"
#include "MegaLightsPasses.h"
#include "MegaLightsConstants.h"
#include "../Rendering/GPULight.h"
#include "../Rendering/RenderBlackboard.h"
#include "../Rendering/RenderFrameContext.h"
#include "../ShaderInterop/FrameConstants.h"
#include "../ShaderInterop/GroupSizes.h"
#include "../ShaderInterop/MegaLightsStochasticConstants.h"

namespace Kurenai::Passes
{
    namespace
    {
        using ShaderInterop::FrameConstants;
        using ShaderInterop::MegaLightsStochasticConstants;
    }

    void MegaLightsPasses::AdvanceHistory(bool temporalRan)
    {
        if (temporalRan)
        {
            m_MegaLightsHistoryIndex ^= 1u;
            // 【1フレーム走ってから有効にする】書いた側を次フレームが読むので、
            // 反転したあとに立てる。立てるのが早いと未初期化の内容を履歴として読む
            m_MegaLightsHistoryValid = true;
        }
        else
        {
            // 走らなかったフレームを挟むと履歴が途切れる(中身が古い or 未初期化)
            m_MegaLightsHistoryValid = false;
        }
    }

    void MegaLightsPasses::AdvanceDenoiseHistory(bool denoiseRan)
    {
        if (denoiseRan)
        {
            m_MegaLightsDenoiseHistoryIndex ^= 1u;
            m_MegaLightsDenoiseHistoryValid = true;
        }
        else
        {
            // 走らなかったフレームを挟むと履歴が途切れる(中身が古い)
            m_MegaLightsDenoiseHistoryValid = false;
        }
    }

    void MegaLightsPasses::ResetAccumulation()
    {
        // 解像度が変わると添字の意味が変わるので、蓄積も書き出しも必ず取り直す。
        // 【書き出し済みフラグも戻すこと】起動直後は既定解像度から実際のウィンドウサイズへ
        // 切り替わる。戻さないと、切り替わる前の低解像度のまま1回書き出して終わってしまう
        m_MegaLightsAccumFrames = 0;
        m_MegaLightsAccumWarmupFrames = 0;
        m_MegaLightsDumpIssued = false;
        m_MegaLightsDumpDone = false;
        m_MegaLightsDumpCopyFrame = 0;
        m_MegaLightsAccumReadback.reset();
    }

    void MegaLightsPasses::CreateLightCullingPipelineState(
        RHI::IRHIDevice& device, const std::wstring& shaderDirectory)
    {
        // タイルライトカリングパス(コンピュートシェーダー)。タイルごとに届くライトのインデックスリストを作る。
        // ライトグリッド本体(RenderTargets::LightTileBuffer)は解像度に依存するためCreateRenderTargetsで作る
        RHI::ShaderDesc lightCullingCsDesc;
        lightCullingCsDesc.Stage = RHI::ShaderStage::Compute;
        lightCullingCsDesc.FilePath = shaderDirectory + L"LightCulling.kshader";
        lightCullingCsDesc.EntryPoint = "CSMain";
        m_LightCullingComputeShader = device.CreateShader(lightCullingCsDesc);
        m_LightCullingPipelineState = device.CreateComputePipelineState({ m_LightCullingComputeShader.get() });

        RHI::BufferDesc lightCullingConstantBufferDesc;
        lightCullingConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        lightCullingConstantBufferDesc.SizeInBytes = sizeof(LightCullingConstants);
        m_LightCullingConstantBuffer = device.CreateBuffer(lightCullingConstantBufferDesc);
    }

    void MegaLightsPasses::CreateStochasticPipelineStates(
        RHI::IRHIDevice& device, const std::wstring& shaderDirectory)
    {
        // MegaLightsの参照実装(コンピュートシェーダー。ポイント/スポットライトを全灯
        // 総当たりし、届いた1灯ごとに光源までの影レイを撃つ)。以降の確率的サンプリングを
        // 評価するときの真値を作るためのパスで、RayQueryを含むためシェーダーモデル6.5が要る
        RHI::ShaderDesc megaLightsRefCsDesc;
        megaLightsRefCsDesc.Stage = RHI::ShaderStage::Compute;
        megaLightsRefCsDesc.FilePath = shaderDirectory + L"MegaLightsReference.kshader";
        megaLightsRefCsDesc.EntryPoint = "CSMain";
        m_MegaLightsReferenceComputeShader = device.CreateShader(megaLightsRefCsDesc);
        m_MegaLightsReferencePipelineState =
            device.CreateComputePipelineState({ m_MegaLightsReferenceComputeShader.get() });

        RHI::BufferDesc megaLightsConstantBufferDesc;
        megaLightsConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        megaLightsConstantBufferDesc.SizeInBytes = sizeof(MegaLightsConstants);
        m_MegaLightsConstantBuffer = device.CreateBuffer(megaLightsConstantBufferDesc);

        // MegaLightsの候補プール(コンピュートシェーダー。タイルごとに届くライトを走査して
        // 重みつきでK灯を抽出する)。レイを撃たないのでRayQueryは要らないが、
        // MegaLightsと同時にしか使わないためここで一緒に作る
        RHI::ShaderDesc megaLightsTilePoolCsDesc;
        megaLightsTilePoolCsDesc.Stage = RHI::ShaderStage::Compute;
        megaLightsTilePoolCsDesc.FilePath = shaderDirectory + L"MegaLightsTilePool.kshader";
        megaLightsTilePoolCsDesc.EntryPoint = "CSMain";
        m_MegaLightsTilePoolComputeShader = device.CreateShader(megaLightsTilePoolCsDesc);
        m_MegaLightsTilePoolPipelineState =
            device.CreateComputePipelineState({ m_MegaLightsTilePoolComputeShader.get() });

        RHI::BufferDesc megaLightsTilePoolConstantBufferDesc;
        megaLightsTilePoolConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        megaLightsTilePoolConstantBufferDesc.SizeInBytes = sizeof(MegaLightsTilePoolConstants);
        m_MegaLightsTilePoolConstantBuffer = device.CreateBuffer(megaLightsTilePoolConstantBufferDesc);

        // MegaLightsの確率的サンプリング本体(2パス)。
        // 【この4本はすべて RayQuery を含む】Initial は初期可視レイ、Temporal は
        // 時間検証レイ、Spatial は目標関数の可視性とバイアス補正レイ、Shade は影レイ。
        // したがってシェーダーモデル6.5が要る(パッカーの kSkipDxbc50Files を参照)。
        // レイを撃たないのは TilePool / Denoise / Accum / Resolve の4本だけで、
        // そちらは3バリアントすべてで焼かれる
        RHI::ShaderDesc megaLightsInitialCsDesc;
        megaLightsInitialCsDesc.Stage = RHI::ShaderStage::Compute;
        megaLightsInitialCsDesc.FilePath = shaderDirectory + L"MegaLightsInitialSample.kshader";
        megaLightsInitialCsDesc.EntryPoint = "CSMain";
        m_MegaLightsInitialComputeShader = device.CreateShader(megaLightsInitialCsDesc);
        m_MegaLightsInitialPipelineState =
            device.CreateComputePipelineState({ m_MegaLightsInitialComputeShader.get() });

        RHI::ShaderDesc megaLightsShadeCsDesc;
        megaLightsShadeCsDesc.Stage = RHI::ShaderStage::Compute;
        megaLightsShadeCsDesc.FilePath = shaderDirectory + L"MegaLightsShade.kshader";
        megaLightsShadeCsDesc.EntryPoint = "CSMain";
        m_MegaLightsShadeComputeShader = device.CreateShader(megaLightsShadeCsDesc);
        m_MegaLightsShadePipelineState =
            device.CreateComputePipelineState({ m_MegaLightsShadeComputeShader.get() });

        // クアッド共有(手法3)の解決パス。2x2の仲間が撃った標本を自分の面で評価し直して
        // 平均する。**レイを1本も撃たない**ので3バリアントすべてで焼ける
        // (パッカーの kSkipDxbc50Files には入れない)
        RHI::ShaderDesc megaLightsResolveCsDesc;
        megaLightsResolveCsDesc.Stage = RHI::ShaderStage::Compute;
        megaLightsResolveCsDesc.FilePath = shaderDirectory + L"MegaLightsResolve.kshader";
        megaLightsResolveCsDesc.EntryPoint = "CSMain";
        m_MegaLightsResolveComputeShader = device.CreateShader(megaLightsResolveCsDesc);
        m_MegaLightsResolvePipelineState =
            device.CreateComputePipelineState({ m_MegaLightsResolveComputeShader.get() });

        // 空間再利用。目標関数に可視性を入れるレイと、不偏化の分母のためのバイアス補正レイを撃つ
        RHI::ShaderDesc megaLightsSpatialCsDesc;
        megaLightsSpatialCsDesc.Stage = RHI::ShaderStage::Compute;
        megaLightsSpatialCsDesc.FilePath = shaderDirectory + L"MegaLightsSpatial.kshader";
        megaLightsSpatialCsDesc.EntryPoint = "CSMain";
        m_MegaLightsSpatialComputeShader = device.CreateShader(megaLightsSpatialCsDesc);
        m_MegaLightsSpatialPipelineState =
            device.CreateComputePipelineState({ m_MegaLightsSpatialComputeShader.get() });

        // 時間再利用。採用した履歴サンプルが今も見えるかを確かめる時間検証レイを1本撃つ
        RHI::ShaderDesc megaLightsTemporalCsDesc;
        megaLightsTemporalCsDesc.Stage = RHI::ShaderStage::Compute;
        megaLightsTemporalCsDesc.FilePath = shaderDirectory + L"MegaLightsTemporal.kshader";
        megaLightsTemporalCsDesc.EntryPoint = "CSMain";
        m_MegaLightsTemporalComputeShader = device.CreateShader(megaLightsTemporalCsDesc);
        m_MegaLightsTemporalPipelineState =
            device.CreateComputePipelineState({ m_MegaLightsTemporalComputeShader.get() });

        // デノイザ。3エントリ(時間累積 / à-trous / 復調戻し)を1ファイルに置く。
        // パッカーは1ファイル内の複数の[numthreads]を自動で見つける
        {
            RHI::ShaderDesc denoiseDesc;
            denoiseDesc.Stage = RHI::ShaderStage::Compute;
            denoiseDesc.FilePath = shaderDirectory + L"MegaLightsDenoise.kshader";
            denoiseDesc.EntryPoint = "CSTemporalAccum";
            m_MegaLightsDenoiseTemporalShader = device.CreateShader(denoiseDesc);
            m_MegaLightsDenoiseTemporalPSO =
                device.CreateComputePipelineState({ m_MegaLightsDenoiseTemporalShader.get() });
            denoiseDesc.EntryPoint = "CSAtrous";
            m_MegaLightsDenoiseAtrousShader = device.CreateShader(denoiseDesc);
            m_MegaLightsDenoiseAtrousPSO =
                device.CreateComputePipelineState({ m_MegaLightsDenoiseAtrousShader.get() });
            denoiseDesc.EntryPoint = "CSRemodulate";
            m_MegaLightsDenoiseRemodulateShader = device.CreateShader(denoiseDesc);
            m_MegaLightsDenoiseRemodulatePSO =
                device.CreateComputePipelineState({ m_MegaLightsDenoiseRemodulateShader.get() });

            RHI::BufferDesc denoiseCbDesc;
            denoiseCbDesc.Usage = RHI::BufferUsage::Constant;
            denoiseCbDesc.SizeInBytes = sizeof(MegaLightsDenoiseConstants);
            m_MegaLightsDenoiseConstantBuffer = device.CreateBuffer(denoiseCbDesc);
        }

        RHI::BufferDesc megaLightsStochasticConstantBufferDesc;
        megaLightsStochasticConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        megaLightsStochasticConstantBufferDesc.SizeInBytes = sizeof(MegaLightsStochasticConstants);
        m_MegaLightsStochasticConstantBuffer =
            device.CreateBuffer(megaLightsStochasticConstantBufferDesc);
        // 空間再利用の反復ごとに1本ずつ。中身は共有分と同じで反復番号だけが違う
        for (uint32_t spatialIteration = 0u; spatialIteration < Passes::kMegaLightsMaxSpatialIterations;
             ++spatialIteration)
        {
            m_MegaLightsSpatialConstantBuffer[spatialIteration] =
                device.CreateBuffer(megaLightsStochasticConstantBufferDesc);
        }

        // 蓄積平均(計測専用)。レイを撃たないがMegaLightsと同時にしか使わないのでここで作る
        RHI::ShaderDesc megaLightsAccumCsDesc;
        megaLightsAccumCsDesc.Stage = RHI::ShaderStage::Compute;
        megaLightsAccumCsDesc.FilePath = shaderDirectory + L"MegaLightsAccum.kshader";
        megaLightsAccumCsDesc.EntryPoint = "CSMain";
        m_MegaLightsAccumComputeShader = device.CreateShader(megaLightsAccumCsDesc);
        m_MegaLightsAccumPipelineState =
            device.CreateComputePipelineState({ m_MegaLightsAccumComputeShader.get() });

        RHI::BufferDesc megaLightsAccumConstantBufferDesc;
        megaLightsAccumConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        megaLightsAccumConstantBufferDesc.SizeInBytes = sizeof(MegaLightsAccumConstants);
        m_MegaLightsAccumConstantBuffer = device.CreateBuffer(megaLightsAccumConstantBufferDesc);
    }

    void MegaLightsPasses::Register(
        Core::RenderGraph& graph,
        const Rendering::RenderFrameContext& frame,
        Rendering::RenderBlackboard& bb)
    {
        // 【フレームの写しをローカルで受ける】frame自体はラムダへ捕捉しない
        const Rendering::RenderTargets* const targets = frame.Targets;

        // 【フレームの写しをローカルで受ける】frame自体はラムダへ捕捉しない
        RHI::IRHIBuffer* const lightBuffer = frame.Scene->LightBuffer.get();
        const Assets::RaytracingScene* const raytracingScene = &frame.Scene->RaytracingScene;

        // 【フレームの写しをローカルで受ける】frame自体はラムダへ捕捉しない
        RHI::IRHITexture* const brdfLUTTexture = frame.IBL->BRDFLUTTexture.get();

        // 【述語の結果はフレームの写しから引く】判定そのものは Should* が唯一の実装で、
        // ここで作り直さない。ラムダへ値で渡すためローカルで受ける
        const int32_t megaLightsSamplesPerPixel = frame.MegaLightsSamplesPerPixel;
        const bool lightCullingRuns = frame.LightCullingRuns;
        const bool megaLightsRuns = frame.MegaLightsRuns;

        // 【ラムダへ値で渡すためローカルへ受け直す】frame そのものは捕捉しない作法
        // (Rendering/RenderFrameContext.h の冒頭)。設定は POD なので写しは安い
        const EmissiveLightSettings emissiveLightSettings = frame.Settings.EmissiveLight;
        const MegaLightsSettings megaLightsSettings = frame.Settings.MegaLights;

        const uint32_t renderWidth = frame.RenderWidth;
        const uint32_t renderHeight = frame.RenderHeight;
        RHI::IRHIBuffer* const frameConstantBuffer = frame.FrameConstantBuffer;
        RHI::IRHISamplerSet* const screenSpaceSamplers = frame.ScreenSpaceSamplers;

        // 【フレームの値をここで写し取る】以下はRender()から機械的に移した登録コードなので、
        // 参照している名前を変えずに済むよう同じ名前で受け直す。
        //
        // gpuLights はポインタ越しに Render() のローカルを指しており、
        // graph.Execute() が終わるまで生きている(参照捕捉のままでよい)
        const std::vector<GPULight>& gpuLights = *frame.Lights;
        const DirectX::XMMATRIX viewMatrix = frame.ViewMatrix;
        const DirectX::XMMATRIX jitteredProj = frame.JitteredProj;
        const uint32_t megaLightsEffectiveTilesX = frame.MegaLightsEffectiveTilesX;
        const uint32_t megaLightsEffectiveTilesY = frame.MegaLightsEffectiveTilesY;
        const DirectX::XMUINT2 megaLightsTileOffset = frame.MegaLightsTileOffset;
        // 候補プールと確率的サンプリングの種。登録から実行までの間に進むことはないので値で持つ
        const uint32_t frameIndex = frame.FrameIndex;

        // --- タイルライトカリングパス: 画面を16x16のタイルに分け、タイルごとに「そのタイルに届くライト」の
        //     インデックスリストをコンピュートシェーダーで作る。直接光パスはそのリストだけをループする。
        //     BufferReads/BufferWritesを宣言しているのは、このパスと直接光パスがどちらもtargets->GBufferDepthを
        //     Readsするだけの「読み手同士」で、テクスチャの依存だけでは両者の間に順序が張られないため。
        //     **積むかどうかはShouldRunLightCullingが決める。** グリッドの読み手は直接光パスと
        //     Presentのデバッグ表示しか無く、MegaLightsが走るフレームは前者が止まっているため、
        //     通常表示では丸ごと無駄になる ---
        if (lightCullingRuns)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "LightCull",
                .Reads = { targets->GBufferDepth.get() },
                .BufferReads = { lightBuffer },
                .BufferWrites = { targets->LightTileBuffer.get() },
                .Execute = [this, targets, lightBuffer, &gpuLights, viewMatrix, jitteredProj, renderWidth, renderHeight](RHI::IRHICommandList* cmd)
                {
                    Passes::LightCullingConstants cullingConstants{};
                    DirectX::XMStoreFloat4x4(
                        &cullingConstants.View, DirectX::XMMatrixTranspose(viewMatrix));
                    cullingConstants.TileParams =
                    {
                        targets->LightTileCountX,
                        targets->LightTileCountY,
                        static_cast<uint32_t>(gpuLights.size()),
                        Passes::kLightTileCapacity,
                    };
                    cullingConstants.RenderSize = { renderWidth, renderHeight, 0u, 0u };

                    // タイル錐台の側面を組み立てるのに射影行列の(0,0)/(1,1)成分が要る。
                    // 深度リニアライズ定数(z/w)は直接光パスへ渡しているものと同じ。
                    // ここで読む_11/_22/_33/_43はいずれもTAAのジッター(_31/_32のみを書き換える)では
                    // 変化しないが、深度バッファを描いたときと同じ行列から導くという規約に揃えている
                    DirectX::XMFLOAT4X4 projection;
                    DirectX::XMStoreFloat4x4(&projection, jitteredProj);
                    cullingConstants.ProjParams =
                    {
                        projection._11,
                        projection._22,
                        projection._33,
                        projection._43,
                    };

                    cmd->UpdateBuffer(m_LightCullingConstantBuffer.get(), &cullingConstants, sizeof(cullingConstants));

                    cmd->SetComputePipelineState(m_LightCullingPipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_LightCullingConstantBuffer.get());
                    cmd->SetComputeShaderResourceBuffer(0, lightBuffer);
                    cmd->SetComputeTexture(1, targets->GBufferDepth.get());
                    cmd->SetComputeUnorderedAccessBuffer(0, targets->LightTileBuffer.get());
                    cmd->Dispatch(targets->LightTileCountX, targets->LightTileCountY, 1);
                },
            });
        }

        // --- MegaLightsの候補プールパス: タイルごとに「そこへ届くライト」を走査し、寄与に比例した
        //     確率でK灯を重みつきで抽出する。到達判定はタイルライトカリングと共有している
        //     (TileLightCulling.hlsli)ので、両者の「届いた灯数」は一致するはず。
        //     現段階では参照実装がこれを読まない(全灯を回す)ため、出力の消費者はまだいない ---
        if (megaLightsRuns && targets->MegaLightsTilePoolBuffer && m_MegaLightsTilePoolPipelineState)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "MegaLightsPool",
                .Reads = { targets->GBufferDepth.get() },
                .BufferReads = { lightBuffer },
                .BufferWrites = { targets->MegaLightsTilePoolBuffer.get() },
                .Execute = [this, targets, lightBuffer, megaLightsSettings, &gpuLights, viewMatrix, jitteredProj, megaLightsEffectiveTilesX, megaLightsEffectiveTilesY, megaLightsTileOffset, frameIndex, renderWidth, renderHeight](RHI::IRHICommandList* cmd)
                {
                    Passes::MegaLightsTilePoolConstants poolConstants{};
                    DirectX::XMStoreFloat4x4(&poolConstants.View, DirectX::XMMatrixTranspose(viewMatrix));
                    poolConstants.TileParams =
                    {
                        megaLightsEffectiveTilesX,
                        megaLightsEffectiveTilesY,
                        static_cast<uint32_t>(gpuLights.size()),
                        // 【書き手と読み手で必ず同じKを使うこと】プールの1タイルぶんの
                        // 要素数はKから決まるので、食い違うと別タイルの領域を読み書きする
                        static_cast<uint32_t>(megaLightsSettings.TilePoolCapacity),
                    };
                    poolConstants.RenderSize = { renderWidth, renderHeight, 0u, 0u };

                    // タイル錐台の組み立てと深度のリニアライズに使う。LightCullパスと同じ行列から
                    // 同じ成分を取る(判定を共有している以上、入力もずらしてはいけない)
                    DirectX::XMFLOAT4X4 projection;
                    DirectX::XMStoreFloat4x4(&projection, jitteredProj);
                    poolConstants.ProjParams =
                    {
                        projection._11,
                        projection._22,
                        projection._33,
                        projection._43,
                    };
                    // 候補を毎フレーム引き直すための種。TAAのフレーム番号を流用する
                    // (単調増加していればよく、ジッターの位相とは無関係)
                    poolConstants.PoolParams =
                    {
                        frameIndex,
                        megaLightsTileOffset.x,
                        megaLightsTileOffset.y,
                        0u,
                    };

                    cmd->UpdateBuffer(m_MegaLightsTilePoolConstantBuffer.get(), &poolConstants, sizeof(poolConstants));

                    cmd->SetComputePipelineState(m_MegaLightsTilePoolPipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_MegaLightsTilePoolConstantBuffer.get());
                    cmd->SetComputeShaderResourceBuffer(0, lightBuffer);
                    cmd->SetComputeTexture(1, targets->GBufferDepth.get());
                    // UAVはDispatch直後に解除されるため毎回バインドし直す
                    cmd->SetComputeUnorderedAccessBuffer(0, targets->MegaLightsTilePoolBuffer.get());
                    cmd->Dispatch(megaLightsEffectiveTilesX, megaLightsEffectiveTilesY, 1);
                },
            });
        }

        // --- MegaLightsパス: ポイント/スポットライトの直接光を専用パスで求め、HDRで書き出す。
        //     直後の直接光パスがt7でこれを読み、自分のライトループは回さない。
        //
        //     【必ず直接光パスより前に登録すること】RenderGraphの依存解決は登録順を1回だけ舐める
        //     前方走査で、Readsは自分より前に登録された書き手しか見つけられない。後ろに置くと
        //     辺が張られず、依存なし同士は登録順で実行されるため直接光パスが先に走り、
        //     **前フレームの残骸を読む**(初回は未初期化のfp16でNaNが伝播する) ---
        // 確率的サンプリングが t7 で読む候補プール。参照実装のフレームや、まだ確保していない
        // 環境では読まれないダミーとしてライトグリッドを張る(DX12はPSO切替でルート引数が
        // 無効化されるため、シェーダが宣言しているリソースは必ず何かをバインドする必要がある)
        RHI::IRHIBuffer* const tilePoolBufferForBinding =
            targets->MegaLightsTilePoolBuffer ? targets->MegaLightsTilePoolBuffer.get() : targets->LightTileBuffer.get();

        // 段階2のメッシュライトを、このフレームで三角形として積むかどうか。
        // 【フレーム単位の1変数に閉じること】画素やタイルごとに切り替えると境界で
        // 二重計上し、静止画では見えない。DX11/非DXR は ShouldRunMegaLights() が偽なので
        // 自動的に段階1のプロキシへ落ちる ―― 分岐を追加で書かない
        const bool meshLightsActive = m_Engine.IsMeshLightsEnabled() && m_Engine.GetMeshLightScene().IsValid();
        const uint32_t meshLightTriangleCount =
            meshLightsActive ? m_Engine.GetMeshLightScene().GetTriangleCount() : 0u;
        // t8 に張る三角形テーブル。無効なフレームでも何かを張る必要がある
        // (DX12はPSO切替でルート引数が無効化されるため)。読まれないダミーとして
        // ライトリストを張る ―― 三角形数0なのでループが1周も回らない
        RHI::IRHIBuffer* const meshLightBufferForBinding =
            meshLightsActive ? m_Engine.GetMeshLightScene().GetTriangleBuffer() : lightBuffer;

        if (megaLightsRuns && frame.Settings.MegaLights.Mode == MegaLightsMode::Reference)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "MegaLights",
                .Reads =
                {
                    targets->GBufferAlbedo.get(), targets->GBufferNormal.get(), targets->GBufferMaterial.get(), targets->GBufferDepth.get(),
                    // スペキュラのエネルギー補正でEssを引く。Readsへ挙げることでBRDFLUTBakeパス
                    // (このLUTの書き手)より後ろに順序付けられる
                    brdfLUTTexture,
                },
                .Writes = { targets->MegaLightsTexture.get() },
                // ライトリストはグラフの外(UpdateBuffer)で更新済みだが、読むものは宣言しておく
                // というこのコードベースの規約に従う。候補プールは確率的サンプリングのときだけ
                // 読むが、宣言しておくことで候補プールパスより後ろへ順序付けられる
                // (参照実装のフレームでは辺が1本余分に張られるだけで無害)
                .BufferReads = { lightBuffer, tilePoolBufferForBinding, meshLightBufferForBinding },
                .Execute = [this, targets, lightBuffer, raytracingScene, brdfLUTTexture, emissiveLightSettings, megaLightsSettings, &gpuLights, tilePoolBufferForBinding, meshLightBufferForBinding, meshLightTriangleCount, frameIndex, renderWidth, renderHeight, frameConstantBuffer, screenSpaceSamplers](RHI::IRHICommandList* cmd)
                {
                    Passes::MegaLightsConstants megaLightsConstants{};
                    megaLightsConstants.Params0 =
                    {
                        renderWidth,
                        renderHeight,
                        static_cast<uint32_t>(std::max(0, megaLightsSettings.ShadowRayCount)),
                        static_cast<uint32_t>(gpuLights.size()),
                    };
                    // 球光源のサンプル列を毎フレーム回す種。確率的サンプリング側と同じ
                    // フレーム番号を使う(あちらは Params1.w)
                    megaLightsConstants.Params1 = { frameIndex, meshLightTriangleCount, 0u, 0u };
                    // 段階1が MakeGPULightFromEmissiveProxy で毎フレーム掛けているのと同じ倍率。
                    // これで ImGui の「自発光の強度」がメッシュライトにもライブに効く
                    // y は影響半径の伸縮。半径は倍率1で焼いてあり、段階1の Range は
                    // peak ∝ intensity から解かれるので R ∝ sqrt(intensity) で伸ばす
                    megaLightsConstants.Params2 = {
                        emissiveLightSettings.Intensity, std::sqrt(std::max(emissiveLightSettings.Intensity, 0.0f)),
                        0.0f, 0.0f };
                    cmd->UpdateBuffer(m_MegaLightsConstantBuffer.get(), &megaLightsConstants,
                                      sizeof(megaLightsConstants));

                    cmd->SetComputePipelineState(m_MegaLightsReferencePipelineState.get());
                    cmd->SetComputeConstantBuffer(0, frameConstantBuffer);
                    cmd->SetComputeConstantBuffer(1, m_MegaLightsConstantBuffer.get());

                    // BRDF積分LUTをColorSampler(s1、Linear+Clamp)で引くためサンプラーを張る。
                    // 直前のパスのバインドが残っていることに依存しない
                    cmd->SetComputeSamplerSet(screenSpaceSamplers);

                    // レジスタ割り当てはMegaLightsReference.hlsl側の宣言と一致させること
                    cmd->SetComputeAccelerationStructure(0, raytracingScene->GetTopLevelAS());
                    cmd->SetComputeTexture(1, targets->GBufferNormal.get());
                    cmd->SetComputeTexture(2, targets->GBufferDepth.get());
                    cmd->SetComputeTexture(3, targets->GBufferAlbedo.get());
                    cmd->SetComputeTexture(4, targets->GBufferMaterial.get());
                    cmd->SetComputeTexture(5, brdfLUTTexture);
                    // ライトが0灯のフレームでも必ずバインドする(DX12はSetPipelineStateのたびに
                    // ルート引数が無効化されるため、シェーダが宣言しているリソースを未バインドで
                    // Dispatchすることになる)
                    cmd->SetComputeShaderResourceBuffer(6, lightBuffer);
                    // 候補プール。参照実装は宣言していないが、DX12はPSO切替でルート引数が
                    // 無効化されるため、確率的サンプリングのときは必ずバインドする必要がある。
                    // 常に張っても害は無いので分岐させない
                    cmd->SetComputeShaderResourceBuffer(7, tilePoolBufferForBinding);
                    // 発光三角形テーブル。三角形数0のフレームでもダミーを張る(同上)
                    cmd->SetComputeShaderResourceBuffer(8, meshLightBufferForBinding);

                    // UAVはDispatch直後に解除されるため毎回バインドし直す(IRHICommandList.h参照)
                    cmd->SetComputeUnorderedAccessTexture(0, targets->MegaLightsTexture.get());
                    cmd->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);
                },
            });
        }

        // --- 確率的サンプリングは2パスに分かれる ---
        //   MegaLightsInitial : 候補プールからRISで1灯へ絞り、**リザーバ**として書く(色は作らない)
        //   MegaLightsShade   : そのリザーバへ影レイを1本撃ち、選ばれた確率で割り戻してHDRを書く
        //
        // 【なぜ1パスにまとめないのか】時間・空間の再利用は「どの灯を選んだか」を持ち回って
        // 現フレームで評価し直す形でしか書けない。選択とシェードが混ざっていると再利用の段を
        // 差し込む場所が無い。分けておけば両者の間に挟むだけで済む
        //
        // 【クアッド共有(手法3)は Initial をそのまま共有する】違うのは後段だけで、
        // 時間・空間再利用を挟まずに Resolve が2x2の4標本を平均する。
        // Initial を共有していることが陽性対照の土台になる ―― 共有を切った手法3は、
        // 手法2から再利用を外した構成と画素単位で一致するはず
        const bool megaLightsQuadShared =
            megaLightsRuns && frame.Settings.MegaLights.Mode == MegaLightsMode::QuadShared;
        if (megaLightsRuns &&
            (frame.Settings.MegaLights.Mode == MegaLightsMode::Stochastic || megaLightsQuadShared))
        {
            // 2パスで同じ定数バッファを共有する。中身はグラフ構築のこの時点で確定しているので、
            // Initial側のExecuteで1回だけ更新すればよい
            const auto buildStochasticConstants =
                [this, megaLightsSamplesPerPixel, megaLightsSettings, jitteredProj, megaLightsQuadShared, megaLightsEffectiveTilesX,
                 megaLightsTileOffset, frameIndex, renderWidth, renderHeight](uint32_t spatialIteration)
            {
                MegaLightsStochasticConstants stochasticConstants{};
                stochasticConstants.Params0 =
                {
                    renderWidth,
                    renderHeight,
                    static_cast<uint32_t>(std::max(1, megaLightsSettings.SampleCount)),
                    // 影レイ本数の意味は参照実装と揃える(0なら影を撃たない=恒等テスト側)。
                    // 確率的サンプリングは選ばれた1灯にしか撃たないので本数ではなく有無
                    (megaLightsSettings.ShadowRayCount > 0) ? 1u : 0u,
                };
                stochasticConstants.Params1 =
                {
                    megaLightsEffectiveTilesX,
                    Passes::kLightTileSize,
                    // 候補プールを書いたときと同じKでなければならない(上のTileParams.wと同値)
                    static_cast<uint32_t>(megaLightsSettings.TilePoolCapacity),
                    frameIndex,
                };
                stochasticConstants.Params2 =
                {
                    static_cast<uint32_t>(std::max(0, megaLightsSettings.SpatialNeighborCount)),
                    static_cast<uint32_t>(std::max(1, megaLightsSettings.SpatialRadius)),
                    megaLightsSettings.SpatialMIS ? 1u : 0u,
                    // 初期可視レイでリザーバを殺すか(Initialが読む)。殺すと影の縁に
                    // 暗い側の系統誤差が残るため、切り替えて測れるようにしてある。
                    // 【手法3では必ず撃つ】クアッド共有は「Initialが撃った1本」だけを
                    // 可視性の情報源にしている。切ると全標本が可視フラグ付きで出てきて
                    // 影が1つも出ない(絵が明るいだけで例外もログも出ない)
                    (megaLightsQuadShared || megaLightsSettings.InitialVisibility) ? 1u : 0u,
                };
                // 候補プールが錐台を組み立てたのと**同じ行列**から取る。ずれると
                // 「その灯が隣のタイルへ届くか」の判定が候補プールと食い違い、定義域がずれる
                {
                    DirectX::XMFLOAT4X4 projection;
                    DirectX::XMStoreFloat4x4(&projection, jitteredProj);

                    // 【プリ露出の補正は入れない ―― TAA/DDGIから写してはいけない】
                    // あちらが補正するのは履歴の*色*で、色はプリ露出に比例するから
                    // 「今の露出 / 前の露出」を掛ける必要がある。こちらが持ち回るのは
                    // リザーバのWで、W = Σw / (M * p̂)、w = p̂ / p_source。
                    // 分子も分母も p̂ に比例し、p_source は正規化された確率なので露出に
                    // 依存しない。**露出が約分されるのでWは露出に対して不変**。
                    //
                    // 【両方向を実測して確かめた(-megalightsperturb 2)】
                    // 「今/前」を掛けると露出+2段の直後に4倍暗くなり、「前/今」を掛けると
                    // 4倍明るいまま居座る。掛けないときだけ履歴なしの経路と一致する。
                    // **静止画では絶対に気付けない誤り**だった
                    stochasticConstants.Params3 = {
                        projection._11, projection._22,
                        // z は未使用(かつて露出補正を入れていた枠。上のコメント参照)
                        0.0f,
                        static_cast<float>(std::max(1, megaLightsSettings.TemporalMClamp)),
                    };
                }
                // 履歴が使えるか。解像度が変わった直後は添字の意味が変わっており、
                // バッファのクリアが無いRHIでは前の内容が別画素のものとして残っている
                // y は空間再利用の反復番号。近傍の型板の種に混ぜて、反復ごとに別の近傍を選ばせる
                // z/w はクアッド共有(手法3)。z は Resolve が、w は Initial が読む。
                //
                // 【手法3の Params4.x の意味は手法2と違う】手法2では「時間再利用が履歴
                // リザーバを読んでよいか」だが、手法3に時間再利用は無く、Initial が
                // 遮蔽の確定した灯のキャッシュを信用してよいかの判定にだけ使う。
                // **陽性対照では切る**(履歴に依存すると手法2との画素単位の一致が崩れる)
                const bool historyUsable = megaLightsQuadShared
                                               ? (m_MegaLightsHistoryValid && megaLightsSettings.BlockedCacheEnabled)
                                               : m_MegaLightsHistoryValid;
                stochasticConstants.Params4 = {
                    historyUsable ? 1u : 0u,
                    spatialIteration,
                    (megaLightsQuadShared && megaLightsSettings.QuadShareEnabled) ? 1u : 0u,
                    (megaLightsQuadShared && megaLightsSettings.QuadStratify) ? 1u : 0u,
                };
                // 1画素あたりの標本数。**リザーババッファの確保と必ず同じ値にすること** ――
                // ずれると Initial が確保外へ書くか、Resolve が別画素の標本を読む
                // (どちらも例外にならず、絵が「それらしく」出るので気付けない)
                stochasticConstants.Params5 = {
                    static_cast<uint32_t>(megaLightsSamplesPerPixel), 0u, 0u, 0u
                };
                // 候補プールを書いたときと同じ格子オフセット。末尾へ足して、途中までしか
                // 宣言しない Shade / Temporal / Resolve の既存レイアウトを変えない
                stochasticConstants.Params6 = {
                    megaLightsTileOffset.x, megaLightsTileOffset.y, 0u, 0u
                };
                return stochasticConstants;
            };
            const auto updateStochasticConstants = [this, buildStochasticConstants](RHI::IRHICommandList* cmd)
            {
                const MegaLightsStochasticConstants stochasticConstants = buildStochasticConstants(0u);
                cmd->UpdateBuffer(
                    m_MegaLightsStochasticConstantBuffer.get(), &stochasticConstants, sizeof(stochasticConstants));
            };

            // --- 再利用の連鎖: Initial → (Temporal) → (Spatial) → Shade ---
            // どちらの再利用も任意に切れるので、シェードが読むリザーバは「最後に書いた者」になる。
            //
            // 【Temporalの出力がそのまま次フレームの履歴になる】Spatialの結果は履歴へ戻さない。
            // 戻すと、空間で混ぜたものを時間でまた混ぜることになり、近傍どうしの相関が
            // フレームをまたいで積み上がる(ノイズが塊で蠢く)。RTXDI系には戻す実装もあるが、
            // まず戻さない形で入れて、必要になったら測ってから変える
            // 【手法3は再利用の段をどちらも通さない】リザーバを持ち回らないのが手法3の要点で、
            // 追加のレイ(可視レイ・時間検証レイ・不偏化の分母のための補正レイ)が
            // ここから生まれている。1画素1レイという予算はこれを外して初めて成り立つ
            const bool temporalRuns = !megaLightsQuadShared && frame.Settings.MegaLights.TemporalEnabled &&
                                      m_MegaLightsTemporalPipelineState &&
                                      targets->MegaLightsReservoirHistory[0] && targets->MegaLightsHistoryGuide[0];
            const bool spatialRuns = !megaLightsQuadShared && frame.Settings.MegaLights.SpatialEnabled &&
                                     m_MegaLightsSpatialPipelineState &&
                                     targets->MegaLightsReservoirSpatialBuffer &&
                                     targets->MegaLightsReservoirSpatialBuffer2 && frame.Settings.MegaLights.SpatialNeighborCount > 0;
            // 反復回数。ping-pongのバッファと定数バッファの本数で上限が決まる。
            // 【時間再利用を切っているときは1回に落とす】不偏化の分母(Z)の可視性込みの
            // 判定は「生きているリザーバはこのフレーム・この画素で可視」という不変条件に
            // 依っており、それを保っているのは時間検証レイである。時間再利用を切ると
            // 検証が無くなり、2回目の反復が未検証のサンプルを重ねて数えるため
            // **明るい側へ大きく偏る**(900枚の蓄積平均で参照実装比 +22.4%。1回なら
            // -0.07% なので反復を重ねたときにだけ出る)。時間再利用があれば不偏
            //(同じ測定で +0.0% / 誤差の中央値は 0.0379 → 0.0305 と改善)
            uint32_t spatialIterations =
                spatialRuns ? static_cast<uint32_t>(std::clamp(
                                  frame.Settings.MegaLights.SpatialIterations, 1,
                                  static_cast<int32_t>(Passes::kMegaLightsMaxSpatialIterations)))
                            : 0u;
            if (!temporalRuns && spatialIterations > 1u)
            {
                spatialIterations = 1u;
            }
            // 最後の反復が書いた側をシェードが読む
            RHI::IRHIBuffer* const spatialPingPong[Passes::kMegaLightsMaxSpatialIterations] = {
                targets->MegaLightsReservoirSpatialBuffer.get(), targets->MegaLightsReservoirSpatialBuffer2.get()
            };

            // ping-pong。今フレームが書く側と、前フレームが書いた側
            const uint32_t historyWriteIndex = m_MegaLightsHistoryIndex;
            const uint32_t historyReadIndex = m_MegaLightsHistoryIndex ^ 1u;

            // 【履歴は時間再利用の出力に取る ―― 空間再利用の出力を履歴へ戻してはいけない】
            // 一度、計画(1-3節)どおり「時間→空間の結果を履歴にする」形へ変えたところ、
            // 発散振動した(隣接フレーム差が63階調。実測)。近傍の履歴に自分の過去の
            // サンプルが混ざる正帰還ループができ、Wが往復のたびに複利で増幅されるため。
            // 空間再利用はフレーム内で完結させ、履歴には時間再利用の出力だけを入れる
            RHI::IRHIBuffer* const temporalOutputBuffer =
                temporalRuns ? targets->MegaLightsReservoirHistory[historyWriteIndex].get() : nullptr;
            // 空間再利用の入力 = 時間再利用を挟んだならその出力、挟まないならInitialの出力
            RHI::IRHIBuffer* const reuseInputBuffer =
                temporalRuns ? temporalOutputBuffer : targets->MegaLightsReservoirBuffer.get();
            RHI::IRHIBuffer* const shadeReservoirBuffer =
                spatialRuns ? spatialPingPong[(spatialIterations - 1u) % Passes::kMegaLightsMaxSpatialIterations]
                            : reuseInputBuffer;

            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "MegaLightsInitial",
                .Reads =
                {
                    targets->GBufferAlbedo.get(), targets->GBufferNormal.get(), targets->GBufferMaterial.get(), targets->GBufferDepth.get(),
                    brdfLUTTexture,
                },
                .BufferReads = { lightBuffer, tilePoolBufferForBinding },
                .BufferWrites = { targets->MegaLightsReservoirBuffer.get(), targets->MegaLightsBlockedLightBuffer.get() },
                .Execute = [this, targets, lightBuffer, raytracingScene, brdfLUTTexture, tilePoolBufferForBinding, updateStochasticConstants, renderWidth, renderHeight, frameConstantBuffer, screenSpaceSamplers](RHI::IRHICommandList* cmd)
                {
                    updateStochasticConstants(cmd);

                    cmd->SetComputePipelineState(m_MegaLightsInitialPipelineState.get());
                    cmd->SetComputeConstantBuffer(0, frameConstantBuffer);
                    cmd->SetComputeConstantBuffer(1, m_MegaLightsStochasticConstantBuffer.get());
                    cmd->SetComputeSamplerSet(screenSpaceSamplers);

                    // レジスタ割り当てはMegaLightsInitialSample.hlsl側の宣言と一致させること。
                    // 初期可視レイ(選んだサンプルが遮蔽されていたら殺す)を撃つのでTLASが要る
                    cmd->SetComputeAccelerationStructure(0, raytracingScene->GetTopLevelAS());
                    cmd->SetComputeTexture(1, targets->GBufferNormal.get());
                    cmd->SetComputeTexture(2, targets->GBufferDepth.get());
                    cmd->SetComputeTexture(3, targets->GBufferAlbedo.get());
                    cmd->SetComputeTexture(4, targets->GBufferMaterial.get());
                    cmd->SetComputeTexture(5, brdfLUTTexture);
                    cmd->SetComputeShaderResourceBuffer(6, lightBuffer);
                    cmd->SetComputeShaderResourceBuffer(7, tilePoolBufferForBinding);

                    cmd->SetComputeUnorderedAccessBuffer(0, targets->MegaLightsReservoirBuffer.get());
                    cmd->SetComputeUnorderedAccessBuffer(1, targets->MegaLightsBlockedLightBuffer.get());
                    cmd->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);
                },
            });

            // --- 時間再利用: 前フレームの自分が選んだ灯を再投影して借りる ---
            // 実効サンプル数がフレーム方向に積み上がるので収束が速くなる。
            // レイは1本だけ増える(採用した履歴サンプルが今も見えるかを確かめる時間検証レイ)
            if (temporalRuns)
            {
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "MegaLightsTemporal",
                    .Reads =
                    {
                        targets->GBufferAlbedo.get(), targets->GBufferNormal.get(), targets->GBufferMaterial.get(), targets->GBufferDepth.get(),
                        brdfLUTTexture, targets->GBufferVelocity.get(),
                    },
                    // 【読むのは前フレームが書いた側】今フレームが書くのはもう片方なので、
                    // 同じバッファへの読み書きが同一フレーム内で起きない(WARが生じない)。
                    // RenderGraphはWARの辺を張らないので、これは構造で守るしかない
                    .BufferReads = { lightBuffer, targets->MegaLightsReservoirBuffer.get(),
                                     targets->MegaLightsReservoirHistory[historyReadIndex].get(),
                                     targets->MegaLightsHistoryGuide[historyReadIndex].get() },
                    .BufferWrites = { targets->MegaLightsReservoirHistory[historyWriteIndex].get(),
                                      targets->MegaLightsHistoryGuide[historyWriteIndex].get() },
                    .Execute = [this, targets, lightBuffer, raytracingScene, brdfLUTTexture, historyReadIndex, historyWriteIndex, renderWidth, renderHeight, frameConstantBuffer, screenSpaceSamplers](RHI::IRHICommandList* cmd)
                    {
                        // 定数はInitial側で更新済み(中身はフレーム内で不変)
                        cmd->SetComputePipelineState(m_MegaLightsTemporalPipelineState.get());
                        cmd->SetComputeConstantBuffer(0, frameConstantBuffer);
                        cmd->SetComputeConstantBuffer(1, m_MegaLightsStochasticConstantBuffer.get());
                        cmd->SetComputeSamplerSet(screenSpaceSamplers);

                        // レジスタ割り当てはMegaLightsTemporal.hlsl側の宣言と一致させること。
                        // 時間検証レイを撃つのでTLASが要る。
                        // 【使わないフレームでも必ずバインドする】DX12は宣言された
                        // リソースが未バインドだと壊れる
                        cmd->SetComputeAccelerationStructure(0, raytracingScene->GetTopLevelAS());
                        cmd->SetComputeTexture(1, targets->GBufferNormal.get());
                        cmd->SetComputeTexture(2, targets->GBufferDepth.get());
                        cmd->SetComputeTexture(3, targets->GBufferAlbedo.get());
                        cmd->SetComputeTexture(4, targets->GBufferMaterial.get());
                        cmd->SetComputeTexture(5, brdfLUTTexture);
                        cmd->SetComputeShaderResourceBuffer(6, lightBuffer);
                        cmd->SetComputeShaderResourceBuffer(7, targets->MegaLightsReservoirBuffer.get());
                        cmd->SetComputeShaderResourceBuffer(
                            8, targets->MegaLightsReservoirHistory[historyReadIndex].get());
                        cmd->SetComputeShaderResourceBuffer(9, targets->MegaLightsHistoryGuide[historyReadIndex].get());
                        // 再投影はTAAとまったく同じ引き方をする(historyUv = uv - velocity)
                        cmd->SetComputeTexture(10, targets->GBufferVelocity.get());

                        cmd->SetComputeUnorderedAccessBuffer(
                            0, targets->MegaLightsReservoirHistory[historyWriteIndex].get());
                        cmd->SetComputeUnorderedAccessBuffer(1, targets->MegaLightsHistoryGuide[historyWriteIndex].get());
                        cmd->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);
                    },
                });
            }

            // --- 空間再利用: 近傍が選んだ灯を借りて自分の面で評価し直す ---
            // 候補プールの重みが法線を見られないぶんを、「選んだあとで隣から借りる」ことで
            // 埋め合わせる。【レイは増える】目標関数に可視性を入れるために標的ごとに1本、
            // 不偏化の分母でも可視性が不明な近傍に補正レイを撃つ(MegaLightsSpatial.hlsl 冒頭)。
            // そのぶん Shade 側の影レイは省ける
            for (uint32_t spatialIteration = 0u; spatialIteration < spatialIterations; ++spatialIteration)
            {
                // 1回目の入力は再利用の連鎖の出力、2回目以降は前の反復の出力
                RHI::IRHIBuffer* const spatialInput =
                    (spatialIteration == 0u)
                        ? reuseInputBuffer
                        : spatialPingPong[(spatialIteration - 1u) % Passes::kMegaLightsMaxSpatialIterations];
                RHI::IRHIBuffer* const spatialOutput =
                    spatialPingPong[spatialIteration % Passes::kMegaLightsMaxSpatialIterations];
                RHI::IRHIBuffer* const spatialConstants =
                    m_MegaLightsSpatialConstantBuffer[spatialIteration % Passes::kMegaLightsMaxSpatialIterations].get();
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "MegaLightsSpatial",
                    .Reads =
                    {
                        targets->GBufferAlbedo.get(), targets->GBufferNormal.get(), targets->GBufferMaterial.get(), targets->GBufferDepth.get(),
                        brdfLUTTexture,
                    },
                    // 入力は「時間再利用を挟んだならその出力、挟まないならInitialの出力」。
                    // 初期リザーバ(今フレームの殺しの持ち回り)も自画素の遮蔽の確定情報として読む
                    .BufferReads = { lightBuffer, spatialInput, targets->MegaLightsTilePoolBuffer.get(),
                                     targets->MegaLightsReservoirBuffer.get(), targets->MegaLightsBlockedLightBuffer.get() },
                    .BufferWrites = { spatialOutput },
                    .Execute = [this, targets, lightBuffer, raytracingScene, brdfLUTTexture, spatialInput, spatialOutput, spatialConstants, spatialIteration, buildStochasticConstants, renderWidth, renderHeight, frameConstantBuffer, screenSpaceSamplers](RHI::IRHICommandList* cmd)
                    {
                        // 【この定数だけは自分で更新する】反復番号が反復ごとに違うため、
                        // Initial が更新する共有分は使えない。UpdateBuffer と
                        // SetConstantBuffer の順序は厳守(逆にすると前フレームの値を読む)
                        const MegaLightsStochasticConstants iterationConstants =
                            buildStochasticConstants(spatialIteration);
                        cmd->UpdateBuffer(spatialConstants, &iterationConstants, sizeof(iterationConstants));
                        cmd->SetComputePipelineState(m_MegaLightsSpatialPipelineState.get());
                        cmd->SetComputeConstantBuffer(0, frameConstantBuffer);
                        cmd->SetComputeConstantBuffer(1, spatialConstants);
                        cmd->SetComputeSamplerSet(screenSpaceSamplers);

                        // レジスタ割り当てはMegaLightsSpatial.hlsl側の宣言と一致させること。
                        // 不偏化の分母(Z)の判定にバイアス補正レイを撃つのでTLASが要る。
                        // 【使わないフレームでも必ずバインドする】DX12は宣言された
                        // リソースが未バインドだと壊れる
                        cmd->SetComputeAccelerationStructure(0, raytracingScene->GetTopLevelAS());
                        cmd->SetComputeTexture(1, targets->GBufferNormal.get());
                        cmd->SetComputeTexture(2, targets->GBufferDepth.get());
                        cmd->SetComputeTexture(3, targets->GBufferAlbedo.get());
                        cmd->SetComputeTexture(4, targets->GBufferMaterial.get());
                        cmd->SetComputeTexture(5, brdfLUTTexture);
                        cmd->SetComputeShaderResourceBuffer(6, lightBuffer);
                        cmd->SetComputeShaderResourceBuffer(7, spatialInput);
                        // MIS重みが「その灯が隣のタイルへ届くか」を判定するのに、
                        // 候補プールのヘッダ(タイルの深度スラブ)を読む
                        cmd->SetComputeShaderResourceBuffer(8, targets->MegaLightsTilePoolBuffer.get());
                        // 今フレームの初期リザーバ。殺しの持ち回り(=自画素の遮蔽の確定情報)を
                        // 選択から外すのに使う
                        cmd->SetComputeShaderResourceBuffer(9, targets->MegaLightsReservoirBuffer.get());
                        // 遮蔽が確定した灯のキャッシュ
                        cmd->SetComputeShaderResourceBuffer(10, targets->MegaLightsBlockedLightBuffer.get());

                        cmd->SetComputeUnorderedAccessBuffer(0, spatialOutput);
                        cmd->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);
                    },
                });
            }

            if (megaLightsQuadShared)
            {
                // --- クアッド共有の解決: 2x2の4標本を自分の面で評価し直して平均する ---
                // レイを1本も撃たないのでTLASを束縛しない。可視性は Initial が撃った
                // 1本の結果を仲間から借りる(受け入れた偏りの本体。MegaLightsResolve.hlsl 冒頭)。
                //
                // 【履歴ガイドをここで書く】手法3は時間再利用パスを持たないので、
                // デノイザが「前フレームの幾何」を引くためのガイドを書く者がいなくなる。
                // 書かないと動く細い形状でデノイザの履歴が構造的に必ず棄却される
                // (docs/ImplementationDetail.md 61.7g.6)
                RHI::IRHIBuffer* const guideWriteBuffer = targets->MegaLightsHistoryGuide[historyWriteIndex].get();
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "MegaLightsResolve",
                    .Reads =
                    {
                        targets->GBufferAlbedo.get(), targets->GBufferNormal.get(), targets->GBufferMaterial.get(),
                        targets->GBufferDepth.get(), brdfLUTTexture,
                    },
                    .Writes = { targets->MegaLightsTexture.get() },
                    .BufferReads = { lightBuffer, targets->MegaLightsReservoirBuffer.get() },
                    .BufferWrites = { guideWriteBuffer },
                    .Execute = [this, targets, lightBuffer, brdfLUTTexture, guideWriteBuffer, renderWidth, renderHeight, frameConstantBuffer, screenSpaceSamplers](RHI::IRHICommandList* cmd)
                    {
                        // 定数はInitial側で更新済み。ここでバインドし直すのは、DX12が
                        // SetPipelineStateのたびにルート引数を無効化するため
                        cmd->SetComputePipelineState(m_MegaLightsResolvePipelineState.get());
                        cmd->SetComputeConstantBuffer(0, frameConstantBuffer);
                        cmd->SetComputeConstantBuffer(1, m_MegaLightsStochasticConstantBuffer.get());
                        cmd->SetComputeSamplerSet(screenSpaceSamplers);

                        // レジスタ割り当てはMegaLightsResolve.hlsl側の宣言と一致させること。
                        // **t0(TLAS)は宣言していない** ―― レイを撃たないパスなので張らない
                        cmd->SetComputeTexture(1, targets->GBufferNormal.get());
                        cmd->SetComputeTexture(2, targets->GBufferDepth.get());
                        cmd->SetComputeTexture(3, targets->GBufferAlbedo.get());
                        cmd->SetComputeTexture(4, targets->GBufferMaterial.get());
                        cmd->SetComputeTexture(5, brdfLUTTexture);
                        cmd->SetComputeShaderResourceBuffer(6, lightBuffer);
                        cmd->SetComputeShaderResourceBuffer(7, targets->MegaLightsReservoirBuffer.get());

                        cmd->SetComputeUnorderedAccessTexture(0, targets->MegaLightsTexture.get());
                        cmd->SetComputeUnorderedAccessBuffer(1, guideWriteBuffer);
                        cmd->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);
                    },
                });
            }
            else
            {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "MegaLightsShade",
                .Reads =
                {
                    targets->GBufferAlbedo.get(), targets->GBufferNormal.get(), targets->GBufferMaterial.get(), targets->GBufferDepth.get(),
                    brdfLUTTexture,
                },
                .Writes = { targets->MegaLightsTexture.get() },
                .BufferReads = { lightBuffer, shadeReservoirBuffer },
                .Execute = [this, targets, lightBuffer, raytracingScene, brdfLUTTexture, shadeReservoirBuffer, renderWidth, renderHeight, frameConstantBuffer, screenSpaceSamplers](RHI::IRHICommandList* cmd)
                {
                    // 定数はInitial側で更新済み。ここでバインドし直すのは、DX12が
                    // SetPipelineStateのたびにルート引数を無効化するため
                    cmd->SetComputePipelineState(m_MegaLightsShadePipelineState.get());
                    cmd->SetComputeConstantBuffer(0, frameConstantBuffer);
                    cmd->SetComputeConstantBuffer(1, m_MegaLightsStochasticConstantBuffer.get());
                    cmd->SetComputeSamplerSet(screenSpaceSamplers);

                    // レジスタ割り当てはMegaLightsShade.hlsl側の宣言と一致させること
                    cmd->SetComputeAccelerationStructure(0, raytracingScene->GetTopLevelAS());
                    cmd->SetComputeTexture(1, targets->GBufferNormal.get());
                    cmd->SetComputeTexture(2, targets->GBufferDepth.get());
                    cmd->SetComputeTexture(3, targets->GBufferAlbedo.get());
                    cmd->SetComputeTexture(4, targets->GBufferMaterial.get());
                    cmd->SetComputeTexture(5, brdfLUTTexture);
                    cmd->SetComputeShaderResourceBuffer(6, lightBuffer);
                    // 空間再利用を挟んだフレームはその出力を、挟まないフレームはInitialの出力を読む
                    cmd->SetComputeShaderResourceBuffer(7, shadeReservoirBuffer);

                    cmd->SetComputeUnorderedAccessTexture(0, targets->MegaLightsTexture.get());
                    cmd->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);
                },
            });
            }
        }

        // --- デノイザ(段階5): 時間累積 + エッジ停止付き a-trous ---
        // 【蓄積パス(計測)より前に置く】計測したいのはデノイズ後の絵。
        // 【TAAより前に落とす】ノイズを残したまま渡すとTAAが履歴を毎フレーム棄却し、
        // ノイズもAAも両方失う(MegaLightsDenoise.hlsl 冒頭)
        // 手法2と手法3は同じデノイザを共有する(入力は「確率的に作られた1枚の絵」で同じもの)
        const bool megaLightsDenoiseRuns = megaLightsRuns &&
                                           (frame.Settings.MegaLights.Mode == MegaLightsMode::Stochastic ||
                                            frame.Settings.MegaLights.Mode == MegaLightsMode::QuadShared) &&
                                           frame.Settings.MegaLights.DenoiseEnabled && m_MegaLightsDenoiseTemporalPSO &&
                                           targets->MegaLightsDenoisedTexture != nullptr;
        bb.MegaLightsDenoiseRuns = megaLightsDenoiseRuns;
        if (megaLightsDenoiseRuns)
        {
            const uint32_t denoiseWrite = m_MegaLightsDenoiseHistoryIndex;
            const uint32_t denoiseRead = denoiseWrite ^ 1u;
            // 履歴の妥当性判定に「前フレームの幾何」を使えるか。ガイドを毎フレーム全画素へ
            // 書いているのは、手法2では時間再利用、手法3では Resolve。
            // どちらも走っていなければ更新されないので使えない
            const bool denoiseGuideWritten =
                (frame.Settings.MegaLights.Mode == MegaLightsMode::QuadShared)
                    ? (m_MegaLightsResolvePipelineState != nullptr)
                    : (frame.Settings.MegaLights.TemporalEnabled && m_MegaLightsTemporalPipelineState != nullptr);
            const bool denoiseGuideValid =
                denoiseGuideWritten && targets->MegaLightsHistoryGuide[0] && m_MegaLightsHistoryValid;
            // 【読むのは前フレームが書いた側】今フレームの時間再利用はもう片方へ書いている
            RHI::IRHIBuffer* const denoiseGuideBuffer =
                targets->MegaLightsHistoryGuide[m_MegaLightsHistoryIndex ^ 1u]
                    ? targets->MegaLightsHistoryGuide[m_MegaLightsHistoryIndex ^ 1u].get()
                    : nullptr;
            const std::vector<RHI::IRHIBuffer*> denoiseGuideReads =
                denoiseGuideBuffer ? std::vector<RHI::IRHIBuffer*>{ denoiseGuideBuffer }
                                   : std::vector<RHI::IRHIBuffer*>{};
            const int atrousPasses = std::clamp(frame.Settings.MegaLights.DenoiseAtrousPasses, 0, 5);

            const auto updateDenoiseConstants =
                [this, megaLightsSettings, denoiseGuideValid, renderWidth, renderHeight](RHI::IRHICommandList* cmd, uint32_t pass, float stepWidth)
            {
                Passes::MegaLightsDenoiseConstants denoiseConstants{};
                denoiseConstants.Params0 = {
                    renderWidth, renderHeight, m_MegaLightsDenoiseHistoryValid ? 1u : 0u, pass
                };
                // 時間累積の上限は手法ごとに別の変数を持つ。手法3にはリザーバの履歴が
                // 無く、デノイザだけが時間方向の記憶なので長くしてある(EngineDefaults.h)
                const int32_t denoiseMaxFrames = (megaLightsSettings.Mode == MegaLightsMode::QuadShared)
                                                     ? megaLightsSettings.QuadDenoiseMaxFrames
                                                     : megaLightsSettings.DenoiseMaxFrames;
                denoiseConstants.Params1 = {
                    stepWidth,
                    static_cast<float>(std::max(1, denoiseMaxFrames)),
                    // 輝度のエッジ停止の強さ(σ_l)。根拠は EngineDefaults.h の宣言に書いてある
                    megaLightsSettings.DenoiseSigmaLuminance,
                    // 法線のエッジ停止の指数(同128)
                    128.0f,
                };
                // 深度のエッジ停止(View空間Zに対する相対差なので無次元)と、
                // ファイアフライの近傍クランプの強さ(近傍平均 + k・標準偏差で頭打ちにする)
                denoiseConstants.Params2 = {
                    0.02f, megaLightsSettings.DenoiseFireflyClamp, denoiseGuideValid ? 1.0f : 0.0f, 0.0f
                };
                cmd->UpdateBuffer(
                    m_MegaLightsDenoiseConstantBuffer.get(), &denoiseConstants, sizeof(denoiseConstants));
            };

            // G-Bufferの束縛。**DX12はSetPipelineStateのたびにルート引数を無効化する**ので
            // パスごとに張り直す
            const auto bindDenoiseCommon = [this, targets, denoiseGuideBuffer, frameConstantBuffer, screenSpaceSamplers](RHI::IRHICommandList* cmd)
            {
                cmd->SetComputeConstantBuffer(0, frameConstantBuffer);
                // 前フレームの幾何。【使わないパスでも必ず張る】DX12はPSO切替でルート引数が
                // 無効化されるため、宣言したリソースが未バインドだと壊れる
                if (denoiseGuideBuffer)
                {
                    cmd->SetComputeShaderResourceBuffer(0, denoiseGuideBuffer);
                }
                cmd->SetComputeConstantBuffer(1, m_MegaLightsDenoiseConstantBuffer.get());
                cmd->SetComputeSamplerSet(screenSpaceSamplers);
                cmd->SetComputeTexture(1, targets->GBufferNormal.get());
                cmd->SetComputeTexture(2, targets->GBufferDepth.get());
                cmd->SetComputeTexture(3, targets->GBufferAlbedo.get());
                cmd->SetComputeTexture(4, targets->GBufferMaterial.get());
                cmd->SetComputeTexture(5, targets->GBufferVelocity.get());
            };

            // --- 時間累積: 生出力を復調して履歴と混ぜる ---
            // 【履歴もここで書く】SVGFは1段目のa-trous出力を履歴にするが、こちらは時間累積の
            // 出力をそのまま履歴にしている。パスが1本減るぶん履歴のノイズは多いが、
            // 指数移動平均が均すので破綻はしない。差が問題になったら分ける
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "MegaLightsDenoiseTemporal",
                .Reads =
                {
                    targets->MegaLightsTexture.get(), targets->GBufferAlbedo.get(), targets->GBufferNormal.get(),
                    targets->GBufferMaterial.get(), targets->GBufferDepth.get(), targets->GBufferVelocity.get(),
                    targets->MegaLightsDenoiseHistory[denoiseRead].get(),
                    targets->MegaLightsDenoiseMoments[denoiseRead].get(),
                },
                .Writes =
                {
                    targets->MegaLightsDenoisePing[0].get(), targets->MegaLightsDenoiseMomentPing[0].get(),
                    targets->MegaLightsDenoiseHistory[denoiseWrite].get(),
                    targets->MegaLightsDenoiseMoments[denoiseWrite].get(),
                },
                // 前フレームの幾何は「前フレームが書いた側」なので今フレームに書き手はいない。
                // 辺は張れないが、ping-pongで別バッファになっているので衝突しない
                .BufferReads = denoiseGuideReads,
                .Execute = [this, targets, denoiseRead, denoiseWrite, updateDenoiseConstants, bindDenoiseCommon, renderWidth, renderHeight](RHI::IRHICommandList* cmd)
                {
                    updateDenoiseConstants(cmd, 0u, 1.0f);
                    cmd->SetComputePipelineState(m_MegaLightsDenoiseTemporalPSO.get());
                    bindDenoiseCommon(cmd);
                    cmd->SetComputeTexture(6, targets->MegaLightsTexture.get());
                    // 【読むのは前フレームが書いた側】今フレームはもう片方へ書くので
                    // 同一フレーム内でのWARが生じない(RenderGraphはWARの辺を張らない)
                    cmd->SetComputeTexture(7, targets->MegaLightsDenoiseHistory[denoiseRead].get());
                    cmd->SetComputeTexture(8, targets->MegaLightsDenoiseMoments[denoiseRead].get());
                    cmd->SetComputeUnorderedAccessTexture(0, targets->MegaLightsDenoisePing[0].get());
                    cmd->SetComputeUnorderedAccessTexture(1, targets->MegaLightsDenoiseMomentPing[0].get());
                    cmd->SetComputeUnorderedAccessTexture(2, targets->MegaLightsDenoiseHistory[denoiseWrite].get());
                    cmd->SetComputeUnorderedAccessTexture(3, targets->MegaLightsDenoiseMoments[denoiseWrite].get());
                    cmd->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);
                },
            });

            // --- a-trous: 段ごとにステップ幅を倍にしてping-pong ---
            for (int atrousPass = 0; atrousPass < atrousPasses; ++atrousPass)
            {
                const int atrousSrc = atrousPass & 1;
                const int atrousDst = atrousSrc ^ 1;
                const float atrousStep = static_cast<float>(1 << atrousPass);
                graph.AddPass(Core::RenderGraphPassDesc{
                    .Name = "MegaLightsDenoiseAtrous",
                    .Reads =
                    {
                        targets->MegaLightsDenoisePing[atrousSrc].get(),
                        targets->MegaLightsDenoiseMomentPing[atrousSrc].get(),
                        targets->GBufferNormal.get(), targets->GBufferDepth.get(), targets->GBufferAlbedo.get(),
                        targets->GBufferMaterial.get(), targets->GBufferVelocity.get(),
                    },
                    .Writes =
                    {
                        targets->MegaLightsDenoisePing[atrousDst].get(),
                        targets->MegaLightsDenoiseMomentPing[atrousDst].get(),
                    },
                    .Execute = [this, targets, atrousSrc, atrousDst, atrousPass, atrousStep, updateDenoiseConstants, bindDenoiseCommon, renderWidth, renderHeight](RHI::IRHICommandList* cmd)
                    {
                        updateDenoiseConstants(cmd, static_cast<uint32_t>(atrousPass + 1), atrousStep);
                        cmd->SetComputePipelineState(m_MegaLightsDenoiseAtrousPSO.get());
                        bindDenoiseCommon(cmd);
                        cmd->SetComputeTexture(6, targets->MegaLightsDenoisePing[atrousSrc].get());
                        // t7は使わないが、DX12は宣言したリソースを全部束縛しないと壊れる
                        cmd->SetComputeTexture(7, targets->MegaLightsDenoisePing[atrousSrc].get());
                        cmd->SetComputeTexture(8, targets->MegaLightsDenoiseMomentPing[atrousSrc].get());
                        cmd->SetComputeUnorderedAccessTexture(0, targets->MegaLightsDenoisePing[atrousDst].get());
                        cmd->SetComputeUnorderedAccessTexture(
                            1, targets->MegaLightsDenoiseMomentPing[atrousDst].get());
                        cmd->SetComputeUnorderedAccessTexture(2, targets->MegaLightsDenoisePing[atrousDst].get());
                        cmd->SetComputeUnorderedAccessTexture(
                            3, targets->MegaLightsDenoiseMomentPing[atrousDst].get());
                        cmd->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);
                    },
                });
            }

            // --- 復調を戻して最終出力にする ---
            const int denoiseFinalSrc = atrousPasses & 1;
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "MegaLightsDenoiseRemodulate",
                .Reads =
                {
                    targets->MegaLightsDenoisePing[denoiseFinalSrc].get(),
                    targets->MegaLightsDenoiseMomentPing[denoiseFinalSrc].get(),
                    targets->GBufferAlbedo.get(), targets->GBufferMaterial.get(), targets->GBufferDepth.get(),
                    targets->GBufferNormal.get(), targets->GBufferVelocity.get(),
                },
                .Writes = { targets->MegaLightsDenoisedTexture.get() },
                .Execute = [this, targets, denoiseFinalSrc, updateDenoiseConstants, bindDenoiseCommon, renderWidth, renderHeight](RHI::IRHICommandList* cmd)
                {
                    updateDenoiseConstants(cmd, 0u, 1.0f);
                    cmd->SetComputePipelineState(m_MegaLightsDenoiseRemodulatePSO.get());
                    bindDenoiseCommon(cmd);
                    cmd->SetComputeTexture(6, targets->MegaLightsDenoisePing[denoiseFinalSrc].get());
                    cmd->SetComputeTexture(7, targets->MegaLightsDenoisePing[denoiseFinalSrc].get());
                    cmd->SetComputeTexture(8, targets->MegaLightsDenoiseMomentPing[denoiseFinalSrc].get());
                    cmd->SetComputeUnorderedAccessTexture(0, targets->MegaLightsDenoisedTexture.get());
                    cmd->SetComputeUnorderedAccessTexture(1, targets->MegaLightsDenoisedTexture.get());
                    cmd->SetComputeUnorderedAccessTexture(2, targets->MegaLightsDenoisedTexture.get());
                    cmd->SetComputeUnorderedAccessTexture(3, targets->MegaLightsDenoisedTexture.get());
                    cmd->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);
                },
            });
        }

        // 計測が読む先。デノイザを通したフレームはその出力になる
        RHI::IRHITexture* const megaLightsAccumSourceTexture =
            (megaLightsDenoiseRuns && targets->MegaLightsDenoisedTexture) ? targets->MegaLightsDenoisedTexture.get()
                                                                   : targets->MegaLightsTexture.get();

        // --- MegaLightsの蓄積パス(計測専用): 出力を線形空間でフレーム方向へ足し込む。
        //     トーンマップ後の8bitをN枚平均しても、トーンマップが凹関数なので
        //     「偏りが無くてもノイズがあるだけで平均が低く出る」。線形で足す場所がここに要る ---
        // 整定を待ってから足し始める(内部解像度の切り替えとストリーミングが片付くまで)
        ++m_MegaLightsAccumWarmupFrames;
        const bool megaLightsAccumRuns = megaLightsRuns && frame.Settings.MegaLights.AccumTargetFrames > 0 &&
                                         m_MegaLightsAccumPipelineState && targets->MegaLightsAccumBuffer &&
                                         m_MegaLightsAccumWarmupFrames > Passes::kMegaLightsAccumWarmup &&
                                         m_MegaLightsAccumFrames < static_cast<uint32_t>(frame.Settings.MegaLights.AccumTargetFrames);
        if (megaLightsAccumRuns)
        {
            // 最初の1枚は「足す」ではなく「代入する」。RHIにバッファのクリアが無いため
            const uint32_t accumReset = (m_MegaLightsAccumFrames == 0u) ? 1u : 0u;
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "MegaLightsAccum",
                // 【計測はデノイズ後の絵を測る】デノイザを通したフレームはその出力を読む。
                // ここを生出力のままにすると、デノイザのON/OFFで測定値が1ビットも動かず
                // 「効いていない」と誤診する(実際に一度そう出た)
                .Reads = { megaLightsAccumSourceTexture },
                .BufferWrites = { targets->MegaLightsAccumBuffer.get() },
                .Execute = [this, targets, accumReset, megaLightsAccumSourceTexture, renderWidth, renderHeight](RHI::IRHICommandList* cmd)
                {
                    Passes::MegaLightsAccumConstants accumConstants{};
                    accumConstants.Params0 = { renderWidth, renderHeight, accumReset, 0u };
                    cmd->UpdateBuffer(m_MegaLightsAccumConstantBuffer.get(), &accumConstants, sizeof(accumConstants));

                    cmd->SetComputePipelineState(m_MegaLightsAccumPipelineState.get());
                    cmd->SetComputeConstantBuffer(0, m_MegaLightsAccumConstantBuffer.get());
                    cmd->SetComputeTexture(0, megaLightsAccumSourceTexture);
                    // UAVはDispatch直後に解除されるため毎回バインドし直す
                    cmd->SetComputeUnorderedAccessBuffer(0, targets->MegaLightsAccumBuffer.get());
                    cmd->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);
                },
            });
            ++m_MegaLightsAccumFrames;
        }

        // 【この位置で publish すること】上の if の中で増えたぶんを含めた値を、
        // 後続の PresentPass が Mode 22 の除数として読む。増える前に配ると1つ古くなる
        bb.MegaLightsAccumFrames = m_MegaLightsAccumFrames;

        // --- 蓄積し終えた平均を生データで書き出す(計測専用) ---
        // 画面キャプチャは8bit・トーンマップ後で、丸めだけでRMSEに0.29階調の下限が生まれる。
        // 「平均が真値へ 1/√N で寄るか」はその下限に隠れて読めないので、線形のまま取り出す
        if (!m_MegaLightsDumpPath.empty() && !m_MegaLightsDumpDone && targets->MegaLightsAccumBuffer &&
            frame.Settings.MegaLights.AccumTargetFrames > 0 &&
            m_MegaLightsAccumFrames >= static_cast<uint32_t>(frame.Settings.MegaLights.AccumTargetFrames))
        {
            const uint32_t accumBytes =
                static_cast<uint32_t>(sizeof(float) * 4) * renderWidth * renderHeight;

            if (!m_MegaLightsDumpIssued)
            {
                if (!m_MegaLightsAccumReadback)
                {
                    try
                    {
                        RHI::BufferDesc readbackDesc;
                        readbackDesc.Usage = RHI::BufferUsage::Readback;
                        readbackDesc.SizeInBytes = accumBytes;
                        readbackDesc.StrideInBytes = static_cast<uint32_t>(sizeof(float) * 4);
                        m_MegaLightsAccumReadback = frame.Device->CreateBuffer(readbackDesc);
                    }
                    catch (const std::exception& e)
                    {
                        Core::Logger::Error(
                            "KurenaiEngine3D",
                            std::string("MegaLightsの蓄積平均の読み戻しバッファを作れませんでした: ") + e.what());
                        m_MegaLightsDumpDone = true; // 何度も試さない
                    }
                }

                if (m_MegaLightsAccumReadback)
                {
                    graph.AddPass(Core::RenderGraphPassDesc{
                        .Name = "MegaLightsDump",
                        .BufferReads = { targets->MegaLightsAccumBuffer.get() },
                        .Execute = [this, targets, accumBytes](RHI::IRHICommandList* cmd)
                        {
                            cmd->CopyBufferToReadback(
                                m_MegaLightsAccumReadback.get(), targets->MegaLightsAccumBuffer.get(), accumBytes);
                        },
                    });
                    m_MegaLightsDumpIssued = true;
                    m_MegaLightsDumpCopyFrame = frame.FrameIndex;
                }
            }
            // GPUの実行はCPUより数フレーム遅れる。積んだ直後に読むと未完了の内容を掴む
            else if (frame.FrameIndex - m_MegaLightsDumpCopyFrame >= 5u)
            {
                std::vector<float> host(static_cast<size_t>(renderWidth) * renderHeight * 4u);
                if (m_MegaLightsAccumReadback->ReadbackData(host.data(), accumBytes))
                {
                    std::ofstream file(m_MegaLightsDumpPath, std::ios::binary | std::ios::trunc);
                    if (file)
                    {
                        // 形式: 'K','M','L','A' / 幅 / 高さ / 足したフレーム数 / 予約 / float4 × 画素数
                        const char magic[4] = { 'K', 'M', 'L', 'A' };
                        const uint32_t header[4] = { renderWidth, renderHeight, m_MegaLightsAccumFrames, 0u };
                        file.write(magic, sizeof(magic));
                        file.write(reinterpret_cast<const char*>(header), sizeof(header));
                        file.write(reinterpret_cast<const char*>(host.data()),
                                   static_cast<std::streamsize>(accumBytes));
                        Core::Logger::Info(
                            "KurenaiEngine3D",
                            "MegaLightsの蓄積平均を書き出しました: " + Core::WideToUtf8(m_MegaLightsDumpPath) +
                                " (" + std::to_string(renderWidth) + "x" + std::to_string(renderHeight) +
                                ", " + std::to_string(m_MegaLightsAccumFrames) + "フレームぶん)");
                    }
                    else
                    {
                        Core::Logger::Error(
                            "KurenaiEngine3D",
                            "MegaLightsの蓄積平均を書き出せませんでした(ファイルを開けない): " +
                                Core::WideToUtf8(m_MegaLightsDumpPath));
                    }
                }
                else
                {
                    Core::Logger::Error(
                        "KurenaiEngine3D", "MegaLightsの蓄積平均の読み戻しに失敗しました");
                }
                m_MegaLightsDumpDone = true;
            }
        }
    }
}
