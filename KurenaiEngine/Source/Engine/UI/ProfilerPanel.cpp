#include "UI/ProfilerPanel.h"

#include <imgui.h>

#include "KurenaiEngine3D.h"
#include "RHI/IRHIGPUProfiler.h"

namespace Kurenai::UI
{
    void ProfilerPanel::Draw(const PanelDrawContext& context)
    {
        (void)context;

        if (!ImGui::Begin(GetWindowName(), GetVisiblePtr()))
        {
            ImGui::End();
            return;
        }

        ImGui::Text("FPS: %.1f", m_Engine.m_FPS);
        ImGui::Text("CPUフレーム時間: %.3f ms", m_Engine.m_CPUFrameTimeMs);
        ImGui::Text("GPUフレーム時間: %.3f ms", m_Engine.m_GPUProfiler->GetTotalFrameTimeMs());
        // GPUの完了待ち(DX12のフレームパイプライン化に伴うフェンス待ち)。CPUフレーム時間や
        // PresentSubmitの計測値からは既に除外済みなので、参考情報として別枠で表示する
        ImGui::Text("GPU待ち: %.3f ms", m_Engine.GetLastFrameGPUWaitTimeMs());

        // パス別のドローコール数(直前フレーム)。**シャドウは4カスケードぶんの合計**。
        // 1モデル1ドロー化やGPU駆動描画が効いたかどうかは、GPU時間だけでは切り分けられない
        // (発行回数が減ってもピクセルの仕事量は変わらないため)。発行回数そのものを出す
        ImGui::Text(
            "ドローコール: G-Buffer %u / シャドウ %u / 深度プリパス %u",
            m_Engine.m_DrawCallsGBufferLastFrame, m_Engine.m_DrawCallsShadowLastFrame,
            m_Engine.m_DrawCallsDepthPrepassLastFrame);

        // フラスタムカリングの効き(直前のフレームぶん・全パス合計)。
        //
        // 【ログにも出しているが画面にも出す】ログは1秒間隔の平均で、カメラを振りながら
        // 「いま間引けているか」を見るには遅い。逆に画面はその場で消えるので、
        // 記録として残すログと両方が要る(SystemPanelの性能ログのトグルと同じ関係)。
        //
        // 【モデル単位とメッシュ単位を別の行にする】分母も効くシーンも違うため。
        // モデル単位は.kmodelを多数並べるシーンで効き、1モデルに数千メッシュを持つ
        // アセット(Emerald Square、Bistro)では1つも間引けない。メッシュ単位はその逆
        ImGui::SeparatorText("フラスタムカリング(直前のフレーム)");
        const auto cullLine = [](const char* label, uint32_t tested, uint32_t culled)
        {
            if (tested == 0)
            {
                // 「判定が一度も走っていない」と「判定は走ったが1つも間引けなかった」は
                // 別のこと。0/0を割合として出すと区別が付かないので、文言で分ける
                ImGui::Text("%s: 判定なし", label);
                return;
            }
            const float ratio = 100.0f * static_cast<float>(culled) / static_cast<float>(tested);
            ImGui::Text("%s: 判定 %u / 間引き %u (%.1f%%)", label, tested, culled, ratio);
        };
        cullLine("モデル単位", m_Engine.m_FrustumCullTestedLastFrame, m_Engine.m_FrustumCullCulledLastFrame);
        cullLine("メッシュ単位", m_Engine.m_MeshCullTestedLastFrame, m_Engine.m_MeshCullCulledLastFrame);

        // パスごとの内訳は行数が多いので、左右に並べて縦の長さを半分にする
        if (!ImGui::BeginTable("PassBreakdown", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::End();
            return;
        }

        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::SeparatorText("CPU パス別");
        for (const auto& result : m_Engine.m_CPUProfiler.GetResults())
        {
            ImGui::Text("%s: %.3f ms", result.Name.c_str(), result.TimeMs);
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::SeparatorText("GPU パス別");
        for (const auto& result : m_Engine.m_GPUProfiler->GetResults())
        {
            ImGui::Text("%s: %.3f ms", result.Name.c_str(), result.TimeMs);
        }

        ImGui::EndTable();

        ImGui::End();
    }
}
