#include "../KurenaiEngine3D.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

#include "Core/Logger.h"
#include "Core/StringUtil.h"
// パス群のカウンタを m_XxxPasses->Get...() で読むため、前方宣言では足りない
#include "../Passes/GeometryPasses.h"
#include "../Passes/ShadowPasses.h"

// 性能の記録をログと -perfdump のファイルへ残す。
// KurenaiEngine3D のメンバ関数のまま、翻訳単位だけをここへ分けている
// (宣言は KurenaiEngine3D.h のまま)
namespace Kurenai
{
    // 性能の記録をログファイルへ残す。ProfilerPanelの表示は実行中しか見えず、後から
    // 「この変更でフレーム時間がどう変わったか」を比較できない。集計期間ぶんを1行に
    // まとめて出すことで、フレーム時間への影響(Logger::Infoはflushを伴う)を
    // 1秒に1回に抑えつつ、実行ごとの記録が残るようにしている
    void KurenaiEngine3D::LogFrameStatsIfDue(float renderDeltaTime)
    {
        if (!m_Settings.System.FrameStatsLoggingEnabled)
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();

        Diagnostics::FrameStatsLogger::FrameSample sample;
        sample.CPUFrameTimeMs = m_RenderStats.CPUFrameTimeMs;
        sample.GPUTimeMs = m_GPUProfiler ? m_GPUProfiler->GetTotalFrameTimeMs() : 0.0f;
        sample.GPUWaitMs = m_Device->GetLastFrameGPUWaitTimeMs();
        sample.FrameTimeMs = renderDeltaTime * 1000.0f;
        sample.FrustumCullTested = m_FrustumCullTested;
        sample.FrustumCullCulled = m_FrustumCullCulled;
        sample.LODSwitchCount = m_LODSwitchCount;
        sample.LODFadingCount = m_RenderStats.LODFadingCount;
        sample.MeshCullTested = m_MeshCullTested;
        sample.MeshCullCulled = m_MeshCullCulled;
        sample.DrawCallsGBuffer = m_GeometryPasses->GetDrawCallsGBuffer();
        sample.DrawCallsShadow = m_ShadowPasses->GetDrawCalls();
        sample.DrawCallsDepthPrepass = m_GeometryPasses->GetDrawCallsDepthPrepass();
        sample.InstancedBatchCount = m_InstancedBatchCount;
        sample.InstancedInstanceCount = m_InstancedInstanceCount;
        m_FrameStats.AddFrame(now, sample);

        const float elapsedSeconds = m_FrameStats.GetElapsedSeconds(now);
        if (elapsedSeconds < Defaults::FrameStatsLogIntervalSeconds)
        {
            return;
        }

        // 集計期間の実測フレーム数から求める。m_RenderStats.FPS(指数移動平均)と違い、この値は
        // 期間中に落ちたフレームがそのまま反映される
        const float averageFPS = static_cast<float>(m_FrameStats.GetFrameCount()) / std::max(elapsedSeconds, 1e-6f);
        const double frameCount = static_cast<double>(m_FrameStats.GetFrameCount());

        char buffer[256];
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%ux%u %s | FPS %.1f (%u frames / %.2fs) | CPU %.2fms | GPU %.2fms | GPU待ち %.2fms | 最悪フレーム %.2fms",
            m_RenderWidth,
            m_RenderHeight,
            m_GraphicsAPI == GraphicsAPI::DX12 ? "DX12" : "DX11",
            averageFPS,
            m_FrameStats.GetFrameCount(),
            elapsedSeconds,
            m_FrameStats.GetCPUTimeSumMs() / frameCount,
            m_FrameStats.GetGPUTimeSumMs() / frameCount,
            m_FrameStats.GetGPUWaitSumMs() / frameCount,
            m_FrameStats.GetWorstFrameTimeMs());
        Core::Logger::Info("Perf", buffer);

        // パス別の内訳。どのパスを削れば効くのかは合計値からは分からないため、
        // 集計期間の最後のフレームぶんを重い順に並べて残す。
        // (毎フレーム平均を取るにはパス構成がフレームごとに変わりうるので、
        //  代表として1フレームぶんを出す。ベイクパスが走ったフレームに当たると
        //  その分だけ大きく出るが、常時走るパスの比較には十分)
        if (m_GPUProfiler)
        {
            std::vector<RHI::GPUTimingResult> passes = m_GPUProfiler->GetResults();
            std::sort(passes.begin(), passes.end(), [](const auto& a, const auto& b) { return a.TimeMs > b.TimeMs; });

            std::string breakdown;
            for (const auto& pass : passes)
            {
                // 0.05ms未満は並べても判断材料にならず、行が長くなるだけなので落とす
                if (pass.TimeMs < 0.05f)
                {
                    break;
                }
                char passText[64];
                std::snprintf(passText, sizeof(passText), "%s %.2f", pass.Name.c_str(), pass.TimeMs);
                if (!breakdown.empty())
                {
                    breakdown += " / ";
                }
                breakdown += passText;
            }

            if (!breakdown.empty())
            {
                Core::Logger::Info("Perf", "  GPU内訳[ms]: " + breakdown);
            }
        }

        // CPU側の内訳も同じ形で残す。GIVolumeを持つシーンではCPUフレーム時間が24〜28msあり、
        // 60fpsの予算(16.7ms)をCPU単独で超えている。GPUの内訳だけでは、その時間が
        // どのパスのドローコール発行に消えているのかが分からない
        {
            std::vector<Core::CPUTimingResult> cpuPasses = m_CPUProfiler.GetResults();
            std::sort(
                cpuPasses.begin(), cpuPasses.end(), [](const auto& a, const auto& b) { return a.TimeMs > b.TimeMs; });

            std::string breakdown;
            for (const auto& pass : cpuPasses)
            {
                // GPU側と同じ理由で0.05ms未満は落とす
                if (pass.TimeMs < 0.05f)
                {
                    break;
                }
                char passText[64];
                std::snprintf(passText, sizeof(passText), "%s %.2f", pass.Name.c_str(), pass.TimeMs);
                if (!breakdown.empty())
                {
                    breakdown += " / ";
                }
                breakdown += passText;
            }

            if (!breakdown.empty())
            {
                Core::Logger::Info("Perf", "  CPU内訳[ms]: " + breakdown);
            }
        }

        // フラスタムカリングの効き。「間引いた数が0」は、判定式が常に通しているのか
        // 本当に全部が視界内なのかを区別できないため、テストした数と併せて出す。
        //
        // 【モデル単位とメッシュ単位を別の行にする】分母も、効くシーンも違う。
        // モデル単位は.kmodelを多数並べるシーンで効き、1モデルに数千メッシュを持つ
        // アセットでは1つも間引けない。メッシュ単位はその逆。合算すると、どちらが効いたのか
        // ―― あるいは片方が一度も実行されていないのか ―― が読めなくなる
        const auto logCullStats = [this](const char* label, uint64_t testedSum, uint64_t culledSum)
        {
            if (testedSum == 0 || m_FrameStats.GetFrameCount() == 0)
            {
                // 判定が1回も走っていない。「間引き0」と区別が付くよう、行そのものを出さない
                return;
            }
            const double testedPerFrame = static_cast<double>(testedSum) / m_FrameStats.GetFrameCount();
            const double culledPerFrame = static_cast<double>(culledSum) / m_FrameStats.GetFrameCount();
            const double ratio = 100.0 * static_cast<double>(culledSum) / static_cast<double>(testedSum);

            char cullText[224];
            std::snprintf(
                cullText, sizeof(cullText), "  %s: 判定 %.1f / 間引き %.1f (%.1f%%) [1フレームあたり・全パス合計]",
                label, testedPerFrame, culledPerFrame, ratio);
            Core::Logger::Info("Perf", cullText);
        };
        logCullStats("フラスタムカリング(モデル単位)", m_FrameStats.GetCullTestedSum(), m_FrameStats.GetCullCulledSum());
        logCullStats("フラスタムカリング(メッシュ単位)", m_FrameStats.GetMeshCullTestedSum(), m_FrameStats.GetMeshCullCulledSum());

        // モデルLOD。【切り替え0回なら一度も効いていない】距離のしきい値が実際の
        // カメラの動く範囲から外れているか、そもそもLODPathが指定されていない
        {
            char lodText[192];
            std::snprintf(
                lodText, sizeof(lodText),
                "  モデルLOD: 切り替え %llu回 / フェード %llu インスタンス×フレーム [いずれも集計期間の合計]",
                static_cast<unsigned long long>(m_FrameStats.GetLODSwitchSum()),
                static_cast<unsigned long long>(m_FrameStats.GetLODFadingSum()));
            Core::Logger::Info("Perf", lodText);
        }

        // モデルのストリーミング。【常駐0や読み込み0なら効いていない】
        // 範囲内なのに常駐していないものが残り続けるなら、発注か受け取りのどこかで詰まっている
        if (m_Scene.HasStreamingDistance)
        {
            char streamText[192];
            std::snprintf(
                streamText, sizeof(streamText),
                "  ストリーミング: 常駐 %u / 範囲内 %u (距離 %.0fm) / 読み込み累計 %llu件 / 破棄累計 %llu件"
                " / RT再構築 %llu回(直近 %.1fms)",
                m_StreamingResidentCount, m_StreamingTargetCount, m_Scene.StreamingDistance,
                static_cast<unsigned long long>(m_StreamingLoadedTotal),
                static_cast<unsigned long long>(m_StreamingEvictedTotal),
                static_cast<unsigned long long>(m_RaytracingRebuildCount), m_RaytracingRebuildLastMs);
            Core::Logger::Info("Perf", streamText);
        }

        // パス別のドローコール数。**「G-Bufferは減ったがシャドウは減っていない」**のような
        // 片手落ちは合計値では見えない(シャドウはカスケード4回ぶんが積み上がる)
        if (m_FrameStats.GetFrameCount() > 0)
        {
            const double frames = static_cast<double>(m_FrameStats.GetFrameCount());
            char drawText[224];
            std::snprintf(
                drawText, sizeof(drawText),
                "  ドローコール: G-Buffer %.1f / シャドウ %.1f (4カスケード計) / 深度プリパス %.1f "
                "[1フレームあたり]",
                static_cast<double>(m_FrameStats.GetDrawCallsGBufferSum()) / frames,
                static_cast<double>(m_FrameStats.GetDrawCallsShadowSum()) / frames,
                static_cast<double>(m_FrameStats.GetDrawCallsDepthPrepassSum()) / frames);
            Core::Logger::Info("Perf", drawText);
        }

        // インスタンシングの効き。**ドローコール数とは別建てにする** ――
        // 「バッチ0」は「まとめられる相手がいない」のか「一度も実行されていない」のかを
        // 区別できないので、まとめた数(バッチ)とまとめた対象(インスタンス)の両方を出す。
        // まとめたことで減ったドロー数は (インスタンス数 - バッチ数) x そのモデルのメッシュ数
        if (m_FrameStats.GetFrameCount() > 0 && m_FrameStats.GetInstancedBatchSum() > 0)
        {
            const double frames = static_cast<double>(m_FrameStats.GetFrameCount());
            char instText[192];
            std::snprintf(
                instText, sizeof(instText),
                "  インスタンシング: バッチ %.1f / まとめたインスタンス %.1f [1フレームあたり・2組の合計]",
                static_cast<double>(m_FrameStats.GetInstancedBatchSum()) / frames,
                static_cast<double>(m_FrameStats.GetInstancedInstanceSum()) / frames);
            Core::Logger::Info("Perf", instText);
        }

        // bindless区画の使用状況。**満杯になっても例外は飛ばず、エラーログ1行と
        // kInvalidBindlessIndex(=白1x1へ落ちる)しか残らない**ため、上限へ近づいていることを
        // 定期的に見えるようにしておく(IRHIDevice::GetBindlessUsedCountのコメント参照)
        if (m_Device)
        {
            const uint32_t bindlessCapacity = m_Device->GetBindlessCapacity();
            if (bindlessCapacity > 0)
            {
                const uint32_t bindlessUsed = m_Device->GetBindlessUsedCount();
                char bindlessText[160];
                std::snprintf(
                    bindlessText, sizeof(bindlessText), "  bindless: %u / %u ディスクリプタ (%.1f%%)",
                    bindlessUsed, bindlessCapacity,
                    100.0 * static_cast<double>(bindlessUsed) / static_cast<double>(bindlessCapacity));
                Core::Logger::Info("Perf", bindlessText);
            }
        }

        // メッシュレット単位のカリング(増幅シェーダー)の効き。上のCPU側とは粒度も判定の種類も
        // 違うので別の行に出す。
        //
        // 【オクルージョンを視錐台+コーンと分けて出す】完了条件がここにある ――
        // 俯瞰(遮蔽が少ない)と街路(遮蔽が多い)でオクルージョンの割合に差が出ることが、
        // 判定が実際に効いていることの証拠になる。合算すると視錐台の変動に埋もれて分からない
        if (m_FrameStats.GetMeshletSampleCount() > 0 && m_FrameStats.GetMeshletTestedSum() > 0)
        {
            const double samples = static_cast<double>(m_FrameStats.GetMeshletSampleCount());
            const double tested = static_cast<double>(m_FrameStats.GetMeshletTestedSum());
            const double frustumRatio = 100.0 * static_cast<double>(m_FrameStats.GetMeshletFrustumCulledSum()) / tested;
            const double occlusionRatio = 100.0 * static_cast<double>(m_FrameStats.GetMeshletOcclusionCulledSum()) / tested;

            char meshletCullText[256];
            std::snprintf(
                meshletCullText, sizeof(meshletCullText),
                "  メッシュレットカリング: 判定 %.1f / 視錐台+コーン %.1f (%.1f%%) / オクルージョン %.1f (%.1f%%)"
                " [1フレームあたり・%u フレーム分]",
                tested / samples,
                static_cast<double>(m_FrameStats.GetMeshletFrustumCulledSum()) / samples, frustumRatio,
                static_cast<double>(m_FrameStats.GetMeshletOcclusionCulledSum()) / samples, occlusionRatio,
                m_FrameStats.GetMeshletSampleCount());
            Core::Logger::Info("Perf", meshletCullText);
        }

        // モデル単位のGPUカリング(Stage 5-3)。
        //
        // 【判定数と視錐台の間引き数がCPUと一致することが合格条件】GPUは同じAABBを
        // 同じ視錐台で判定しているので、一致しなければ平面の作り方か候補の積み方が壊れている。
        // **間接描画はこの数を信じて描く**ので、食い違ったまま進むと絵が消えてから
        // 原因を探すことになる。だから食い違いは警告として残す
        if (m_CullStats.GetModelTested() > 0)
        {
            char modelCullText[256];
            std::snprintf(
                modelCullText, sizeof(modelCullText),
                "  モデル単位GPUカリング: 判定 %u (CPU候補 %u) / 視錐台 %u (CPU %u) / オクルージョン %u / 生存 %u",
                m_CullStats.GetModelTested(), m_CullStats.GetModelComparedCandidateCount(),
                m_CullStats.GetModelFrustumCulled(), m_CullStats.GetModelComparedCpuFrustumCulled(),
                m_CullStats.GetModelOcclusionCulled(), m_CullStats.GetModelSurvived());
            Core::Logger::Info("Perf", modelCullText);

            // 区画ごとの発行数。**間引きの数だけ見ても、間接描画が本当に描いているかは分からない** ――
            // 描画発行に繋がっていなければここは全部0のままで、絵はCPUループが出している
            char modelCullRegionText[256];
            std::snprintf(
                modelCullRegionText, sizeof(modelCullRegionText),
                "  モデル単位GPU発行(%s): G-Buffer %u+%u / プリパス不透明 %u+%u / プリパスカットアウト %u+%u",
                m_GeometryPasses->WasModelCullIndirectActiveLastFrame() ? "間接描画" : "計数のみ",
                m_CullStats.GetModelRegionIssued(Passes::kModelCullRegionGBuffer),
                m_CullStats.GetModelRegionIssued(Passes::kModelCullRegionGBufferMirrored),
                m_CullStats.GetModelRegionIssued(Passes::kModelCullRegionPrepassOpaque),
                m_CullStats.GetModelRegionIssued(Passes::kModelCullRegionPrepassOpaqueMirrored),
                m_CullStats.GetModelRegionIssued(Passes::kModelCullRegionPrepassCutout),
                m_CullStats.GetModelRegionIssued(Passes::kModelCullRegionPrepassCutoutMirrored));
            Core::Logger::Info("Perf", modelCullRegionText);

            // どの経路で判定したか。**間引き数だけでは切り替わったか分からない** ――
            // カメラが止まっていれば前フレームのHi-Zと今フレームのHi-Zは同じ内容になり、
            // 新旧どちらの経路でも同じ数が出る。経路そのものを出しておく
            char modelCullPathText[192];
            std::snprintf(
                modelCullPathText, sizeof(modelCullPathText),
                "  Hi-Zの出どころ: %s / 判定ディスパッチ: プリパスぶん %u + G-Bufferぶん %u",
                m_GeometryPasses->WasHiZFromDepthPrepassLastFrame() ? "深度プリパス(今フレーム)" : "G-Bufferの後(前フレーム)",
                m_GeometryPasses->GetModelCullDispatchCount(0), m_GeometryPasses->GetModelCullDispatchCount(1));
            Core::Logger::Info("Perf", modelCullPathText);

            if (m_CullStats.GetModelTested() != m_CullStats.GetModelComparedCandidateCount() ||
                m_CullStats.GetModelFrustumCulled() != m_CullStats.GetModelComparedCpuFrustumCulled())
            {
                // 【黙って進めない】食い違ったままExecuteIndirectへ繋ぐと、
                // 絵が消えてから原因を探すことになる
                Core::Logger::Warning(
                    "Perf",
                    "モデル単位GPUカリングの判定がCPUと食い違っています(判定 " +
                        std::to_string(m_CullStats.GetModelTested()) + " vs " +
                        std::to_string(m_CullStats.GetModelComparedCandidateCount()) + " / 視錐台 " +
                        std::to_string(m_CullStats.GetModelFrustumCulled()) + " vs " +
                        std::to_string(m_CullStats.GetModelComparedCpuFrustumCulled()) + ")");
            }
        }

        m_FrameStats.Reset();

        // テクスチャの常駐ミップの内訳。**サイズ帯ごとに分けて出す** ――
        // 64KBタイルはBC7で256x256テクセルを覆うため、ミップ/タイル単位の制御が効くのは
        // 大きいテクスチャに偏る。「入れたから減った」ではなくどの帯に効いたかで語るため
        if (m_TextureStreaming.IsEnabled())
        {
            m_TextureStreaming.LogStats("periodic");
        }

        // 【自己申告と実測を並べる】常駐管理が積算したバイト数だけを見ていると、
        // 物差し自体が間違っていても気付けない。OSから見たVRAM使用量と一緒に出す
        uint64_t usedBytes = 0;
        uint64_t budgetBytes = 0;
        if (m_Device->GetVideoMemoryUsage(usedBytes, budgetBytes))
        {
            constexpr double kBytesPerMiB = 1024.0 * 1024.0;
            char vramLine[160];
            std::snprintf(
                vramLine, sizeof(vramLine), "VRAM: 使用 %.1f MB / 予算 %.1f MB",
                static_cast<double>(usedBytes) / kBytesPerMiB, static_cast<double>(budgetBytes) / kBytesPerMiB);
            Core::Logger::Info("Perf", vramLine);
        }
    }

    void KurenaiEngine3D::AccumulatePerfDump()
    {
        // --- GPU計測の書き出し(計測専用) ---
        // 【毎フレーム走る場所へ置くこと】Perfログを出す関数は1秒に1回しか先へ進まない
        // (集計期間に達するまで早期returnする)。そこへ置くと収集が毎秒になり、
        // 300フレーム集めるのに5分かかって測定が終わらない
        // 【Perfログとは別に集める】あちらは0.05ms未満を落とし1フレームの代表値しか出さない。
        // ここでは**閾値なしで全パスを、指定枚数ぶん平均**する
        if (!m_PerfDumpPath.empty() && !m_PerfDumpDone && m_GPUProfiler)
        {
            ++m_PerfDumpWarmupFrames;
            // 整定を待つ。内部解像度の切り替えとストリーミングが片付くまで
            if (m_PerfDumpWarmupFrames > static_cast<int32_t>(Passes::kMegaLightsAccumWarmup))
            {
                for (const RHI::GPUTimingResult& pass : m_GPUProfiler->GetResults())
                {
                    m_PerfDumpTotals[pass.Name] += static_cast<double>(pass.TimeMs);
                }
                ++m_PerfDumpCollected;

                if (m_PerfDumpCollected >= m_PerfDumpTargetFrames)
                {
                    m_PerfDumpDone = true;
                    std::ofstream file(m_PerfDumpPath, std::ios::trunc);
                    if (file)
                    {
                        file << "pass,avg_ms\n";
                        for (const auto& entry : m_PerfDumpTotals)
                        {
                            file << entry.first << ','
                                 << (entry.second / static_cast<double>(m_PerfDumpCollected)) << '\n';
                        }
                        file << "__frames," << m_PerfDumpCollected << '\n';
                        Core::Logger::Info(
                            "KurenaiEngine3D",
                            "GPU計測を書き出しました: " + Core::WideToUtf8(m_PerfDumpPath) + " (" +
                                std::to_string(m_PerfDumpCollected) + "フレームの平均)");
                    }
                    else
                    {
                        Core::Logger::Error(
                            "KurenaiEngine3D",
                            "GPU計測を書き出せませんでした(ファイルを開けない): " +
                                Core::WideToUtf8(m_PerfDumpPath));
                    }
                }
            }
        }
    }
}
