#include "UI/LightingPanel.h"

#include <DirectXMath.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Core/Camera.h"
#include "EngineDefaults.h"
#include "KurenaiEngine3D.h"
#include "UI/UIWidgets.h"

namespace Kurenai::UI
{
    namespace
    {
        // ライト方向編集用。正規化済み方向ベクトルをYaw/Pitch(度)に変換する。
        // DragFloat3で直接編集すると正規化のたびに値が跳ねて操作しづらいため、角度で編集する。
        // Yawは水平面内の角度(X軸を0度、Z軸を90度)、Pitchは水平面からの仰角(下向きが負)
        void DirectionToYawPitch(const float direction[3], float& outYawDegrees, float& outPitchDegrees)
        {
            outYawDegrees = DirectX::XMConvertToDegrees(std::atan2(direction[2], direction[0]));
            outPitchDegrees = DirectX::XMConvertToDegrees(std::asin(std::clamp(direction[1], -1.0f, 1.0f)));
        }

        void YawPitchToDirection(float yawDegrees, float pitchDegrees, float outDirection[3])
        {
            const float yaw = DirectX::XMConvertToRadians(yawDegrees);
            const float pitch = DirectX::XMConvertToRadians(pitchDegrees);
            outDirection[0] = std::cos(pitch) * std::cos(yaw);
            outDirection[1] = std::sin(pitch);
            outDirection[2] = std::cos(pitch) * std::sin(yaw);
        }
    }

    void LightingPanel::Draw(const PanelDrawContext& context)
    {
        if (!ImGui::Begin(GetWindowName(), GetVisiblePtr()))
        {
            ImGui::End();
            return;
        }

        DrawUsageHint();

        if (ImGui::CollapsingHeader("太陽###Sun", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawSunSection();
        }

        if (ImGui::CollapsingHeader("ライト###Lights", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawLightsSection(context);
        }

        ImGui::End();
    }

    void LightingPanel::DrawSunSection()
    {
        BeginParamGroup();

        SliderFloatEx(
            "時刻###TimeOfDay", &m_Engine.m_TimeOfDay, 0.0f, 24.0f, Defaults::TimeOfDay, "%.2f h", 0,
            "1日のうちの時刻。太陽高度と空の色がこれで決まる");
        CheckboxEx(
            "自動で進める###AutoAdvance", &m_Engine.m_TimeAutoAdvance, Defaults::TimeAutoAdvance,
            "時刻を実時間に応じて自動で進める");
        if (m_Engine.m_TimeAutoAdvance)
        {
            SliderFloatEx(
                "進む速さ###TimeSpeed", &m_Engine.m_TimeAdvanceSpeed, 0.1f, 10.0f, Defaults::TimeAdvanceSpeed,
                "%.1f h/s", 0, "実時間1秒あたりに進むシーン内の時間");
        }
        SliderFloatEx(
            "方位角###SunAzimuth", &m_Engine.m_SunAzimuthDegrees, 0.0f, 360.0f, Defaults::SunAzimuthDegrees, "%.1f deg",
            0, "太陽が昇る方位。時刻と組み合わせて太陽の向きが決まる");
        // 月の位置は時刻に連動しない(実際の月は太陽と独立した周期で動くため)。平行光源の枠は
        // 太陽と共有しており、太陽が沈むと支配ライトが月へ切り替わる。
        // 月を動かすと夜空の目標照度が変わるため、空を焼き直す必要がある
        bool moonMoved = SliderFloatEx(
            "月の方位角###MoonAzimuth", &m_Engine.m_MoonAzimuthDegrees, 0.0f, 360.0f, Defaults::MoonAzimuthDegrees,
            "%.1f deg", 0, "月の方位。時刻に連動しないため、任意の月齢・任意の時刻の見え方を作れる");
        moonMoved |= SliderFloatEx(
            "月の仰角###MoonElevation", &m_Engine.m_MoonElevationDegrees, -90.0f, 90.0f,
            Defaults::MoonElevationDegrees, "%.1f deg", 0, "月の高さ。0度以下なら地平線下にあり月光は出ない");
        if (moonMoved)
        {
            m_Engine.m_SkyBakeDirty = true;
        }

        // 太陽だけを消して環境光のみで照らす状態を作る(White Furnace Testが使う)。
        // 時刻を夜にする方法と違い、環境光の明るさは下がらない
        CheckboxEx(
            "太陽光を有効にする###EnableSun", &m_Engine.m_SunEnabled, Defaults::SunEnabled,
            "無効にすると環境光だけで照らした状態になる。時刻を夜にする方法と違い、環境光の明るさは下がらない");

        // 手続き空(Perez分布をGPUで評価)。無効にするとオフラインで焼いたSky.ddsへ戻る。
        // .ksceneがスカイボックスを明示しているシーン(White Furnace Test)では、
        // このトグルに関わらず常にそのDDSが使われる
        if (CheckboxEx(
                "手続き空###ProceduralSky", &m_Engine.m_ProceduralSkyEnabled, Defaults::ProceduralSkyEnabled,
                "空をGPU上で毎回生成する。無効にするとオフラインで焼いたSky.ddsを使う。"
                "スカイボックスを明示しているシーンでは、この設定に関わらずそのテクスチャが使われる"))
        {
            m_Engine.m_SkyBakeDirty = true;
            m_Engine.m_IBLBaked = false;
            m_Engine.m_IBLIrradianceBaked = false;
        }
        // タービディティ(P7: Preetham xyYモデルの大気の濁り具合)。変更時にm_SkyBakeDirtyを
        // 直接ここで立てず、Render()側のturbidityMoved判定(exposureMovedと同じ形)に任せる
        SliderFloatEx(
            "タービディティ###SkyTurbidity", &m_Engine.m_SkyTurbidity, 1.7f, 10.0f, Defaults::SkyTurbidity, "%.2f",
            0,
            "Preethamモデルの大気の濁り具合。値が大きいほど地平線が白く霞み、天頂の青が薄くなる。"
            "1.7が最も澄んだ空、10が霞んだ空に近い");
        // 空の彩度(アート指定)。タービディティと同じくPreethamの色度を動かすため、
        // Render()側のsaturationMoved判定でベイクが焼き直される
        SliderFloatEx(
            "空の彩度###SkySaturation", &m_Engine.m_SkySaturation, 0.0f, 2.0f, Defaults::SkySaturation, "%.2f",
            0,
            "物理量ではないアート指定。1.0でPreethamの色度そのまま、上げるほど空が鮮やかになる"
            "(色度図上で白色点から遠ざける倍率なので色相は変わらない)。"
            "実測した参考写真の空はPreethamがどのタービディティでも出せない深さ(B/R=4.84)にあり、"
            "その差を埋めるためのつまみ。2.0を超えると色域外へ出て赤成分が潰れる");
        // 背景の解析評価(P3)。キューブマップの中身(SkyGenerateのベイク結果)には一切影響しない
        // 表示の切り替えでしかないため、上の「手続き空」トグルと違ってm_SkyBakeDirty等の
        // ベイク用フラグは立てない
        CheckboxEx(
            "空の背景を解析評価する###AnalyticSkyBackground", &m_Engine.m_SkyAnalyticBackground,
            Defaults::SkyAnalyticBackground,
            "背景(深度が無い画素)をキューブマップのサンプルではなく、Perez分布を画面解像度で"
            "直接評価して描く。キューブマップは256px/面しかなく背景としては拡大表示されるため、"
            "こちらのほうが空の輪郭がシャープになる。IBL(反射プローブ・拡散イラディアンス)は"
            "常にキューブマップのままで、この設定の影響を受けない。手続き空が無効なときは、"
            "この設定に関わらず常にキューブマップが使われる");
        SliderFloatEx(
            "EV100###SceneExposure", &m_Engine.m_SceneExposureEV100, -8.0f, 20.0f, Defaults::SceneExposureEV100, "%.2f",
            0,
            "実在の写真露出値。太陽・環境光・ポイント/スポットライトすべてに一様にかかるシーン全体の露出。"
            "自動露出が有効なときは、バッファの数値レンジを決める基準値として働く");
        SliderFloatEx(
            "自発光の強度###EmissiveIntensity", &m_Engine.m_EmissiveIntensity, 0.0f, 64.0f, Defaults::EmissiveIntensity,
            "%.2fx", ImGuiSliderFlags_Logarithmic,
            "シーン全体の自発光にかける倍率。glTFのemissiveFactorは通常1.0以下に収まるため、"
            "アセットを作り直さずにHDRな自発光(照明器具のにじみ)を作るための倍率");

        EndParamGroup();
    }

    void LightingPanel::DrawLightsSection(const PanelDrawContext& context)
    {
        uint32_t activeCount = 0;
        for (const Assets::Light& light : m_Engine.m_Lights)
        {
            if (light.Enabled)
            {
                ++activeCount;
            }
        }
        ImGui::Text("有効: %u / %zu", activeCount, m_Engine.m_Lights.size());

        // 有効チェック・種別・名前を列に揃える。BeginChildで縦に並べるより読みやすい
        const float listHeight = ImGui::GetTextLineHeightWithSpacing() * 6.0f;
        if (ImGui::BeginTable(
                "LightList", 3,
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                    ImGuiTableFlags_SizingFixedFit,
                ImVec2(0.0f, listHeight)))
        {
            ImGui::TableSetupColumn("##enabled", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
            ImGui::TableSetupColumn("種別", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.5f);
            ImGui::TableSetupColumn("名前", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < m_Engine.m_Lights.size(); ++i)
            {
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(i));

                ImGui::TableSetColumnIndex(0);
                ImGui::Checkbox("##enabled", &m_Engine.m_Lights[i].Enabled);

                ImGui::TableSetColumnIndex(1);
                const char* typeLabel = m_Engine.m_Lights[i].Type == Assets::LightType::Directional ? "平行光"
                                       : m_Engine.m_Lights[i].Type == Assets::LightType::Spot       ? "スポット"
                                                                                                    : "ポイント";
                ImGui::TextUnformatted(typeLabel);

                ImGui::TableSetColumnIndex(2);
                const char* name =
                    m_Engine.m_Lights[i].Name.empty() ? "(名前なし)" : m_Engine.m_Lights[i].Name.c_str();
                if (ImGui::Selectable(
                        name, m_Engine.m_SelectedLightIndex == static_cast<int>(i), ImGuiSelectableFlags_SpanAllColumns))
                {
                    m_Engine.m_SelectedLightIndex = static_cast<int>(i);
                }

                ImGui::PopID();
            }

            ImGui::EndTable();
        }

        if (ImGui::Button("追加"))
        {
            Assets::Light newLight;
            if (context.Camera != nullptr)
            {
                const DirectX::XMFLOAT3 cameraPosition = context.Camera->GetPosition();
                newLight.Position[0] = cameraPosition.x;
                newLight.Position[1] = cameraPosition.y;
                newLight.Position[2] = cameraPosition.z;
            }
            newLight.Name = "New Light";
            m_Engine.m_Lights.push_back(newLight);
            m_Engine.m_SelectedLightIndex = static_cast<int>(m_Engine.m_Lights.size()) - 1;
            // インデックスが同じまま別のライトを指す場合があるため、選択の変化だけでは
            // 名前バッファの同期判定に足りない。ここで強制的に詰め直させる
            m_NameBufferLightIndex = -1;
        }
        ItemHelp("現在のカメラ位置に新しいポイントライトを追加する");

        const bool hasSelection = m_Engine.m_SelectedLightIndex >= 0 &&
                                  m_Engine.m_SelectedLightIndex < static_cast<int>(m_Engine.m_Lights.size());

        ImGui::SameLine();
        ImGui::BeginDisabled(!hasSelection);
        if (ImGui::Button("複製") && hasSelection)
        {
            Assets::Light duplicated = m_Engine.m_Lights[static_cast<size_t>(m_Engine.m_SelectedLightIndex)];
            duplicated.Name += " (Copy)";
            m_Engine.m_Lights.push_back(duplicated);
            m_Engine.m_SelectedLightIndex = static_cast<int>(m_Engine.m_Lights.size()) - 1;
            m_NameBufferLightIndex = -1;
        }
        ImGui::SameLine();
        if (ImGui::Button("削除") && hasSelection)
        {
            m_Engine.m_Lights.erase(m_Engine.m_Lights.begin() + m_Engine.m_SelectedLightIndex);
            m_Engine.m_SelectedLightIndex =
                m_Engine.m_Lights.empty()
                    ? -1
                    : std::min(m_Engine.m_SelectedLightIndex, static_cast<int>(m_Engine.m_Lights.size()) - 1);
            m_NameBufferLightIndex = -1;
        }
        ImGui::EndDisabled();

        if (hasSelection)
        {
            ImGui::Separator();
            DrawSelectedLightEditor();
        }
    }

    void LightingPanel::DrawSelectedLightEditor()
    {
        Assets::Light& light = m_Engine.m_Lights[static_cast<size_t>(m_Engine.m_SelectedLightIndex)];

        BeginParamGroup();

        if (m_NameBufferLightIndex != m_Engine.m_SelectedLightIndex)
        {
            m_NameBufferLightIndex = m_Engine.m_SelectedLightIndex;
            std::snprintf(m_NameBuffer.data(), m_NameBuffer.size(), "%s", light.Name.c_str());
        }
        if (ImGui::InputText("名前###LightName", m_NameBuffer.data(), m_NameBuffer.size()))
        {
            light.Name = m_NameBuffer.data();
        }

        int typeIndex = static_cast<int>(light.Type);
        const char* typeItems[] = { "平行光", "ポイント", "スポット" };
        if (ImGui::Combo("種別###LightType", &typeIndex, typeItems, IM_ARRAYSIZE(typeItems)))
        {
            light.Type = static_cast<Assets::LightType>(typeIndex);
        }
        ItemHelp("平行光は向きだけを持ち距離減衰しない。ポイントは全方向、スポットは円錐状に照らす");

        ImGui::ColorEdit3("色###LightColor", light.Color);
        ImGui::SliderFloat(
            "強度###LightIntensity", &light.Intensity, 0.01f, 1000000.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ItemHelp("ポイント/スポットはカンデラ、平行光はルクス。実在の照明と同じ単位なので値の桁が大きくなる");

        if (light.Type != Assets::LightType::Directional)
        {
            ImGui::DragFloat3("位置###LightPosition", light.Position, 0.1f);
            ImGui::SliderFloat("届く距離###LightRange", &light.Range, 0.1f, 500.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            ItemHelp("この距離を超えると寄与を打ち切る。物理的な減衰とは別の、描画負荷のための上限");
        }

        if (light.Type != Assets::LightType::Point)
        {
            float yawDegrees = 0.0f;
            float pitchDegrees = 0.0f;
            DirectionToYawPitch(light.Direction, yawDegrees, pitchDegrees);
            bool directionChanged = false;
            directionChanged |= ImGui::SliderFloat("向き 方位角###LightYaw", &yawDegrees, -180.0f, 180.0f, "%.1f deg");
            directionChanged |= ImGui::SliderFloat("向き 仰角###LightPitch", &pitchDegrees, -89.0f, 89.0f, "%.1f deg");
            if (directionChanged)
            {
                YawPitchToDirection(yawDegrees, pitchDegrees, light.Direction);
            }
        }

        if (light.Type == Assets::LightType::Spot)
        {
            float innerDegrees = DirectX::XMConvertToDegrees(light.SpotInnerConeAngle);
            float outerDegrees = DirectX::XMConvertToDegrees(light.SpotOuterConeAngle);
            ImGui::SliderFloat("内側の角度###SpotInner", &innerDegrees, 0.0f, 89.0f, "%.1f deg");
            ItemHelp("この角度の内側は減衰なし。外側の角度までの間で滑らかに落ちる");
            ImGui::SliderFloat("外側の角度###SpotOuter", &outerDegrees, 0.0f, 90.0f, "%.1f deg");
            if (innerDegrees > outerDegrees)
            {
                innerDegrees = outerDegrees;
            }
            light.SpotInnerConeAngle = DirectX::XMConvertToRadians(innerDegrees);
            light.SpotOuterConeAngle = DirectX::XMConvertToRadians(outerDegrees);
        }

        // このライトがスクリーンスペースシャドウを落とすか。ピクセルあたりのシャドウレイ数には
        // 上限(レンダリングパネルの「影を落とすライト数の上限」)があるため、
        // 影を出したいライトへ予算を回すのに使う
        ImGui::Checkbox("影を落とす###LightCastShadow", &light.CastShadow);
        ItemHelp(
            "このライトにスクリーンスペースシャドウを適用する。ピクセルあたりのレイ本数には上限があるため、"
            "影を出したいライトだけに絞ると負荷を抑えられる");

        EndParamGroup();
    }
}
