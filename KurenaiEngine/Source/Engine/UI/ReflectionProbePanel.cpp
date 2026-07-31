#include "UI/ReflectionProbePanel.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

#include "EngineDefaults.h"
#include "KurenaiEngine3D.h"
#include "UI/UIWidgets.h"

namespace Kurenai::UI
{
    void ReflectionProbePanel::Draw(const PanelDrawContext& context)
    {
        (void)context;

        if (!ImGui::Begin(GetWindowName(), GetVisiblePtr()))
        {
            ImGui::End();
            return;
        }

        DrawUsageHint();

        DrawGlobalSettings();
        ImGui::Separator();
        DrawProbeList();

        const bool hasSelection = m_Engine.m_SelectedProbeIndex >= 0 &&
                                  m_Engine.m_SelectedProbeIndex < static_cast<int>(m_Engine.m_ReflectionProbes.size());
        if (hasSelection)
        {
            ImGui::Separator();
            DrawSelectedProbeEditor();
        }

        ImGui::End();
    }

    void ReflectionProbePanel::DrawGlobalSettings()
    {
        using ProbeUpdateMode = KurenaiEngine3D::ProbeUpdateMode;

        BeginParamGroup();

        CheckboxEx(
            "反射プローブを有効にする###EnableReflectionProbes", &m_Engine.m_ReflectionProbeEnabled,
            Defaults::ReflectionProbeEnabled,
            "無効にすると、鏡面反射の環境項がすべてスカイボックス由来のグローバルIBLになる");

        // 以下2つは球形・単一選択・視差補正なしの旧構成との見比べ用。どちらも焼き直し不要で、
        // 環境ソースの引き方だけが変わる
        CheckboxEx(
            "視差補正###ProbeParallaxCorrection", &m_Engine.m_ProbeParallaxCorrectionEnabled,
            Defaults::ProbeParallaxCorrectionEnabled,
            "Box形状のときのみ有効。反射ベクトルを箱と交差させることで、プローブの中心から離れた"
            "場所でも反射の位置が合うようにする");
        CheckboxEx(
            "プローブのブレンド###ProbeBlending", &m_Engine.m_ProbeBlendingEnabled, Defaults::ProbeBlendingEnabled,
            "影響範囲の境界から内側へブレンド距離ぶんかけて重みを立ち上げる。"
            "無効にすると最も近いプローブだけを使うため、境界に継ぎ目が出る");

        // 更新モード。焼き直しのコストとシーンの変化への追従はトレードオフの関係にあり、
        // プロファイラの ProbeBakeN / ProbeRealtimeCapture / ProbeRealtimeConvolve と
        // 見比べながら選べるようにしてある
        static const char* kUpdateModeNames[] = { "焼き込み", "変化を検出して焼き直す", "毎フレーム1面ずつ" };
        int updateModeIndex = static_cast<int>(m_Engine.m_ProbeUpdateMode);
        if (ComboEx(
                "更新モード###ProbeUpdateMode", &updateModeIndex, kUpdateModeNames, IM_ARRAYSIZE(kUpdateModeNames),
                static_cast<int>(ProbeUpdateMode::Baked),
                "焼き込み: シーン読み込み時と「焼き直す」ボタンのときだけ焼く。\n"
                "変化を検出して焼き直す: 太陽・時刻・ライトが変わったときにも焼き直す。\n"
                "毎フレーム1面ずつ: さらに毎フレーム1面ずつ焼き、プローブをラウンドロビンで回る"))
        {
            m_Engine.m_ProbeUpdateMode = static_cast<ProbeUpdateMode>(updateModeIndex);
        }

        EndParamGroup();

        ImGui::Text(
            "プローブ数: %zu / %u", m_Engine.m_ReflectionProbes.size(), KurenaiEngine3D::kMaxReflectionProbes);

        if (!m_Engine.m_ProbeBaked && !m_Engine.m_ReflectionProbes.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "まだ焼かれていません");
        }
        else if (m_Engine.m_ProbeUpdateMode == ProbeUpdateMode::Realtime && !m_Engine.m_ReflectionProbes.empty())
        {
            // 今どのプローブの何面目を焼いているか。1周にプローブ数×6フレームかかるので、
            // 「変化が反射へ現れるまでの遅れ」がこの進行から読める
            ImGui::Text(
                "更新中: プローブ %u の %u / %u 面目", m_Engine.m_ProbeRealtimeProbeIndex,
                m_Engine.m_ProbeRealtimeFace + 1, KurenaiEngine3D::kCubeFaceCount);
        }
    }

    void ReflectionProbePanel::DrawProbeList()
    {
        if (m_Engine.m_ReflectionProbes.empty())
        {
            ImGui::TextDisabled("このシーンには反射プローブがありません");
        }
        else if (ImGui::BeginTable(
                     "ProbeList", 1,
                     ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter,
                     ImVec2(0.0f, ImGui::GetFontSize() * 6.0f)))
        {
            for (size_t i = 0; i < m_Engine.m_ReflectionProbes.size(); ++i)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(static_cast<int>(i));

                char label[192];
                std::snprintf(
                    label, sizeof(label), "[%zu] %s", i,
                    m_Engine.m_ReflectionProbes[i].Name.empty() ? "(名前なし)"
                                                                : m_Engine.m_ReflectionProbes[i].Name.c_str());
                if (ImGui::Selectable(label, m_Engine.m_SelectedProbeIndex == static_cast<int>(i)))
                {
                    m_Engine.m_SelectedProbeIndex = static_cast<int>(i);
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // キューブマップ配列は固定容量のため、上限に達したら追加できない
        ImGui::BeginDisabled(m_Engine.m_ReflectionProbes.size() >= KurenaiEngine3D::kMaxReflectionProbes);
        if (ImGui::Button("追加"))
        {
            // 追加位置はカメラ位置ではなくシーンAABBの中心にする(カメラ位置だと壁や地面へ
            // めり込んだ位置に置かれやすく、そのまま焼くと真っ暗なプローブになるため)
            Assets::ReflectionProbe newProbe;
            newProbe.Position[0] = (m_Engine.m_Scene.BoundsMin[0] + m_Engine.m_Scene.BoundsMax[0]) * 0.5f;
            newProbe.Position[1] = (m_Engine.m_Scene.BoundsMin[1] + m_Engine.m_Scene.BoundsMax[1]) * 0.5f;
            newProbe.Position[2] = (m_Engine.m_Scene.BoundsMin[2] + m_Engine.m_Scene.BoundsMax[2]) * 0.5f;
            newProbe.Name = "New Probe";
            m_Engine.m_ReflectionProbes.push_back(newProbe);
            m_Engine.m_SelectedProbeIndex = static_cast<int>(m_Engine.m_ReflectionProbes.size()) - 1;
            m_Engine.m_ProbeBakeRequested = true;
            // 選択が変わるので名前バッファを詰め直させる
            m_NameBufferProbeIndex = -1;
        }
        ImGui::EndDisabled();
        ItemHelp("シーンAABBの中心に新しいプローブを置く。キューブマップ配列の容量まで追加できる");

        const bool hasSelection = m_Engine.m_SelectedProbeIndex >= 0 &&
                                  m_Engine.m_SelectedProbeIndex < static_cast<int>(m_Engine.m_ReflectionProbes.size());

        ImGui::SameLine();
        ImGui::BeginDisabled(!hasSelection);
        if (ImGui::Button("削除") && hasSelection)
        {
            m_Engine.m_ReflectionProbes.erase(m_Engine.m_ReflectionProbes.begin() + m_Engine.m_SelectedProbeIndex);
            m_Engine.m_SelectedProbeIndex =
                m_Engine.m_ReflectionProbes.empty()
                    ? -1
                    : std::min(m_Engine.m_SelectedProbeIndex, static_cast<int>(m_Engine.m_ReflectionProbes.size()) - 1);
            // 番号がずれるため残り全部を焼き直す
            m_Engine.m_ProbeBakeRequested = !m_Engine.m_ReflectionProbes.empty();
            m_Engine.m_ProbeBaked = m_Engine.m_ProbeBaked && !m_Engine.m_ReflectionProbes.empty();
            // 削除でインデックスが同じまま別のプローブを指す場合があるため、必ず詰め直させる
            m_NameBufferProbeIndex = -1;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(m_Engine.m_ReflectionProbes.empty());
        if (ImGui::Button("焼き直す"))
        {
            m_Engine.m_ProbeBakeRequested = true;
        }
        ImGui::EndDisabled();
        ItemHelp("全プローブを現在のライティングで撮り直す");
    }

    void ReflectionProbePanel::DrawSelectedProbeEditor()
    {
        Assets::ReflectionProbe& probe = m_Engine.m_ReflectionProbes[static_cast<size_t>(m_Engine.m_SelectedProbeIndex)];

        BeginParamGroup();

        if (m_NameBufferProbeIndex != m_Engine.m_SelectedProbeIndex)
        {
            m_NameBufferProbeIndex = m_Engine.m_SelectedProbeIndex;
            std::snprintf(m_NameBuffer.data(), m_NameBuffer.size(), "%s", probe.Name.c_str());
        }
        if (ImGui::InputText("名前###ProbeName", m_NameBuffer.data(), m_NameBuffer.size()))
        {
            probe.Name = m_NameBuffer.data();
        }

        // 位置はキャプチャ内容そのものを変えるため、動かしたら焼き直す必要がある
        if (ImGui::DragFloat3("位置###ProbePosition", probe.Position, 0.1f))
        {
            m_Engine.m_ProbeBakeRequested = true;
        }
        ItemHelp("6方向を撮る位置。動かすと焼き直しが要る");

        // 以下の影響範囲パラメータはどれもキャプチャ内容には影響しない(どこから撮るかは
        // 位置だけで決まる)ため、変更しても焼き直しは不要
        static const char* kShapeNames[] = { "球", "箱" };
        int shapeIndex = (probe.Shape == Assets::ReflectionProbeShape::Box) ? 1 : 0;
        if (ImGui::Combo("影響範囲の形###ProbeShape", &shapeIndex, kShapeNames, IM_ARRAYSIZE(kShapeNames)))
        {
            probe.Shape =
                (shapeIndex == 1) ? Assets::ReflectionProbeShape::Box : Assets::ReflectionProbeShape::Sphere;
        }
        ItemHelp("このプローブが効く範囲の形。視差補正は箱のときだけ働く");

        if (probe.Shape == Assets::ReflectionProbeShape::Box)
        {
            // 各軸の半径。0以下だと箱が潰れて交差計算が成り立たないため下限を与える
            if (ImGui::DragFloat3("箱の半径###ProbeBoxExtents", probe.BoxExtents, 0.1f, 0.1f, 1000.0f, "%.2f"))
            {
                for (float& extent : probe.BoxExtents)
                {
                    extent = std::max(extent, 0.1f);
                }
            }
            ImGui::DragFloat("箱の向き###ProbeYaw", &probe.YawDegrees, 0.5f, -180.0f, 180.0f, "%.1f deg");
            ItemHelp("Y軸まわりの回転。壁が軸に平行でない部屋へ箱を合わせるために使う");
        }
        else
        {
            ImGui::DragFloat("半径###ProbeRadius", &probe.Radius, 0.1f, 0.1f, 1000.0f, "%.2f");
        }

        ImGui::DragFloat("ブレンド距離###ProbeBlendDistance", &probe.BlendDistance, 0.05f, 0.0f, 100.0f, "%.2f");
        ItemHelp("影響範囲の境界から内側へ、このプローブの重みが1になるまでの距離。0にすると境界が硬く切れる");

        EndParamGroup();
    }
}
