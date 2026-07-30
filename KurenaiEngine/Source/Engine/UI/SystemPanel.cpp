#include "UI/SystemPanel.h"

#include <imgui.h>

#include <cfloat>

#include "EngineDefaults.h"
#include "KurenaiEngine3D.h"
#include "UI/UIManager.h"
#include "UI/UIWidgets.h"

namespace Kurenai::UI
{
    void SystemPanel::Draw(const PanelDrawContext& context)
    {
        (void)context;

        if (!ImGui::Begin(GetWindowName(), GetVisiblePtr()))
        {
            ImGui::End();
            return;
        }

        DrawUsageHint();

        if (ImGui::CollapsingHeader("表示###Display", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawDisplaySection();
        }
        if (ImGui::CollapsingHeader("UI###UI", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawUISection();
        }

        ImGui::End();
    }

    void SystemPanel::DrawDisplaySection()
    {
        BeginParamGroup();

        CheckboxEx(
            "垂直同期###EnableVSync", &m_Engine.m_VSyncEnabled, Defaults::VSyncEnabled,
            "Presentをディスプレイのリフレッシュに同期させる。ティアリングは消えるが遅延は増える");

        CheckboxEx(
            "フレームレート制限###FixedFPS", &m_Engine.m_FixedFPSEnabled, Defaults::FixedFPSEnabled,
            "指定したフレームレートを超えないように待機を入れる");

        if (m_Engine.m_FixedFPSEnabled)
        {
            static const char* kTargetFPSNames[] = { "30", "60", "120" };
            static const float kTargetFPSValues[] = { 30.0f, 60.0f, 120.0f };
            static_assert(
                IM_ARRAYSIZE(kTargetFPSNames) == IM_ARRAYSIZE(kTargetFPSValues),
                "表示名と値の並びを一致させること");

            // 見つからない場合は60fps相当の位置にしておく
            int targetFPSIndex = 1;
            int defaultIndex = 1;
            for (int i = 0; i < IM_ARRAYSIZE(kTargetFPSValues); ++i)
            {
                if (kTargetFPSValues[i] == m_Engine.m_TargetFPS)
                {
                    targetFPSIndex = i;
                }
                if (kTargetFPSValues[i] == Defaults::TargetFPS)
                {
                    defaultIndex = i;
                }
            }

            if (ComboEx(
                    "目標フレームレート###TargetFPS", &targetFPSIndex, kTargetFPSNames, IM_ARRAYSIZE(kTargetFPSNames),
                    defaultIndex, "上限とするフレームレート"))
            {
                m_Engine.m_TargetFPS = kTargetFPSValues[targetFPSIndex];
            }
        }

        EndParamGroup();
    }

    void SystemPanel::DrawUISection()
    {
        ImGui::TextUnformatted("パネルの表示");
        m_UIManager.DrawPanelVisibilityControls();

        ImGui::Separator();

        if (ImGui::Button("レイアウトを初期化", ImVec2(-FLT_MIN, 0.0f)))
        {
            m_UIManager.RequestResetLayout();
        }
        ItemHelp(
            "パネルの配置とサイズを既定へ戻し、閉じたパネルも表示し直す。"
            "配置はexeと同じフォルダのimgui.iniへ自動保存される");

        // フォントとパネル寸法の拡大率。DPIへの追従状況を確認できるようにしておく
        ImGui::Text("UIスケール: %.2fx", ImGui::GetStyle().FontScaleDpi);
        ItemHelp("モニタのDPIから決まるUI全体の拡大率。96 DPIを1.00xとする");
    }
}
