#include "../KurenaiEngine3D.h"

#include <algorithm>
// std::begin / std::end (m_ModelCullRegionIssued の一括ゼロ埋め)
#include <iterator>

#include "Core/CPUProfiler.h"
// パス群のカウンタと履歴の反転を呼ぶため、前方宣言では足りない
#include "../Passes/GeometryPasses.h"
#include "../Passes/MegaLightsPasses.h"
#include "../ShaderInterop/FrameConstants.h"
#include "RenderBlackboard.h"

// Render()から切り出したフレームの締め(段階6.6のEブロック)。
// KurenaiEngine3D のメンバ関数のまま、翻訳単位だけをここへ分けている
// (宣言は KurenaiEngine3D.h のまま。Diagnostics/RenderDumpService.cpp と同じ作法)
namespace Kurenai
{
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
            uint32_t counters[kMeshletCullStatsCount] = {};
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
            uint32_t counters[kModelCullCounterCount] = {};
            if (m_ModelCullReadback[oldest] &&
                m_ModelCullReadback[oldest]->ReadbackData(counters, sizeof(counters)))
            {
                m_ModelCullTested = counters[0];
                m_ModelCullFrustumCulled = counters[1];
                m_ModelCullOcclusionCulled = counters[2];
                m_ModelCullSurvived = counters[3];
                for (uint32_t region = 0; region < kModelCullRegionCount; ++region)
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
        m_SwapChain->Present(m_SystemSettings.VSyncEnabled);
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
            const bool temporalRan = ShouldRunMegaLights() && m_MegaLightsSettings.Mode == MegaLightsMode::Stochastic &&
                                     m_MegaLightsSettings.TemporalEnabled && m_MegaLightsPasses->HasTemporalPipelineState() &&
                                     m_RenderTargets.MegaLightsReservoirHistory[0] && m_RenderTargets.MegaLightsHistoryGuide[0];
            // 【手法3もガイドを書くので同じ反転が要る】あちらは時間再利用を持たないが、
            // デノイザが読む「前フレームの幾何」を Resolve が書いている。反転しないと
            // 同じフレームで書いた側を読むことになり、比べたい「別のフレームの同じ点」に
            // ならない(そのうえ RenderGraph は WAR の辺を張らないので競合する)
            const bool quadGuideRan = ShouldRunMegaLights() &&
                                      m_MegaLightsSettings.Mode == MegaLightsMode::QuadShared &&
                                      m_MegaLightsPasses->HasResolvePipelineState() && m_RenderTargets.MegaLightsHistoryGuide[0];
            m_MegaLightsPasses->AdvanceHistory(temporalRan || quadGuideRan);
            // 露出はパスの有無に関わらず記録する(次に走ったときの比較の基準になる)
            m_MegaLightsPrevEffectiveExposureEV100 = m_EffectiveExposureEV100;

            // デノイザの履歴も同じ場所で反転する。今フレームの書き込み先が次フレームの履歴になる
            // 【時間再利用の有無には依存しない】デノイザは「出た色」をならすもので、
            // リザーバを混ぜる時間再利用とは独立に効く。条件を混ぜると、片方を切ったときに
            // もう片方の履歴まで無効になって原因が分からなくなる
            const bool denoiseRan = ShouldRunMegaLights() &&
                                    (m_MegaLightsSettings.Mode == MegaLightsMode::Stochastic ||
                                     m_MegaLightsSettings.Mode == MegaLightsMode::QuadShared) &&
                                    m_MegaLightsSettings.DenoiseEnabled && m_MegaLightsPasses->HasDenoisePipelineStates() &&
                                    m_RenderTargets.MegaLightsDenoisedTexture != nullptr;
            m_MegaLightsPasses->AdvanceDenoiseHistory(denoiseRan);
        }

        if (m_PostProcessSettings.TAAEnabled)
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
