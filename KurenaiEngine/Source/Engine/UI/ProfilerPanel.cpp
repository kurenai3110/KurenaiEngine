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
