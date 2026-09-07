#include "../KurenaiEngine3D.h"

#include <algorithm>

#include "Core/RenderGraph.h"
#include "PresentPass.h"
#include "DDGIConstants.h"
#include "EnvironmentConstants.h"
#include "MegaLightsConstants.h"
#include "../Rendering/ShadowConstants.h"
#include "../Rendering/RenderBlackboard.h"
#include "../Rendering/RenderFrameContext.h"

namespace Kurenai::Passes
{
    namespace
    {
        // レンダー解像度(renderWidth x renderHeight)のアスペクト比を保ったまま、
        // windowWidth x windowHeight の中央に収まるビューポート(レターボックス/ピラーボックス)を求める
        RHI::Viewport ComputeLetterboxViewport(uint32_t windowWidth, uint32_t windowHeight, uint32_t renderWidth, uint32_t renderHeight)
        {
            const float windowAspect = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
            const float renderAspect = static_cast<float>(renderWidth) / static_cast<float>(renderHeight);

            float viewportWidth;
            float viewportHeight;
            if (windowAspect > renderAspect)
            {
                // ウィンドウの方が横長 -> 高さいっぱいに合わせ、左右に余白(ピラーボックス)
                viewportHeight = static_cast<float>(windowHeight);
                viewportWidth = viewportHeight * renderAspect;
            }
            else
            {
                // ウィンドウの方が縦長 -> 幅いっぱいに合わせ、上下に余白(レターボックス)
                viewportWidth = static_cast<float>(windowWidth);
                viewportHeight = viewportWidth / renderAspect;
            }

            RHI::Viewport viewport;
            viewport.TopLeftX = (static_cast<float>(windowWidth) - viewportWidth) * 0.5f;
            viewport.TopLeftY = (static_cast<float>(windowHeight) - viewportHeight) * 0.5f;
            viewport.Width = viewportWidth;
            viewport.Height = viewportHeight;
            return viewport;
        }
    }

    void PresentPass::Register(
        Core::RenderGraph& graph,
        RHI::IRHICommandList* commandList,
        const Rendering::RenderFrameContext& frame,
        const Rendering::RenderBlackboard& bb)
    {
        // 【フレームの写しをローカルで受ける】frame自体はラムダへ捕捉しない
        const Rendering::GIResources* const gi = frame.GI;

        // 【フレームの写しをローカルで受ける】frame自体はラムダへ捕捉しない
        const Rendering::RenderTargets* const targets = frame.Targets;

        // 【フレームの写しをローカルで受ける】frame自体はラムダへ捕捉しない
        RHI::IRHITexture* const cloudDetailNoiseTexture = frame.Sky->CloudDetailNoiseTexture.get();
        RHI::IRHITexture* const cloudShapeNoiseTexture = frame.Sky->CloudShapeNoiseTexture.get();
        RHI::IRHITexture* const multiScatteringLUT = frame.Sky->MultiScatteringLUT.get();
        RHI::IRHITexture* const skyViewLUT = frame.Sky->SkyViewLUT.get();
        RHI::IRHITexture* const transmittanceLUT = frame.Sky->TransmittanceLUT.get();

        // 【フレームの写しをローカルで受ける】frame自体はラムダへ捕捉しない
        RHI::IRHITexture* const brdfLUTTexture = frame.IBL->BRDFLUTTexture.get();
        RHI::IRHITexture* const irradianceTexture = frame.IBL->IrradianceTexture.get();
        RHI::IRHITexture* const prefilteredEnvTexture = frame.IBL->PrefilteredEnvTexture.get();

        // 【述語の結果はフレームの写しから引く】判定そのものは Should* が唯一の実装で、
        // ここで作り直さない。ラムダへ値で渡すためローカルで受ける
        const bool megaLightsRuns = frame.MegaLightsRuns;
        const bool raytracedShadowRuns = frame.RaytracedShadowRuns;

        const uint32_t renderWidth = frame.RenderWidth;
        const uint32_t renderHeight = frame.RenderHeight;
        RHI::IRHIBuffer* const frameConstantBuffer = frame.FrameConstantBuffer;
        RHI::IRHISamplerSet* const screenSpaceSamplers = frame.ScreenSpaceSamplers;

        // --- Presentパス: 選択中のレンダーターゲットを、アスペクト比を保ってバックバッファへ出力 ---
        // デバッグ表示(Render Targets UI)で選択されたバッファに応じて表示ソースを切り替える。
        // 深度バッファ(GBuffer深度・シャドウマップ)はPresent.hlsl側でグレースケール化するためMode=1を渡す
        RHI::IRHITexture* presentSourceTexture = targets->TonemapTexture.get();
        // Mode 9(IBL Irradiance/Prefilterのキューブマップ表示)専用。他のModeでは使われないが、
        // t1には常に何らかの有効なTextureCubeをバインドしておく必要があるため既定値を持たせる
        RHI::IRHITexture* presentDebugCubeTexture = frame.SkyTexture;
        // Mode 10(シャドウマップのカスケード表示)専用。t1と同じ理由で、t2にも常に有効な
        // Texture2DArrayをバインドしておく必要があるためシャドウマップ配列自身を既定値にする
        RHI::IRHITexture* presentDebugArrayTexture = targets->ShadowCascadeArray.get();
        // Mode 12(反射プローブのキューブマップ配列)専用。TextureCube(t1)ともTexture2DArray(t2)とも
        // 型が違うためさらに別スロット(t4)が要る。こちらも常に有効なテクスチャをバインドしておく
        // (反射プローブは鏡面専任なので、既定値はプリフィルタ済み鏡面の配列にしてある)
        RHI::IRHITexture* presentDebugCubeArrayTexture = gi->ProbePrefilteredArray.get();
        // Mode 18(雲の3Dノイズ)専用。Texture3Dはここまでのどの型とも別なのでさらに
        // 別スロット(t5)が要る。他と同じく常に有効なテクスチャをバインドしておく
        RHI::IRHITexture* presentDebugVolumeTexture = cloudShapeNoiseTexture;
        int32_t presentMode = 0;
        uint32_t presentSourceWidth = renderWidth;
        uint32_t presentSourceHeight = renderHeight;
        switch (frame.Settings.DebugView.View)
        {
        case DebugView::Final:
            // Tonemapパスが既にSSR有効/無効を考慮したHDRソースをLDR変換済みのため、そのまま使う。
            // 超解像が有効なときは、その先のRCASまで通した出力解像度の結果へ差し替える。
            // レターボックスの基準になるpresentSourceWidth/Heightも出力解像度にすること
            // (ここを内部解像度のままにすると、拡大済みの絵をさらに拡大してしまう)
            if (bb.UpscaleActive)
            {
                presentSourceTexture = m_Engine.m_UpscaleSharpTexture.get();
                presentSourceWidth = m_Engine.m_UpscaleTargetWidth;
                presentSourceHeight = m_Engine.m_UpscaleTargetHeight;
            }
            else
            {
                presentSourceTexture = targets->TonemapTexture.get();
            }
            break;
        case DebugView::Albedo:
            presentSourceTexture = targets->GBufferAlbedo.get();
            break;
        case DebugView::Normal:
            presentSourceTexture = targets->GBufferNormal.get();
            presentMode = 7; // オクタヘドラルエンコードをデコードして[0,1]へ再マップして表示
            break;
        case DebugView::Material:
            presentSourceTexture = targets->GBufferMaterial.get();
            break;
        case DebugView::Emissive:
            presentSourceTexture = targets->GBufferEmissive.get();
            break;
        case DebugView::Depth:
            presentSourceTexture = targets->GBufferDepth.get();
            presentMode = 2;
            break;
        case DebugView::DepthRaw:
            presentSourceTexture = targets->GBufferDepth.get();
            presentMode = 5; // 生の深度値(0〜1)を加工せずそのまま表示(reverse-z等の生値確認用)
            break;
        case DebugView::DirectLight:
            presentSourceTexture = targets->DirectLightTexture.get();
            presentMode = 4; // HDRのためトーンマッピング(Reinhard)+ガンマ補正して表示
            break;
        case DebugView::MegaLights:
            // MegaLightsが求めたポイント/スポットの直接光。パスが今フレーム実行されていない場合、
            // バッファの中身は前フレーム/未定義の残骸なので最終結果のまま何も切り替えない
            // (SWラスタ・PlanarReflection・RTShadowのデバッグ表示と同じ方針)。
            // ModeはDebugView::DirectLightと同じ4 ―― 並べて差分を取るのが目的なので、
            // 表示側の処理まで一致させる
            if (megaLightsRuns)
            {
                presentSourceTexture = m_Engine.m_MegaLightsTexture.get();
                presentMode = 4;
            }
            break;
        case DebugView::AOIndirectLight:
            presentSourceTexture = frame.ActiveAOTexture;
            presentMode = 0; // rgb(間接拡散光)をそのまま表示。SSAOはrgbが常に0のため常に黒になる
            break;
        case DebugView::AOIndirectLightRaw:
            presentSourceTexture = frame.ActiveAORawTexture;
            presentMode = 0; // ブラー前の生値(タイル状ノイズが乗った状態)
            break;
        case DebugView::AOOcclusion:
            presentSourceTexture = frame.ActiveAOTexture;
            presentMode = 3; // a(遮蔽率)をグレースケール表示
            break;
        case DebugView::AOOcclusionRaw:
            presentSourceTexture = frame.ActiveAORawTexture;
            presentMode = 3; // ブラー前の生値(タイル状ノイズが乗った状態)
            break;
        case DebugView::ShadowMap:
            // Texture2DArrayはSourceTexture(t0、Texture2D)へバインドできないため、専用の
            // DebugArrayTexture(t2)を表示スライス指定付きでサンプルする(IBLキューブマップの
            // Mode 9と同じ方式。Present.hlsl参照)
            presentDebugArrayTexture = targets->ShadowCascadeArray.get();
            presentMode = 10;
            presentSourceWidth = Rendering::kShadowMapSize;
            presentSourceHeight = Rendering::kShadowMapSize;
            break;
        case DebugView::RTShadow:
            // 可視率(0〜1のスカラー)をそのままグレースケール表示する。RTシャドウを実行していない
            // フレーム(非対応環境・手法がRaytraced以外)はテクスチャの中身が意味を持たないため、
            // 最終結果のまま何も切り替えない
            if (raytracedShadowRuns)
            {
                presentSourceTexture = targets->RTShadowTexture.get();
                presentMode = 5;
            }
            break;
        case DebugView::SSR:
            // 反射がOffのときは反射パスをスキップしているため、Tonemapパスの入力もSceneColorになり
            // 結果的にFinalと同一表示になる(SSR / RT反射のどちらでも同じ扱い)
            presentSourceTexture = targets->TonemapTexture.get();
            break;
        case DebugView::HiZ:
            presentSourceTexture = targets->HiZTexture.get();
            presentMode = 6; // 指定ミップをSampleLevelで読みグレースケール表示
            presentSourceWidth = std::max(1u, renderWidth >> frame.Settings.DebugView.HiZDebugMipLevel);
            presentSourceHeight = std::max(1u, renderHeight >> frame.Settings.DebugView.HiZDebugMipLevel);
            break;
        case DebugView::IBLIrradiance:
            // 本物のTextureCubeのため、SourceTexture(t0、Texture2D)ではなくDebugCubeTexture(t1)を
            // 現在のカメラ視線方向でサンプルする(Present.hlsl Mode 9、presentDebugCubeTexture参照)
            presentDebugCubeTexture = irradianceTexture;
            presentMode = 9;
            presentSourceWidth = renderWidth;
            presentSourceHeight = renderHeight;
            break;
        case DebugView::IBLPrefilter:
            presentDebugCubeTexture = prefilteredEnvTexture;
            presentMode = 9;
            presentSourceWidth = renderWidth;
            presentSourceHeight = renderHeight;
            break;
        case DebugView::ProbePrefilter:
            presentDebugCubeArrayTexture = gi->ProbePrefilteredArray.get();
            presentMode = 12;
            break;
        case DebugView::ProbeInfluence:
            // 塗り分けはDeferredLighting.hlsl側(FrameConstants.ProbeParams.y)で行うため、
            // Presentは通常どおり最終結果を表示するだけでよい
            presentSourceTexture = targets->TonemapTexture.get();
            break;
        case DebugView::ProbeDistance:
            // 距離キューブ(19.12節)。格納値はワールド距離なので専用のMode 13でGain倍して
            // グレースケール表示する(Mode 12でそのまま出すと数メートルで白飛びする)
            presentDebugCubeArrayTexture = gi->ProbeDistanceArray.get();
            presentMode = 13;
            break;
        case DebugView::IBLBRDFLUT:
            presentSourceTexture = brdfLUTTexture;
            presentMode = 0; // (A, B, Eavg)の生値をそのままRGBとして表示(値域はおおむね[0,1])
            presentSourceWidth = kIBLBRDFLUTSize;
            presentSourceHeight = kIBLBRDFLUTSize;
            break;
        case DebugView::Bloom:
            // ピラミッド最上段(半解像度、HDR)。Mode 4でトーンマッピングしてから表示する
            if (!targets->BloomUpTextures.empty())
            {
                presentSourceTexture = targets->BloomUpTextures[0].get();
                presentMode = 4;
                presentSourceWidth = targets->BloomLevelSizes[0].x;
                presentSourceHeight = targets->BloomLevelSizes[0].y;
            }
            break;
        case DebugView::LightTiles:
            // ライトグリッドは構造化バッファなのでSourceTexture(t0)では受け取れず、専用のt3から読む
            // (Present.hlsl Mode 11)。t0には何かをバインドしておく必要があるため、
            // 解像度だけ合わせてRenderTargets::TonemapTextureをそのまま渡す(Mode 11では読まれない)
            presentSourceTexture = targets->TonemapTexture.get();
            presentMode = 11;
            break;
        case DebugView::MegaLightsAverage:
            // 蓄積した平均。1フレームも足していないうちは中身が未定義なので切り替えない
            if (m_Engine.m_MegaLightsAccumFrames > 0u && m_Engine.m_MegaLightsAccumBuffer)
            {
                presentSourceTexture = targets->TonemapTexture.get();
                presentMode = 22;
            }
            break;
        case DebugView::MegaLightsTilePool:
            // 候補プールも構造化バッファなのでt3から読む(Present.hlsl Mode 21)。t0の扱いは
            // Mode 11と同じ。パスが走っていないフレームは中身が前フレーム/未定義の残骸なので、
            // 最終結果のまま何も切り替えない(他のMegaLights系の表示と同じ方針)
            if (megaLightsRuns && m_Engine.m_MegaLightsTilePoolBuffer)
            {
                presentSourceTexture = targets->TonemapTexture.get();
                presentMode = 21;
            }
            break;
        case DebugView::BentNormal:
            // bent normal(34章)。正規化しないベクトルなので、法線表示(Mode 7)のような
            // オクタヘドラルのデコードは通さず専用のModeで扱う。
            // 【15ではなく19】15はDDGIのイラディアンスアトラスが使っている。Present.hlslの
            // PSMainではそちらの分岐が先にreturnするため、15を割り当てるとbent normalの
            // 表示へ到達できない(Present.hlsl冒頭のMode一覧を参照)
            presentSourceTexture = targets->GBufferBentNormal.get();
            presentMode = 19;
            break;
        case DebugView::MotionVector:
            // 速度バッファ。格納値はUV単位(1画素ぶんの移動で1/解像度、1920幅なら約0.0005)と
            // 極端に小さく、そのまま色として出しても真っ黒にしか見えない。専用のMode 14で
            // ピクセル単位へ換算してから中間灰色を原点に色付けする
            presentSourceTexture = targets->GBufferVelocity.get();
            presentMode = 14;
            break;
        case DebugView::SceneColorRaw:
            // トーンマップもガンマも通さないリニア値をそのまま出す。スペキュラのエネルギー補正の
            // 各方式を数値で突き合わせるための測定用(14.9.9節)。
            // バックバッファが8bit UNormのため1.0を超える値はクリップする ―― 測定時は
            // EV100を上げてピークが1.0未満に収まるようにしてから読むこと。
            // TAAが有効な場合、hdrSceneColorはTAAの蓄積結果(23章)を指す。静止して収束させれば
            // ジッターの平均が取れたぶん単フレームより安定した値が読めるが、カメラを動かした
            // 直後の数フレームは履歴が混ざっているため、値を読むのは静止させてから
            presentSourceTexture = bb.HdrSceneColor;
            presentMode = 0;
            break;
        case DebugView::DDGIIrradiance:
        case DebugView::DDGIDistance:
        case DebugView::DDGIProbeBackface:
        {
            // アトラスはただのTexture2Dなのでt0でそのまま受け取れる(22章)。
            // 反射プローブのキューブと違い専用スロットは要らない。
            // アトラスは横長(列=Cx*Cy、行=Cz)なので、レターボックスがその比率に合うよう
            // 実寸を渡す。渡さないと画面いっぱいへ引き伸ばされ、セルが正方形に見えなくなる
            // 裏面率はイラディアンスアトラスのαなので、資源も寸法もイラディアンスと同じ
            const bool isIrradiance =
                (frame.Settings.DebugView.View == DebugView::DDGIIrradiance || frame.Settings.DebugView.View == DebugView::DDGIProbeBackface);
            const uint32_t cell = isIrradiance ? kDDGIIrradianceCell : kDDGIDistanceCell;
            const uint32_t columns = gi->GIVolume.ProbeCounts[0] * gi->GIVolume.ProbeCounts[1];
            const uint32_t rows = gi->GIVolume.ProbeCounts[2];

            presentSourceTexture = isIrradiance ? gi->DDGIIrradianceAtlas.get() : gi->DDGIDistanceAtlas.get();
            // Present.hlslのMode 14はモーションベクター(TAA、23章)が既に使っているため、
            // DDGIのイラディアンス/距離モーメントはMode 15/16にずらしてある
            presentMode = (frame.Settings.DebugView.View == DebugView::DDGIProbeBackface) ? 20 : (isIrradiance ? 15 : 16);
            presentSourceWidth = gi->HasGIVolume ? columns * cell : cell;
            presentSourceHeight = gi->HasGIVolume ? rows * cell : cell;
            break;
        }
        case DebugView::WaterMask:
            // G-BufferのMaterial.a(水面のマテリアルID)をそのままグレースケール表示する。
            // 0/1の二値なのでMode 3(Gain倍する遮蔽率表示)ではなく専用のMode 17を使う
            presentSourceTexture = targets->GBufferMaterial.get();
            presentMode = 17;
            break;
        case DebugView::PlanarReflection:
            // 平面反射パスの出力。パスが今フレーム実行されていない(無効化・水面なし)場合、
            // m_RenderTargets.PlanarReflectionColorの中身は前フレーム/未定義の残骸なので最終結果のまま何も
            // 切り替えない(RTShadowデバッグ表示と同じ方針)
            if (frame.PlanarReflectionPassRuns)
            {
                // HDRのためMode 4でReinhardトーンマッピング+ガンマ補正して表示する
                // (DirectLight/Bloomと同じ扱い)。専用のMode追加は不要でPresent.hlslは無変更のまま使える
                presentSourceTexture = targets->PlanarReflectionColor.get();
                presentMode = 4;
                presentSourceWidth = targets->PlanarReflectionWidth;
                presentSourceHeight = targets->PlanarReflectionHeight;
            }
            break;
        case DebugView::AtmosphereLUT:
            // 大気散乱のLUT。HDRなのでMode 4(Reinhard+ガンマ)で表示する。
            // Transmittanceは0〜1なのでそのままでも読めるが、MultiScatteringは値が小さいので
            // 表示輝度の倍率と併用する
            if (frame.Settings.Sky.AtmosphereLUTDebugIndex == 1)
            {
                presentSourceTexture = multiScatteringLUT;
                presentSourceWidth = kMultiScatteringLUTSize;
                presentSourceHeight = kMultiScatteringLUTSize;
            }
            else if (frame.Settings.Sky.AtmosphereLUTDebugIndex == 2)
            {
                presentSourceTexture = skyViewLUT;
                presentSourceWidth = kSkyViewLUTWidth;
                presentSourceHeight = kSkyViewLUTHeight;
            }
            else
            {
                presentSourceTexture = transmittanceLUT;
                presentSourceWidth = kTransmittanceLUTWidth;
                presentSourceHeight = kTransmittanceLUTHeight;
            }
            presentMode = 4;
            break;
        case DebugView::SoftwareRaster:
        case DebugView::SoftwareRasterDepth:
        case DebugView::SoftwareRasterNormal:
            // 自前ソフトウェアラスタライザ(46章)の出力。パスが今フレーム実行されていない場合、
            // バッファの中身は前フレーム/未定義の残骸なので最終結果のまま何も切り替えない
            // (PlanarReflection・RTShadowのデバッグ表示と同じ方針)。
            //
            // 【Modeはハードウェア側と同じものを使う】深度はDebugView::DepthRawと同じMode 5、
            // 法線はDebugView::Normalと同じMode 7。並べて差分を取るのが目的なので、
            // 表示側の処理まで完全に一致させる。Present.hlslは無変更のまま使える
            if (bb.SoftwareRasterPassRuns)
            {
                if (frame.Settings.DebugView.View == DebugView::SoftwareRasterDepth)
                {
                    presentSourceTexture = targets->SoftwareRasterDepth.get();
                    presentMode = 5;
                }
                else if (frame.Settings.DebugView.View == DebugView::SoftwareRasterNormal)
                {
                    presentSourceTexture = targets->SoftwareRasterNormal.get();
                    presentMode = 7;
                }
                else
                {
                    // フラット陰影はHDRのためMode 4(Reinhard+ガンマ)
                    presentSourceTexture = targets->SoftwareRasterColor.get();
                    presentMode = 4;
                }
            }
            break;
        case DebugView::CloudNoiseSlice:
        {
            // 雲の3Dノイズ。形状(128^3)とディテール(32^3)を切り替えて任意のスライスを見る。
            // 正方形のテクスチャなので表示も正方形にする(レターボックスの計算に渡す)
            const bool showDetail = frame.Settings.Cloud.NoiseDebugShowDetail;
            presentDebugVolumeTexture =
                showDetail ? cloudDetailNoiseTexture : cloudShapeNoiseTexture;
            presentMode = 18;
            const uint32_t size = showDetail ? kCloudDetailNoiseSize : kCloudShapeNoiseSize;
            presentSourceWidth = size;
            presentSourceHeight = size;
            break;
        }
        }

        // Mode 11(ライトグリッド)とMode 21(MegaLightsの候補プール)はどちらもt3の構造化バッファを
        // 読むが、1タイルぶんの要素数が違う。バッファと容量は必ず対で切り替えること
        // (片方だけ切り替えると、正しいバッファを別のストライドで読んで無関係な値をヒートマップにする)
        const bool presentUsesTilePool = (presentMode == 21) && m_Engine.m_MegaLightsTilePoolBuffer != nullptr;
        RHI::IRHIBuffer* const presentTileBuffer =
            presentUsesTilePool ? m_Engine.m_MegaLightsTilePoolBuffer.get() : m_Engine.m_LightTileBuffer.get();
        const uint32_t presentTileCapacity =
            presentUsesTilePool ? static_cast<uint32_t>(frame.Settings.MegaLights.TilePoolCapacity) : kLightTileCapacity;
        // Mode 21だけは候補プールを書いた有効タイル幅を使う。Mode 11は従来のライトグリッドなので
        // m_LightTileCountXのままにし、デバッグ表示が実データと別の添字を読まないようにする
        const uint32_t presentTileCountX =
            presentUsesTilePool ? frame.MegaLightsEffectiveTilesX : m_Engine.m_LightTileCountX;

        PresentConstants presentConstants{};
        presentConstants.Mode = presentMode;
        presentConstants.TileParams =
        {
            static_cast<float>(presentTileCountX),
            static_cast<float>(kLightTileSize),
            static_cast<float>(presentTileCapacity),
            // ヒートマップで赤に振り切る基準のライト数。容量そのものを基準にすると
            // 実データ(数灯)ではほぼ真っ青で差が読めないため、別のつまみにしてある
            static_cast<float>(std::max(1, frame.Settings.DebugView.LightTileHeatmapMax)),
        };
        presentConstants.TileRenderSize =
        {
            static_cast<float>(renderWidth),
            static_cast<float>(renderHeight),
            presentUsesTilePool ? static_cast<float>(frame.MegaLightsTileOffset.x) : 0.0f,
            presentUsesTilePool ? static_cast<float>(frame.MegaLightsTileOffset.y) : 0.0f,
        };
        // Mode 22(蓄積平均)が割る数。0で割らないよう下限1
        presentConstants.AccumParams =
        {
            static_cast<float>(std::max(1u, m_Engine.m_MegaLightsAccumFrames)),
            0.0f,
            0.0f,
            0.0f,
        };
        if (frame.Settings.DebugView.View == DebugView::IBLPrefilter)
        {
            presentConstants.MipLevel = static_cast<float>(frame.Settings.IBL.PrefilterDebugMipLevel);
        }
        else if (frame.Settings.DebugView.View == DebugView::IBLIrradiance)
        {
            presentConstants.MipLevel = 0.0f; // イラディアンスマップは常に1ミップのみ
        }
        else if (frame.Settings.DebugView.View == DebugView::ProbePrefilter)
        {
            presentConstants.MipLevel = static_cast<float>(frame.Settings.ReflectionProbe.PrefilterDebugMipLevel);
        }
        else
        {
            presentConstants.MipLevel = static_cast<float>(frame.Settings.DebugView.HiZDebugMipLevel);
        }
        // ArraySliceはMode 10ではカスケード番号、Mode 12ではプローブ番号として使う。
        // プローブが1つも無い場合でも配列の範囲外を引かないようクランプする
        if (frame.Settings.DebugView.View == DebugView::ProbePrefilter || frame.Settings.DebugView.View == DebugView::ProbeDistance)
        {
            presentConstants.ArraySlice = static_cast<float>(
                std::clamp(frame.Settings.ReflectionProbe.DebugIndex, 0, std::max(0, static_cast<int32_t>(m_Engine.m_ReflectionProbes.size()) - 1)));
        }
        else if (frame.Settings.DebugView.View == DebugView::CloudNoiseSlice)
        {
            // Mode 18ではW座標(0〜1)として使う。3Dテクスチャなので配列番号ではなく連続値
            presentConstants.ArraySlice = std::clamp(frame.Settings.Cloud.NoiseDebugSlice, 0.0f, 1.0f);
        }
        else
        {
            presentConstants.ArraySlice =
                static_cast<float>(std::clamp(frame.Settings.Shadow.DebugCascade, 0, static_cast<int32_t>(Rendering::kCascadeCount) - 1));
        }
        // Finalの見た目は倍率の影響を受けてはならないため、デバッグ表示のときだけ倍率を掛ける
        // (Gainはゼロ初期化のままだと0倍=真っ黒になるので、必ず明示的に設定すること)
        if (frame.Settings.DebugView.View == DebugView::ProbeDistance || frame.Settings.DebugView.View == DebugView::DDGIDistance)
        {
            // 距離は色ではなくワールド距離なので、Debug View Gain(1倍以上)ではなく
            // 「白になる距離」の逆数を渡す。Present.hlsl Mode 13/15の式は他と同じ「値×Gain」のまま。
            // DDGI側は距離がMaxRayDistanceでクランプされているので、そこを白にすると
            // 「クランプに当たっている方向」が一目で分かる
            const float whiteAt = (frame.Settings.DebugView.View == DebugView::DDGIDistance)
                ? gi->GIVolume.MaxRayDistance
                : frame.Settings.ReflectionProbe.DistanceDebugRange;
            presentConstants.Gain = 1.0f / std::max(whiteAt, 0.01f);
        }
        else if (frame.Settings.DebugView.View == DebugView::DDGIIrradiance)
        {
            // アトラスは露出非依存の物理量で持っている(FrameConstants::DDGIParams4 参照)ため、
            // そのまま出すと昼は数万倍の値になって白飛びする。表示だけ実効プリ露出を掛けて
            // 他のバッファと同じ表示レンジへ揃える。こうしておくと
            // 「IBL - イラディアンス」の表示と直接見比べられる(22.9.1節の検証がこれに依存している)
            presentConstants.Gain = frame.Settings.DebugView.Gain * frame.EffectiveExposure;
        }
        else
        {
            presentConstants.Gain = (frame.Settings.DebugView.View == DebugView::Final) ? 1.0f : frame.Settings.DebugView.Gain;
        }
        commandList->UpdateBuffer(m_Engine.m_PresentConstantBuffer.get(), &presentConstants, sizeof(presentConstants));

        // レターボックス/ピラーボックスの余白もクリア色のまま残るよう、絞ったビューポートで描画する
        const RHI::Viewport letterboxViewport = ComputeLetterboxViewport(
            frame.WindowWidth, frame.WindowHeight, presentSourceWidth, presentSourceHeight);

        // グラフィックスデバッガ向けの名前を焼く。**フレームの記録とは独立**なので
        // レンダーグラフへは積まず、ここで直接呼ぶ(ID3D12Object::SetNameはコマンドではない)。
        // 立っているのは起動直後とレンダーターゲットを作り直した直後だけ
        if (m_Engine.m_DebugNamesDirty)
        {
            m_Engine.ApplyDebugNames();
            m_Engine.m_DebugNamesDirty = false;
        }

        // 【Presentより前に積む】書き出す対象は中間バッファなので、Presentの後ろに置く理由が無い。
        // Readsで書き手より後に順序付くので、この位置に積めば「そのフレームの最終的な中身」が取れる
        m_Engine.IssueTextureDumps(graph);

        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "Present",
            .Reads = { presentSourceTexture, presentDebugCubeTexture, presentDebugArrayTexture,
                       presentDebugCubeArrayTexture, presentDebugVolumeTexture },
            // DebugView::LightTilesでライトグリッドを、DebugView::MegaLightsTilePoolで候補プールを
            // 読むため、それぞれの書き手より後に順序付ける(表示していないフレームでも
            // 同じポインタになるだけで無害)
            .BufferReads = { m_Engine.m_LightTileBuffer.get(), presentTileBuffer, m_Engine.m_MegaLightsAccumBuffer.get() },
            .SwapChainTarget = frame.SwapChain,
            .Execute = [this, letterboxViewport, presentSourceTexture, presentDebugCubeTexture, presentDebugArrayTexture, presentDebugCubeArrayTexture, presentDebugVolumeTexture, presentTileBuffer, frameConstantBuffer, screenSpaceSamplers](RHI::IRHICommandList* cmd)
            {
                cmd->ClearRenderTarget({ 0.05f, 0.05f, 0.08f, 1.0f });
                cmd->ClearDepth(1.0f);
                cmd->SetViewport(letterboxViewport);

                cmd->SetPipelineState(m_Engine.m_PresentPipelineState.get());
                cmd->SetConstantBuffer(0, frameConstantBuffer);
                cmd->SetConstantBuffer(1, m_Engine.m_PresentConstantBuffer.get());
                cmd->SetSamplerSet(screenSpaceSamplers);
                cmd->SetTexture(0, presentSourceTexture);
                cmd->SetTexture(1, presentDebugCubeTexture);
                cmd->SetTexture(2, presentDebugArrayTexture);
                // Mode 11(ライトグリッド)/ Mode 21(候補プール)以外でも、シェーダが宣言している
                // リソースは必ずバインドする(SetPipelineStateが毎回ルート引数を無効化するため)
                cmd->SetShaderResourceBuffer(3, presentTileBuffer);
                // Mode 22(蓄積平均)専用。読まれないModeでも必ずバインドする(上と同じ理由)
                cmd->SetShaderResourceBuffer(6, m_Engine.m_MegaLightsAccumBuffer.get());
                cmd->SetTexture(4, presentDebugCubeArrayTexture);
                cmd->SetTexture(5, presentDebugVolumeTexture);
                cmd->Draw(3, 0);
            },
        });
    }
}
