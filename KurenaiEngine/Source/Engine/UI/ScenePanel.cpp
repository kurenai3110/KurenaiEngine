#include "UI/ScenePanel.h"

#include <imgui.h>

#include <cfloat>
#include <string>

#include "Core/StringUtil.h"
#include "KurenaiEngine3D.h"

namespace Kurenai::UI
{
    void ScenePanel::Draw(const PanelDrawContext& context)
    {
        (void)context;

        if (!ImGui::Begin(GetWindowName(), GetVisiblePtr()))
        {
            // 折りたたまれている・ドックの非アクティブタブなどで中身が不要な場合も
            // End()は必ず呼ぶ必要がある(ImGuiのBegin/Endは戻り値に関わらず対で呼ぶ規約)
            ImGui::End();
            return;
        }

        // 使用中のグラフィックスAPIはメニューバーに常時出しているため、ここでは扱わない
        ImGui::TextDisabled("ボタンを押すとそのシーンを読み込む");

        for (size_t i = 0; i < m_Engine.m_SceneDisplayNames.size(); ++i)
        {
            const bool isCurrent = (i == m_Engine.m_CurrentSceneIndex);
            if (isCurrent)
            {
                ImGui::BeginDisabled();
            }

            const std::string label = Core::WideToUtf8(m_Engine.m_SceneDisplayNames[i]);
            if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f)))
            {
                // 実際の読み込みはLoaderスレッドが行うため、ここは要求を出すだけで即座に戻る
                // (KurenaiEngine3D::RequestSceneLoad参照)
                m_Engine.RequestSceneLoad(i);
            }

            if (isCurrent)
            {
                ImGui::EndDisabled();
            }
        }

        ImGui::End();
    }
}
