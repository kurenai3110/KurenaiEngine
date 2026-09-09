#include "../KurenaiEngine3D.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <exception>
// std::begin / std::end (m_ModelCullRegionIssued の一括ゼロ埋め)
#include <iterator>
#include <string>
#include <vector>

#include "Core/CPUProfiler.h"
#include "Core/Logger.h"
// パス群のRegisterとカウンタと履歴の反転を呼ぶため、前方宣言では足りない
#include "../Passes/DDGIPasses.h"
#include "../Passes/EnvironmentPasses.h"
#include "../Passes/GeometryPasses.h"
#include "../Passes/LightingPasses.h"
#include "../Passes/MegaLightsPasses.h"
#include "../Passes/PostProcessPasses.h"
#include "../Passes/PresentPass.h"
#include "../Passes/ReflectionPasses.h"
#include "../Passes/ReflectionProbePasses.h"
#include "../Passes/ShadowPasses.h"
#include "../ShaderInterop/FrameConstants.h"
#include "../UI/UIManager.h"
#include "../UI/UITheme.h"
#include "CubeFaceMath.h"
#include "RenderBlackboard.h"
#include "RenderFrameContext.h"
#include "SunLighting.h"

// Render()から切り出したフレームの先頭(Aブロック)・パスの登録(Dブロック)・
// 締め(Eブロック)。段階6.6。
// KurenaiEngine3D のメンバ関数のまま、翻訳単位だけをここへ分けている
// (宣言は KurenaiEngine3D.h のまま。Diagnostics/RenderDumpService.cpp と同じ作法)。
//
// **定義の並びは Render() から呼ぶ順に合わせてある。** 呼ぶ順が実行順の一部なので、
// 追うときはこの順で読めるようにしておく
namespace Kurenai
{
    void KurenaiEngine3D::ResetFrameCounters()
    {
        // フラスタムカリングの統計はフレーム単位。ここで0に戻し、各描画パスが積み上げる。
        // モデル単位とメッシュ単位は別のカウンタで、混ぜない(KurenaiEngine3D.h参照)。
        //
        // 【0に戻す前に前フレームの値を控える】ドローコール数と同じ理由で、UIパネルは
        // Renderの外で描かれるため現在のカウンタを読むと必ずリセット直後の0になる
        m_RenderStats.FrustumCullTestedLastFrame = m_FrustumCullTested;
        m_RenderStats.FrustumCullCulledLastFrame = m_FrustumCullCulled;
        m_RenderStats.MeshCullTestedLastFrame = m_MeshCullTested;
        m_RenderStats.MeshCullCulledLastFrame = m_MeshCullCulled;
        m_FrustumCullTested = 0;
        m_FrustumCullCulled = 0;
        m_MeshCullTested = 0;
        m_MeshCullCulled = 0;
        // ドローコール数も同じくフレーム単位。各パスが自分のカウンタを積み上げる。
        //
        // 【0に戻す前に前フレームの値を控える】UIパネルはRenderの外で描かれるため、
        // 現在のカウンタを読むと必ずリセット直後の0になる(実際にそう表示されていた)。
        // 完成した最後のフレームの値を別に持たせる
        m_RenderStats.DrawCallsGBufferLastFrame = m_GeometryPasses->GetDrawCallsGBuffer();
        m_RenderStats.DrawCallsShadowLastFrame = m_ShadowPasses->GetDrawCalls();
        m_RenderStats.DrawCallsDepthPrepassLastFrame = m_GeometryPasses->GetDrawCallsDepthPrepass();
        m_GeometryPasses->ResetDrawCalls();
        m_ShadowPasses->ResetDrawCalls();
        // bindless区画の使用数を控える(UIパネルは m_Device へ直接触れないため。
        // m_RenderCapabilities.MeshShaderAvailable と同じ扱い)。登録はシーン読み込み時にしか起きないので、
        // フレームごとに1回問い合わせるだけで足りる
        m_RenderStats.BindlessUsedCount = m_Device ? m_Device->GetBindlessUsedCount() : 0;
    }

    void KurenaiEngine3D::ApplyPendingRecreations()
    {
        // WM_SIZE(Updateスレッド)が記録しておいたリサイズ要求を、スワップチェーンを実際に使う
        // このスレッドで反映する。このフレームのGPUコマンドをまだ1つも積んでいないこの位置で
        // 呼ぶこと(DX12SwapChain::Resizeは内部でWaitForGPUIdleを呼び、コマンドリストが
        // 記録待ちの状態であることを前提としているため)
        ApplyPendingResize();

        // ここでは要求だけを積み、実際の作り直しは従来どおり後段のUpdateSceneStreamingと
        // バッファ精度/解像度の作り直しブロックへ集約する。作り直しの契機を散らさないためこの位置に置く。
        ApplyScheduledRecreations();

        // Loaderスレッドが出来上がったシーンを置いていれば取り込み、保留中の切り替え要求があれば発注する。
        // 旧シーンの破棄(WaitForGPUIdleを伴う)もここで行うため、このフレームのGPUコマンドを
        // まだ1つも積んでいないこの位置で呼ぶこと
        UpdateSceneStreaming();

        // テクスチャの常駐ミップ差し替えを確定する。
        // **このフレームで最初にSetTextureを呼ぶより前でなければならない** ――
        // DX12CommandList::SetTextureは描画を記録するたびにテクスチャのSRVディスクリプタを
        // コピー元として読むため、記録が始まってから書き換えると読みながら書くことになる
        // (詳細はIRHIDevice::PrepareTextureContentsのコメント)
        m_TextureStreaming.CommitReady(*m_Device);
    }

    void KurenaiEngine3D::BeginImGuiFrame(const KurenaiEngine3D::FrameState& frameState)
    {
        // WndProc(Updateスレッド)でキューイングされたメッセージを、ImGuiの状態を実際に読み書きする
        // このRenderスレッド自身からImGui_ImplWin32_WndProcHandlerへ転送する。ImGui::NewFrame()より前に
        // 行うことで、このフレームのNewFrame()が最新のマウス/キーボード状態を反映できる
        m_Window->ForwardQueuedMessagesToImGui();

        // モニタの拡大率に合わせてUIの大きさを揃える。ImGuiの状態を触るのはこのRenderスレッド
        // だけという不変条件を守るため、Window側は値をatomicへ置くだけにし、
        // 実際のスタイル再適用はここで行う
        m_UIManager->OnUIScaleChanged(m_Window->GetDpiScale() * UI::UITheme::kUIScaleMultiplier);

        m_ImGuiBackend->NewFrame();
        if (frameState.ImGuiVisible)
        {
            UI::PanelDrawContext panelContext;
            panelContext.Camera = &frameState.Camera;
            m_UIManager->Draw(panelContext);

            // オーサリングツール(Tools/KurenaiShowEditor)が足す追加のUI。
            // 【ImGuiVisibleの中に置くこと】F1で全パネルを隠したときに、これだけが
            // 画面に残ってしまうとスクリーンショットでの計測が壊れる
            if (m_ExtraImGuiCallback)
            {
                m_ExtraImGuiCallback();
            }
        }

        // ImGuiがマウス/キーボードを掴んでいるかをUpdateスレッドへ返す(Update()が読む)。
        // パネル非表示のときは掴んでいないので明示的にfalseを書く
        {
            const ImGuiIO& io = ImGui::GetIO();
            m_ImGuiWantCaptureKeyboard.store(
                frameState.ImGuiVisible && io.WantCaptureKeyboard, std::memory_order_relaxed);
            m_ImGuiWantCaptureMouse.store(frameState.ImGuiVisible && io.WantCaptureMouse, std::memory_order_relaxed);
        }
    }

    void KurenaiEngine3D::RecreateDirtyRenderTargets()
    {
        // バッファ精度(デバッグ表示パネルのラジオボタン)と内部レンダー解像度(システムパネル)の
        // 切り替え要求をここで処理する。
        // レンダーターゲットを破棄する前に、DX12がまだ実行中かもしれない直前数フレームの
        // 描画コマンドを完了させる必要がある(LoadSceneがGPUリソースを破棄する前に
        // WaitForGPUIdleを呼ぶのと同じ理由)。このフレームのGPUコマンドはまだ1つも
        // 積んでいないため、ここで待っても待ち時間は前フレームぶんだけで済む。
        //
        // ここはApplyPendingResizeの後、かつこのフレームでm_RenderWidth/m_RenderHeightを
        // 読み始めるより前(最初の読み取りはTAAジッター)なので、解像度をまとめて差し替えてよい
        if (m_BufferPrecisionDirty || m_RenderResolutionDirty || m_PlanarReflectionResolutionDirty ||
            m_UpscaleTargetsDirty || m_MegaLightsReservoirDirty)
        {
            const bool precisionChanged = m_BufferPrecisionDirty;
            m_BufferPrecisionDirty = false;
            // MegaLightsのリザーババッファは CreateRenderTargets の中で作り直される。
            // 標本数の変更だけでもここを通す(GPUが参照していない状態が要るため)
            m_MegaLightsReservoirDirty = false;

            const uint32_t previousWidth = m_RenderWidth;
            const uint32_t previousHeight = m_RenderHeight;
            if (m_RenderResolutionDirty)
            {
                m_RenderResolutionDirty = false;
                m_RenderWidth = m_PendingRenderWidth;
                m_RenderHeight = m_PendingRenderHeight;
            }
            if (m_PlanarReflectionResolutionDirty)
            {
                m_PlanarReflectionResolutionDirty = false;
                m_Settings.Reflection.PlanarResolutionScale = m_PendingPlanarReflectionResolutionScale;
            }

            m_Device->WaitForGPUIdle();
            try
            {
                CreateRenderTargets(m_RenderWidth, m_RenderHeight);
                // 平面反射専用のレンダーターゲットも、呼び出し箇所をCreateRenderTargetsと
                // 揃えてここで作り直す(反射解像度の倍率変更だけの要求でもここを通る)
                CreatePlanarReflectionTargets();
            }
            catch (const std::exception& e)
            {
                // CreateRenderTargets自身がHDR→Legacy8bitのフォールバックを持つため、ここへ来るのは
                // 要求した解像度そのものが確保できない場合(高解像度でのVRAM不足など)。
                // 元の解像度へ戻して作り直す。それも失敗するなら復旧手段が無いのでそのまま送出する
                Core::Logger::Error(
                    "KurenaiEngine3D",
                    "内部レンダー解像度" + std::to_string(m_RenderWidth) + "x" + std::to_string(m_RenderHeight) +
                        "のレンダーターゲット作成に失敗したため、" + std::to_string(previousWidth) + "x" +
                        std::to_string(previousHeight) + "へ戻します: " + e.what());
                m_RenderWidth = previousWidth;
                m_RenderHeight = previousHeight;
                CreateRenderTargets(m_RenderWidth, m_RenderHeight);
                CreatePlanarReflectionTargets();
            }

            // 超解像の出力解像度用テクスチャ。内部解像度用とは作り直す契機が違うため
            // CreateRenderTargetsとは別に持っているが、GPUがそれらを参照していない状態で
            // 作り直す必要があるのは同じなので、上のWaitForGPUIdle()の後のここで行う
            if (m_UpscaleTargetsDirty)
            {
                m_UpscaleTargetsDirty = false;
                try
                {
                    CreateUpscaleTargets(m_Settings.PostProcess.UpscaleOutputWidth, m_Settings.PostProcess.UpscaleOutputHeight);
                }
                catch (const std::exception& e)
                {
                    // 確保できなければ超解像を諦めて等倍表示へ落とす。内部解像度は下がったままだが、
                    // Presentがバイリニアで拡大するので絵は出続ける(41.23節以前と同じ経路)
                    Core::Logger::Error(
                        "KurenaiEngine3D",
                        "超解像の出力解像度" + std::to_string(m_Settings.PostProcess.UpscaleOutputWidth) + "x" +
                            std::to_string(m_Settings.PostProcess.UpscaleOutputHeight) +
                            "のテクスチャ作成に失敗したため、超解像を無効にします: " + e.what());
                    m_Settings.PostProcess.UpscaleEnabled = false;
                    m_RenderTargets.ResetUpscale();
                }
            }

            // カメラのアスペクト比はUpdateスレッドが読み取って反映する(m_RenderAspectの宣言参照)
            m_RenderAspect.store(
                static_cast<float>(m_RenderWidth) / static_cast<float>(m_RenderHeight), std::memory_order_relaxed);

            // PSOはレンダーターゲットのフォーマットだけに依存し解像度には依存しないため、
            // 作り直すのは精度が変わったときだけでよい。
            // G-Buffer(Emissive)とAO/GIのフォーマットが変わるため、それらへ描くPSOも作り直す。
            // 作り直さないとPSOが宣言するRenderTargetFormatsと実際のRTVがずれ、D3D12では
            // 仕様違反になる(DX11は検証しないため露見しない)
            if (precisionChanged)
            {
                CreatePrecisionDependentPipelineStates();
            }
        }
    }

    void KurenaiEngine3D::UpdateEffectiveExposure(float keyIlluminanceLux)
    {
        // === 可変プリ露出の決定 ===
        // 昼(直射日光10万lx)を基準0として、そのフレームのキー照度が何段暗いかを求め、
        // ユーザー設定のEV100へ足す。これによりHDRバッファへ流れる値のレンジが
        // 昼でも夜でもおおむね一定に保たれ、夜がfp16でつぶれなくなる
        // (詳細な理由はm_EffectiveExposureEV100の宣言コメント)。
        // 露出はTonemap/Bloom/AutoExposureが同じ値で割り戻すため、これを動かしても絵は変わらない
        {
            const float keyIlluminance = std::max(keyIlluminanceLux, 1e-6f);
            const float autoBias = std::log2(keyIlluminance / kSunIlluminanceLux);
            // 【下限-12段の根拠 ― 表示レンジの両端をfp16に収める】
            // このバイアスは「HDRバッファの値」と「トーンマップが受け取る表示値」の橋渡しで、
            //   表示値 = バッファの値 × 2^bias
            // という関係にある。したがってバッファの上限(fp16の65504)と下限(最小正規化数6.1e-5)は、
            // そのまま**表示できる明るさの上限と下限**になる。
            //
            //   上側: 表示16(ACESの白より十分上。ここまで出せれば発光物が白へ振り切れる)を
            //         表すには 65504 × 2^bias >= 16 → bias >= -12
            //   下側: 表示1e-4(sRGBで1階調にも満たない=見えない)がfp16の正規化域に残るには
            //         1e-4 × 2^-bias >= 6.1e-5 → bias <= 0.7
            //
            // よって-12が両立点になる。**要件が表示値で書けているので、シーンのExposureにも
            // その夜の照度にも依存しない**のがこの値の性質である。
            //
            // 【かつて-18だった理由と、それが上限を潰していたこと】
            // 元の-18は「照度の差(満月なら-18.34段)をできるだけ打ち消す」という下側だけの
            // 発想で決まっており、上側は見ていなかった。その結果 表示上限が
            // 65504 × 2^-18 = 0.25 に落ち、**夜はどんな発光物も表示0.25(sRGBで132前後)より
            // 明るくできない**状態になっていた。ドローンショーで[DroneShow]Brightnessを
            // 0.30から45へ150倍にしても画素値が1段も動かなかったのはこれが原因で、
            // 上限に張り付いたまま裾だけが飽和して色を失っていた。
            // -12にすると上限は65504 × 2^-12 = 16へ64倍広がり、Brightnessが再び効くようになる
            // (実測: 0.30/1.0/3.0 で編隊の最大画素値が 166/211/237 と動く。-18では全部153だった)。
            //
            // 【絵が変わらないことの保証】Tonemap/Bloom/AutoExposureはいずれもこの値を
            // 割り戻すので、fp16の範囲に収まっている画素は1つも動かない。実測でも
            // 地形・水面・空の画素値は-18のときと完全に一致し、飽和していた機体だけが変わった。
            // 上限0段は「昼より明るくはしない」の意味で従来どおり
            const float targetEV100 = m_Settings.PostProcess.SceneExposureEV100 + std::clamp(autoBias, -12.0f, 0.0f);

            if (!m_EffectiveExposureInitialized)
            {
                // 起動直後・シーン切り替え直後は平滑化せず即座に合わせる
                m_EffectiveExposureEV100 = targetEV100;
                m_EffectiveExposureInitialized = true;
            }
            else
            {
                // 一時停止や巨大なdtで飛ばないよう上限を設ける
                const float deltaTime = std::clamp(m_RenderDeltaTime, 0.0f, 0.1f);
                const float t = std::clamp(1.0f - std::exp(-deltaTime * m_Settings.PostProcess.EffectiveExposureAdaptSpeed), 0.0f, 1.0f);
                m_EffectiveExposureEV100 += (targetEV100 - m_EffectiveExposureEV100) * t;
            }
        }
    }

    void KurenaiEngine3D::ApplyMegaLightsPerturbationIfDue()
    {
        // 【検証専用】蓄積が始まる瞬間に1回だけ摂動を加える。
        // 時間再利用の「追従」(灯を消したら何フレームで消えるか、露出が跳んでも
        // 明るさが暴れないか)は、静止した絵をいくら撮っても測れない。
        // 蓄積ダンプは総和を書くので、Nを変えた2本の差が1フレームぶんになる ――
        // これで追従の時間変化を、フレームごとのGPU読み戻し無しで測れる
        if (m_Settings.MegaLights.PerturbMode != 0 && !m_MegaLightsPerturbApplied && m_Settings.MegaLights.AccumTargetFrames > 0 &&
            m_MegaLightsPasses->GetAccumWarmupFrames() >= Passes::kMegaLightsAccumWarmup)
        {
            m_MegaLightsPerturbApplied = true;
            if (m_Settings.MegaLights.PerturbMode == 1)
            {
                // 全ライトを消す。次フレーム以降のGPULight配列から外れるので、
                // 真値は「ローカルライトの寄与が0」になる。時間再利用が履歴を抱えていると
                // すぐには0にならず、その残り方がゴーストそのもの
                for (Assets::Light& light : m_Lights)
                {
                    light.Enabled = false;
                }
                Core::Logger::Info("KurenaiEngine3D", "【検証】全ライトを消しました(ゴースト測定)");
            }
            else if (m_Settings.MegaLights.PerturbMode == 2)
            {
                // 実効プリ露出を+2段跳ばす。ライトの放射輝度は露出を掛け込んで作られるので、
                // 履歴のWは前フレームの露出のままになる。補正が効いていれば絵は変わらない
                m_EffectiveExposureEV100 += 2.0f;
                Core::Logger::Info(
                    "KurenaiEngine3D", "【検証】実効プリ露出を+2段跳ばしました(プリ露出補正の確認)");
            }
        }
    }

    void KurenaiEngine3D::UpdateSceneForFrame(
        RHI::IRHICommandList* commandList, const DirectX::XMFLOAT3& cameraPosition, const Core::Camera& camera)
    {
        // モデルLODの段を、レンダーグラフを組む前にこの1回だけ決める。
        // 【パスごとに測り直してはいけない】深度プリパスとG-Bufferが違う段を選ぶと、
        // プリパスが深度を書いた画素をG-Bufferが描かず、画面に穴が開く。
        // G-Bufferパスのラムダはそもそもカメラ位置をキャプチャしていない(半透明パスだけが持つ)ので、
        // ここで決めてm_InstanceLODStatesへ置く形にしてある
        UpdateModelLOD(cameraPosition, m_RenderDeltaTime);

        // モデルのストリーミング。【LODの後に呼ぶ】どの段を読むかは選ばれた段で決まる
        UpdateModelStreaming(cameraPosition);

        // インスタンシングのバッチを組み直す。【LODとストリーミングの後】まとめられるかどうかは
        // 「そのフレームに選ばれた段」と「読み込み済みか」で決まる。レンダーグラフの構築より前に
        // 1回だけ呼ぶこと ―― パスごとに組み直すと、深度プリパスとG-Bufferが違うまとめ方をする
        BuildInstanceBatches(commandList);

        // レイトレーシングを常駐の増減へ追随させる(ストリーミング時のみ働く)
        UpdateRaytracingRebuild();

        // テクスチャの常駐ミップの目標を更新し、差のあるものをワーカーへ積む。
        // 実際の差し替えは次フレーム以降のCommitReady(このフレームの先頭で呼んだもの)で確定する。
        // 画面の高さは内部レンダー解像度を使う(ウィンドウ解像度ではない。
        // 内部解像度を下げているときは必要なテクセル密度もその分下がる)
        m_TextureStreaming.UpdateTargets(
            cameraPosition, std::tan(camera.GetFovY() * 0.5f), m_RenderHeight, m_RenderDeltaTime);
    }

    void KurenaiEngine3D::RegisterPasses(
        Core::RenderGraph& graph, Rendering::RenderFrameContext& frameContext,
        Rendering::RenderBlackboard& blackboard, RHI::IRHICommandList* commandList,
        std::vector<RHI::IRHITexture*>& probeCaptureReads, size_t bakedLightCount,
        RHI::IRHITexture* skyTexture, const DirectX::XMMATRIX (&cascadeViewProj)[kCascadeCount],
        const Core::Camera& camera)
    {
        // --- 環境(空・大気・雲・IBL)の焼き込みパス群(段階6で Passes/EnvironmentPasses へ移設) ---
        // 【必ずグラフの先頭で登録すること】依存解決は登録順の前方走査なので、
        // ここより後ろへ動かすとSkyIntegrateが未初期化のLUTを読む
        m_EnvironmentPasses->Register(graph, frameContext, blackboard);

        RHI::Viewport shadowViewport;
        shadowViewport.Width = static_cast<float>(Rendering::kShadowMapSize);
        shadowViewport.Height = static_cast<float>(Rendering::kShadowMapSize);

        RHI::Viewport gbufferViewport;
        gbufferViewport.Width = static_cast<float>(m_RenderWidth);
        gbufferViewport.Height = static_cast<float>(m_RenderHeight);
        frameContext.GBufferViewport = gbufferViewport;
        frameContext.ShadowViewport = shadowViewport;
        frameContext.BakedLightCount = bakedLightCount;

        // プローブのキューブ面キャプチャが使う射影。**反射プローブとDDGIで同じものを使う**
        // ため、どちらの群からも引けるようフレームのスナップショットへ載せる
        const DirectX::XMMATRIX probeFaceProjection =
            ComputeCubeFaceProjection(camera.GetNearZ(), camera.GetFarZ());
        frameContext.ProbeFaceProjection = probeFaceProjection;

        // プローブのキャプチャが読むテクスチャ一式。反射プローブとDDGIが同じ組を読む。
        // 【実体はRender()にある】frameContext.ProbeCaptureReadsがこれを指し、
        // graph.Execute()の時点でも生きている必要があるのでここのローカルにはしない
        probeCaptureReads = {
            m_RenderTargets.ShadowCascadeArray.get(),
            skyTexture, m_IBLResources.IrradianceTexture.get(), m_IBLResources.PrefilteredEnvTexture.get(), m_IBLResources.BRDFLUTTexture.get(),
        };
        frameContext.ProbeCaptureReads = &probeCaptureReads;
        frameContext.CascadeViewProj = cascadeViewProj;

        // --- シャドウのパス群(段階6で Passes/ShadowPasses へ移設) ---
        // 【この位置で登録すること】依存が同点のときRenderGraphは最小登録番号を選ぶ。
        // 登録順そのものが実行順の一部になっている
        m_ShadowPasses->RegisterCascades(graph, frameContext);

        // --- 反射プローブのパス群(段階6で Passes/ReflectionProbePasses へ移設) ---
        // 【この位置で登録すること】依存が同点のときRenderGraphは最小登録番号を選ぶ。
        // 登録順そのものが実行順の一部になっている
        m_ReflectionProbePasses->Register(graph, frameContext, blackboard);

        // --- DDGIのパス群(段階6で Passes/DDGIPasses へ移設) ---
        // 【この位置で登録すること】依存が同点のときRenderGraphは最小登録番号を選ぶ。
        // 登録順そのものが実行順の一部になっている
        m_DDGIPasses->RegisterProbeUpdate(graph, frameContext, blackboard);

        // --- ジオメトリのパス群(段階6で Passes/GeometryPasses へ移設) ---
        // 【この位置で登録すること】依存が同点のときRenderGraphは最小登録番号を選ぶ。
        // 登録順そのものが実行順の一部になっている
        m_GeometryPasses->Register(graph, frameContext, blackboard);

        // --- MegaLightsのパス群(段階6で Passes/MegaLightsPasses へ移設) ---
        // 【この位置で登録すること】依存が同点のときRenderGraphは最小登録番号を選ぶ。
        // 登録順そのものが実行順の一部になっている
        m_MegaLightsPasses->Register(graph, frameContext, blackboard);

        // --- RTシャドウ(段階6で Passes/ShadowPasses へ移設) ---
        m_ShadowPasses->RegisterRaytraced(graph, frameContext);

        // --- 直接光・AO/GI・雲のパス群(段階6で Passes/LightingPasses へ移設) ---
        m_LightingPasses->RegisterDirectAndAO(graph, frameContext, blackboard);

        // --- DDGIの解決(段階6で Passes/DDGIPasses へ移設) ---
        m_DDGIPasses->RegisterResolve(graph, frameContext);

        // --- 合成と半透明のパス群(段階6で Passes/LightingPasses へ移設) ---
        // 【この位置で登録すること】依存が同点のときRenderGraphは最小登録番号を選ぶ
        m_LightingPasses->RegisterSceneLighting(graph, frameContext, blackboard);

        // --- 反射のパス群(段階6で Passes/ReflectionPasses へ移設) ---
        // 【この位置で登録すること】依存が同点のときRenderGraphは最小登録番号を選ぶ。
        // 登録順そのものが実行順の一部になっている
        m_ReflectionPasses->Register(graph, frameContext, blackboard);

        // --- ポストプロセスのパス群(段階6で Passes/PostProcessPasses へ移設) ---
        // 【この位置で登録すること】依存が同点のときRenderGraphは最小登録番号を選ぶ。
        // 登録順そのものが実行順の一部になっている
        m_PostProcessPasses->Register(graph, frameContext, blackboard);

        // --- Present パス群(段階6で Passes/PresentPass へ移設) ---
        // 【この位置で登録すること】RenderGraph は依存が同点のとき最小登録番号を選ぶため、
        // 登録順そのものが実行順の一部になっている。移設で順番が動くと実行順が変わる
        m_PresentPass->Register(graph, commandList, frameContext, blackboard);
    }

    void KurenaiEngine3D::ResolveFrameCullStats(
        const Rendering::RenderBlackboard& blackboard, bool meshletCullStatsActive)
    {
        // --- メッシュレットカリングの統計を読み戻す(Stage 5-2) ---
        //
        // 【GPUを待たない】直前に積んだコピーはまだ実行されていないので、リングの中で
        // **最も古いもの**(kMeshletCullStatsRingSize-1 = 2フレーム前に書いたもの)を読む。
        // DX12はkFrameCount(=2)フレームぶんCPUが先行するため、2フレーム前のGPU実行は
        // 完了している。待ちを入れるとフレームが直列化し、計測のために計測対象を壊す。
        //
        // 【読めなかったフレームは足さない】DX11のMap(DO_NOT_WAIT)はまだ実行中ならfalseを返す。
        // 0として集計に足すと間引き率が実際より低く出るので、そのフレームは丸ごと飛ばす
        if (meshletCullStatsActive)
        {
            const uint32_t oldestIndex = (m_MeshletCullStatsRingIndex + 1) % kMeshletCullStatsRingSize;
            uint32_t counters[Passes::kMeshletCullStatsCount] = {};
            if (m_MeshletCullStatsReadback[oldestIndex] &&
                m_MeshletCullStatsReadback[oldestIndex]->ReadbackData(counters, sizeof(counters)))
            {
                m_RenderStats.MeshletCullTested = counters[0];
                m_RenderStats.MeshletCullFrustumCulled = counters[1];
                m_RenderStats.MeshletCullOcclusionCulled = counters[2];

                m_FrameStatsMeshletTestedSum += m_RenderStats.MeshletCullTested;
                m_FrameStatsMeshletFrustumCulledSum += m_RenderStats.MeshletCullFrustumCulled;
                m_FrameStatsMeshletOcclusionCulledSum += m_RenderStats.MeshletCullOcclusionCulled;
                ++m_FrameStatsMeshletSampleCount;
            }
            m_MeshletCullStatsRingIndex = (m_MeshletCullStatsRingIndex + 1) % kMeshletCullStatsRingSize;
        }
        else
        {
            // 統計を切っている間に古い値が残っていると、UIやログが「今もこの数だけ間引いている」
            // ように見える。切った時点で0へ戻す
            m_RenderStats.MeshletCullTested = 0;
            m_RenderStats.MeshletCullFrustumCulled = 0;
            m_RenderStats.MeshletCullOcclusionCulled = 0;
        }

        // --- モデル単位のGPUカリングの結果を読み戻す(Stage 5-3) ---
        // リングの理由も「読めなかったフレームは足さない」もメッシュレット統計と同じ
        if (blackboard.ModelCullReady)
        {
            // 今フレームのCPU側の結果を、GPUのコピーとまったく同じ位置へ積む。
            // 読むときに同じ位置から取れば、比べるのは同じフレームのもの同士になる
            m_ModelCullCpuFrustumHistory[m_ModelCullRingIndex] = m_GeometryPasses->GetModelCullCpuFrustumCulled();
            // 【比べる相手はG-Bufferぶんの候補数】GPU側の「判定」もそこだけを数えている
            m_ModelCullCandidateHistory[m_ModelCullRingIndex] =
                m_GeometryPasses->GetModelCullCandidateCount()
                - m_GeometryPasses->GetModelCullPrepassCandidateCount();

            const uint32_t oldest = (m_ModelCullRingIndex + 1) % kMeshletCullStatsRingSize;
            uint32_t counters[Passes::kModelCullCounterCount] = {};
            if (m_ModelCullReadback[oldest] &&
                m_ModelCullReadback[oldest]->ReadbackData(counters, sizeof(counters)))
            {
                m_ModelCullTested = counters[0];
                m_ModelCullFrustumCulled = counters[1];
                m_ModelCullOcclusionCulled = counters[2];
                m_ModelCullSurvived = counters[3];
                for (uint32_t region = 0; region < Passes::kModelCullRegionCount; ++region)
                {
                    m_ModelCullRegionIssued[region] = counters[4 + region];
                }
                m_ModelCullComparedCpuFrustumCulled = m_ModelCullCpuFrustumHistory[oldest];
                m_ModelCullComparedCandidateCount = m_ModelCullCandidateHistory[oldest];
            }
            m_ModelCullRingIndex = (m_ModelCullRingIndex + 1) % kMeshletCullStatsRingSize;
        }
        else
        {
            m_ModelCullTested = 0;
            m_ModelCullFrustumCulled = 0;
            m_ModelCullOcclusionCulled = 0;
            m_ModelCullSurvived = 0;
            std::fill(std::begin(m_ModelCullRegionIssued), std::end(m_ModelCullRegionIssued), 0u);
        }
    }

    void KurenaiEngine3D::SubmitAndPresentFrame()
    {
        // ImGuiはPresentパスでバインドされたバックバッファにそのまま重ねて描画する。
        // GPU側は計測していない(このスコープ専用の描画パイプラインを持たないため)が、
        // CPU側のコマンド記録コストはDX11/DX12で差が出やすいのでここも計測しておく
        m_CPUProfiler.BeginScope("ImGui");
        m_ImGuiBackend->Render();
        m_CPUProfiler.EndScope(); // ImGui

        // Present呼び出しでコマンドリストが実行投入される(DX12)ため、それより前にEndFrame()で
        // フレーム終端のタイムスタンプ書き込み・結果リードバックのコマンドを記録しておく必要がある
        m_GPUProfiler->EndFrame();

        // ExecuteCommandLists・実際のPresent・(DX12のみ)フェンス待ちを含む区間。
        // Present呼び出し自体のCPUコストはここで計測しないと、各パスのコマンド記録時間の
        // 合計とCPU Frame Time全体の差分がどこにあるのか分からなくなるため計測しておく
        m_CPUProfiler.BeginScope("PresentSubmit");
        m_SwapChain->Present(m_Settings.System.VSyncEnabled);
        m_CPUProfiler.EndScope(); // PresentSubmit

        // GPUの完了待ち(DX12のフレームパイプライン化に伴うフェンス待ち)は実際のCPU負荷ではなく
        // GPU側の処理時間の反映なので、PresentSubmitの計測値からは除外しておく
        m_CPUProfiler.SubtractFromScope("PresentSubmit", m_Device->GetLastFrameGPUWaitTimeMs());
    }

    void KurenaiEngine3D::AdvanceFramePrevViewState(
        const ShaderInterop::FrameConstants& constants, const DirectX::XMFLOAT2& jitterUv)
    {
        // --- 次フレームがこのフレームを「前フレーム」として参照するための状態を確定させる ---
        // 早期returnより後のここで行うことで、描画を行わなかったフレームでは前フレームの状態が
        // そのまま保たれ、履歴テクスチャの中身と行列の対応が1フレームずれない
        m_TAAPrevViewProj = constants.ViewProj;
        m_TAAPrevJitterUv = jitterUv;
        // Hi-Zオクルージョンカリングが「1フレームぶんの視差ずれ」を見積もるのに使う。
        // m_TAAPrevViewProjと同じ場所・同じタイミングで書くので有効性の管理も同じで済む
        m_PrevCameraPosition = { constants.CameraPosition.x, constants.CameraPosition.y, constants.CameraPosition.z };
        m_TAAPrevViewProjValid = true;
    }

    void KurenaiEngine3D::AdvanceFrameHistory()
    {
        m_TAAPrevEffectiveExposureEV100 = m_EffectiveExposureEV100;

        // MegaLightsの時間再利用も同じ場所でping-pongを反転する。
        // 今フレームの書き込み先が、次フレームでは履歴(読み込み元)になる
        {
            const bool temporalRan = ShouldRunMegaLights() && m_Settings.MegaLights.Mode == MegaLightsMode::Stochastic &&
                                     m_Settings.MegaLights.TemporalEnabled && m_MegaLightsPasses->HasTemporalPipelineState() &&
                                     m_RenderTargets.MegaLightsReservoirHistory[0] && m_RenderTargets.MegaLightsHistoryGuide[0];
            // 【手法3もガイドを書くので同じ反転が要る】あちらは時間再利用を持たないが、
            // デノイザが読む「前フレームの幾何」を Resolve が書いている。反転しないと
            // 同じフレームで書いた側を読むことになり、比べたい「別のフレームの同じ点」に
            // ならない(そのうえ RenderGraph は WAR の辺を張らないので競合する)
            const bool quadGuideRan = ShouldRunMegaLights() &&
                                      m_Settings.MegaLights.Mode == MegaLightsMode::QuadShared &&
                                      m_MegaLightsPasses->HasResolvePipelineState() && m_RenderTargets.MegaLightsHistoryGuide[0];
            m_MegaLightsPasses->AdvanceHistory(temporalRan || quadGuideRan);
            // 露出はパスの有無に関わらず記録する(次に走ったときの比較の基準になる)
            m_MegaLightsPrevEffectiveExposureEV100 = m_EffectiveExposureEV100;

            // デノイザの履歴も同じ場所で反転する。今フレームの書き込み先が次フレームの履歴になる
            // 【時間再利用の有無には依存しない】デノイザは「出た色」をならすもので、
            // リザーバを混ぜる時間再利用とは独立に効く。条件を混ぜると、片方を切ったときに
            // もう片方の履歴まで無効になって原因が分からなくなる
            const bool denoiseRan = ShouldRunMegaLights() &&
                                    (m_Settings.MegaLights.Mode == MegaLightsMode::Stochastic ||
                                     m_Settings.MegaLights.Mode == MegaLightsMode::QuadShared) &&
                                    m_Settings.MegaLights.DenoiseEnabled && m_MegaLightsPasses->HasDenoisePipelineStates() &&
                                    m_RenderTargets.MegaLightsDenoisedTexture != nullptr;
            m_MegaLightsPasses->AdvanceDenoiseHistory(denoiseRan);
        }

        if (m_Settings.PostProcess.TAAEnabled)
        {
            // 今フレームの書き込み先が、次フレームでは履歴(読み込み元)になる
            m_TAAHistoryIndex ^= 1u;
            m_TAAHistoryValid.store(true, std::memory_order_relaxed);
        }
        else
        {
            // 無効の間は履歴を更新していないので、再度有効化されたときに古い絵が混ざらないよう落としておく
            m_TAAHistoryValid.store(false, std::memory_order_relaxed);
        }
    }
}
