#include "ShowEditor.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <exception>

#include <imgui.h>

#include "Core/StringUtil.h"
#include "KurenaiEngine3D.h"

namespace Kurenai::ShowEditor
{
    namespace
    {
        using Core::Utf8ToWide;
        using Core::WideToUtf8;

        // GPU側の構造化バッファ(KurenaiEngine3D.cppのkMaxDrones)の固定容量。
        // これを超える機体数のショーを書いても、エンジンは超過分を描かない
        constexpr int kMaxDrones = 4096;
    }

    Assets::ShowData BuildStandardShow(uint32_t droneCount)
    {
        Assets::ShowData data{};
        data.DroneCount = droneCount;
        // 既定値の意図はShowLoader.h / ShowPackage.hのコメントを参照。
        // 各値の実測に基づく根拠はdocs/ImplementationDetail.md 38.6節・38.9節
        data.Speed = 1.0f;
        data.HoldSeconds = 6.0f;
        data.MorphSeconds = 4.0f;
        data.Brightness = 1.0f;
        data.Radius = 4.0f;
        data.HoverAmplitude = 0.6f;
        data.Seed = 20260804u;

        for (uint32_t k = 0; k < static_cast<uint32_t>(GeneratorKind::Count); ++k)
        {
            const GeneratorKind kind = static_cast<GeneratorKind>(k);
            Assets::ShowFormation formation;
            formation.Name = GeneratorName(kind);
            GenerateFormation(kind, droneCount, formation.Positions);
            PaintByHeight(formation.Positions, DefaultPalette(kind), formation.Colors);
            data.Formations.push_back(std::move(formation));
        }
        return data;
    }

    Editor::Editor(KurenaiEngine3D& engine, std::wstring initialPath)
        : m_Engine(engine), m_PathUtf8(WideToUtf8(initialPath))
    {
        // 起動時に指定のファイルがあれば開き、無ければ標準の6形状で始める。
        // 「空のエディタ」から始めても最初にやることは決まっているため
        try
        {
            FromShowData(Assets::LoadShow(initialPath));
            m_Status = "読み込みました: " + m_PathUtf8;
            m_StatusIsError = false;
        }
        catch (const std::exception&)
        {
            FromShowData(BuildStandardShow(static_cast<uint32_t>(m_DroneCount)));
            m_Status = "既存のファイルが無いため、標準の6形状で開始しました";
            m_StatusIsError = false;
        }
    }

    Assets::ShowData Editor::ToShowData() const
    {
        Assets::ShowData data{};
        data.DroneCount = static_cast<uint32_t>(m_DroneCount);
        data.Speed = m_Speed;
        data.HoldSeconds = m_HoldSeconds;
        data.MorphSeconds = m_MorphSeconds;
        data.Brightness = m_Brightness;
        data.Radius = m_Radius;
        data.HoverAmplitude = m_HoverAmplitude;
        data.Seed = static_cast<uint32_t>(m_Seed);
        for (const EditorFormation& source : m_Formations)
        {
            Assets::ShowFormation formation;
            formation.Name = source.Name;
            formation.Positions = source.Positions;
            formation.Colors = source.Colors;
            data.Formations.push_back(std::move(formation));
        }
        return data;
    }

    void Editor::FromShowData(const Assets::ShowData& data)
    {
        m_DroneCount = static_cast<int>(std::clamp<uint32_t>(data.DroneCount, 1u, kMaxDrones));
        m_Speed = data.Speed;
        m_HoldSeconds = data.HoldSeconds;
        m_MorphSeconds = data.MorphSeconds;
        m_Brightness = data.Brightness;
        m_Radius = data.Radius;
        m_HoverAmplitude = data.HoverAmplitude;
        m_Seed = static_cast<int>(data.Seed);

        m_Formations.clear();
        for (const Assets::ShowFormation& source : data.Formations)
        {
            EditorFormation formation;
            formation.Name = source.Name;
            formation.Positions = source.Positions;
            formation.Colors = source.Colors;
            // 生成器は復元できない。名前が標準の形と一致する場合だけ引き当てる
            // (BuildStandardShowが編隊名に生成器名をそのまま使っているため、
            //  自分で書いたファイルを読み直したときは生成器が戻る)
            for (uint32_t k = 0; k < static_cast<uint32_t>(GeneratorKind::Count); ++k)
            {
                const GeneratorKind kind = static_cast<GeneratorKind>(k);
                if (formation.Name == GeneratorName(kind))
                {
                    formation.Kind = kind;
                    formation.Palette = DefaultPalette(kind);
                    break;
                }
            }
            m_Formations.push_back(std::move(formation));
        }

        m_Selected = 0;
        m_Dirty = true;
    }

    void Editor::ApplyToEngine()
    {
        m_Engine.ApplyDroneShowData(ToShowData());
    }

    void Editor::ResizeAllFormations()
    {
        const uint32_t count = static_cast<uint32_t>(m_DroneCount);
        for (EditorFormation& formation : m_Formations)
        {
            if (formation.Kind != GeneratorKind::Count)
            {
                // 生成器が分かっているなら撒き直すのが正しい。添字の取り直しでは
                // 元の分布(フィボナッチ球の等間隔など)が崩れる
                GenerateFormation(formation.Kind, count, formation.Positions);
            }
            else
            {
                std::vector<DirectX::XMFLOAT3> resampled;
                ResamplePoints(formation.Positions, count, resampled);
                formation.Positions = std::move(resampled);
            }
            PaintByHeight(formation.Positions, formation.Palette, formation.Colors);
        }
    }

    void Editor::DrawFileSection()
    {
        char pathBuffer[512];
        std::snprintf(pathBuffer, sizeof(pathBuffer), "%s", m_PathUtf8.c_str());
        if (ImGui::InputText("ファイル###ShowPath", pathBuffer, sizeof(pathBuffer)))
        {
            m_PathUtf8 = pathBuffer;
        }

        if (ImGui::Button("開く###ShowOpen"))
        {
            try
            {
                FromShowData(Assets::LoadShow(Utf8ToWide(m_PathUtf8)));
                m_Status = "読み込みました: " + m_PathUtf8;
                m_StatusIsError = false;
            }
            catch (const std::exception& e)
            {
                m_Status = std::string("読み込みに失敗しました: ") + e.what();
                m_StatusIsError = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("保存###ShowSave"))
        {
            try
            {
                Assets::SaveShow(Utf8ToWide(m_PathUtf8), ToShowData());
                m_Status = "保存しました: " + m_PathUtf8;
                m_StatusIsError = false;
            }
            catch (const std::exception& e)
            {
                m_Status = std::string("保存に失敗しました: ") + e.what();
                m_StatusIsError = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("標準の6形状を生成###ShowStandard"))
        {
            FromShowData(BuildStandardShow(static_cast<uint32_t>(m_DroneCount)));
            m_Status = "標準の6形状を生成しました";
            m_StatusIsError = false;
        }

        if (!m_Status.empty())
        {
            const ImVec4 color = m_StatusIsError ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(0.6f, 0.85f, 0.6f, 1.0f);
            ImGui::TextColored(color, "%s", m_Status.c_str());
        }
    }

    void Editor::DrawShowSection()
    {
        if (ImGui::SliderInt("機体数###ShowDroneCount", &m_DroneCount, 1, kMaxDrones))
        {
            // 【全編隊を同時に揃える】1つでも点数が違うとモーフの途中で機体が消える。
            // 揃える責任はエディタにあり、書き出し時にShowLoaderが最後の関所として検査する
            ResizeAllFormations();
            m_Dirty = true;
        }
        ImGui::SetItemTooltip("全編隊が同じ点数を持つ。変えると全編隊が作り直される\n上限4096はGPU側の構造化バッファの固定容量");

        m_Dirty |= ImGui::SliderFloat("再生速度###ShowSpeed", &m_Speed, 0.0f, 8.0f, "%.2f");
        m_Dirty |= ImGui::SliderFloat("保持時間[秒]###ShowHold", &m_HoldSeconds, 0.0f, 30.0f, "%.1f");
        m_Dirty |= ImGui::SliderFloat("変形時間[秒]###ShowMorph", &m_MorphSeconds, 0.1f, 30.0f, "%.1f");
        m_Dirty |= ImGui::SliderFloat("明るさ###ShowBrightness", &m_Brightness, 0.0f, 4.0f, "%.2f");
        ImGui::SetItemTooltip(
            "実効プリ露出が掛かった後の値。夜のシーンでは表示できる上限が2^-12段ぶんで頭打ちになり、\n"
            "そこを超えると明るくならずに色が白へ飛ぶだけになる(実測はdocs/ImplementationDetail.md 38.9節)");
        m_Dirty |= ImGui::SliderFloat("機体の半径[m]###ShowRadius", &m_Radius, 0.1f, 20.0f, "%.2f");
        ImGui::SetItemTooltip("ワールドの実寸。編隊の大きさ(シーンのScale)を変えても機体は太らない");
        m_Dirty |= ImGui::SliderFloat("揺れの振幅[m]###ShowHover", &m_HoverAmplitude, 0.0f, 10.0f, "%.2f");
        ImGui::SetItemTooltip("0で完全に静止する。全機が数学的に完全な位置で止まると模型のように見える");
        m_Dirty |= ImGui::InputInt("種###ShowSeed", &m_Seed);
        ImGui::SetItemTooltip("揺れと出発タイミングのばらつきを決める。固定しておけば毎回同じ絵になる");

        const Assets::ShowData preview = ToShowData();
        ImGui::Text("1巡: %.1f秒 (再生速度を含めると %.1f秒)",
                    Assets::ShowLoopDuration(preview),
                    m_Speed > 0.0f ? Assets::ShowLoopDuration(preview) / m_Speed : 0.0f);
    }

    void Editor::DrawFormationListSection()
    {
        if (ImGui::BeginListBox("###ShowFormationList", ImVec2(-FLT_MIN, 6.0f * ImGui::GetTextLineHeightWithSpacing())))
        {
            for (int i = 0; i < static_cast<int>(m_Formations.size()); ++i)
            {
                char label[256];
                std::snprintf(label, sizeof(label), "%d: %s###ShowFormation%d", i + 1, m_Formations[i].Name.c_str(), i);
                if (ImGui::Selectable(label, m_Selected == i))
                {
                    m_Selected = i;
                }
            }
            ImGui::EndListBox();
        }

        const bool hasSelection = m_Selected >= 0 && m_Selected < static_cast<int>(m_Formations.size());

        if (ImGui::Button("上へ###ShowMoveUp") && hasSelection && m_Selected > 0)
        {
            std::swap(m_Formations[m_Selected], m_Formations[m_Selected - 1]);
            --m_Selected;
            m_Dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("下へ###ShowMoveDown") && hasSelection &&
            m_Selected + 1 < static_cast<int>(m_Formations.size()))
        {
            std::swap(m_Formations[m_Selected], m_Formations[m_Selected + 1]);
            ++m_Selected;
            m_Dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("削除###ShowRemove") && hasSelection)
        {
            m_Formations.erase(m_Formations.begin() + m_Selected);
            m_Selected = std::max(0, m_Selected - 1);
            m_Dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("追加###ShowAdd"))
        {
            ImGui::OpenPopup("###ShowAddPopup");
        }

        if (ImGui::BeginPopup("###ShowAddPopup"))
        {
            for (uint32_t k = 0; k < static_cast<uint32_t>(GeneratorKind::Count); ++k)
            {
                const GeneratorKind kind = static_cast<GeneratorKind>(k);
                if (ImGui::MenuItem(GeneratorName(kind)))
                {
                    EditorFormation formation;
                    formation.Name = GeneratorName(kind);
                    formation.Kind = kind;
                    formation.Palette = DefaultPalette(kind);
                    GenerateFormation(kind, static_cast<uint32_t>(m_DroneCount), formation.Positions);
                    PaintByHeight(formation.Positions, formation.Palette, formation.Colors);
                    m_Formations.push_back(std::move(formation));
                    m_Selected = static_cast<int>(m_Formations.size()) - 1;
                    m_Dirty = true;
                }
            }
            ImGui::EndPopup();
        }
    }

    void Editor::DrawSelectedFormationSection()
    {
        if (m_Selected < 0 || m_Selected >= static_cast<int>(m_Formations.size()))
        {
            ImGui::TextDisabled("編隊が選ばれていません");
            return;
        }
        EditorFormation& formation = m_Formations[m_Selected];

        char nameBuffer[128];
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", formation.Name.c_str());
        if (ImGui::InputText("名前###ShowFormationName", nameBuffer, sizeof(nameBuffer)))
        {
            formation.Name = nameBuffer;
            m_Dirty = true;
        }

        int kindIndex = static_cast<int>(formation.Kind);
        const bool imported = formation.Kind == GeneratorKind::Count;
        if (ImGui::BeginCombo("生成器###ShowFormationKind", GeneratorName(formation.Kind)))
        {
            for (uint32_t k = 0; k < static_cast<uint32_t>(GeneratorKind::Count); ++k)
            {
                const GeneratorKind kind = static_cast<GeneratorKind>(k);
                if (ImGui::Selectable(GeneratorName(kind), kindIndex == static_cast<int>(k)))
                {
                    formation.Kind = kind;
                    formation.Palette = DefaultPalette(kind);
                    GenerateFormation(kind, static_cast<uint32_t>(m_DroneCount), formation.Positions);
                    PaintByHeight(formation.Positions, formation.Palette, formation.Colors);
                    m_Dirty = true;
                }
            }
            ImGui::EndCombo();
        }
        if (imported)
        {
            ImGui::TextDisabled("この編隊は読み込んだ点データで、生成器が分かりません");
        }

        bool paletteChanged = false;
        paletteChanged |= ImGui::ColorEdit3("下端の色###ShowPaletteLow", &formation.Palette.Low.x);
        paletteChanged |= ImGui::ColorEdit3("上端の色###ShowPaletteHigh", &formation.Palette.High.x);
        if (paletteChanged || ImGui::Button("色を塗り直す###ShowRepaint"))
        {
            // 色は正規化空間での高さから決まるので、点さえあれば取り込みの編隊でも塗り直せる
            PaintByHeight(formation.Positions, formation.Palette, formation.Colors);
            m_Dirty = true;
        }
        ImGui::SetItemTooltip(
            "上端に白に近い色を置くと、加算合成とACESを通る過程で編隊がほぼ真っ白になり、\n"
            "色が変わるという見どころが消える。彩度を保つこと");

        ImGui::Text("点数: %zu", formation.Positions.size());
    }

    void Editor::Draw()
    {
        // ###以降がウィンドウIDになる。imgui.iniとドックレイアウトのキーになるため変更しないこと
        if (ImGui::Begin("ドローンショー編集###ShowEditor"))
        {
            DrawFileSection();
            ImGui::Separator();
            if (ImGui::CollapsingHeader("ショー全体###ShowGlobal", ImGuiTreeNodeFlags_DefaultOpen))
            {
                DrawShowSection();
            }
            if (ImGui::CollapsingHeader("編隊###ShowFormations", ImGuiTreeNodeFlags_DefaultOpen))
            {
                DrawFormationListSection();
                ImGui::Separator();
                DrawSelectedFormationSection();
            }
        }
        ImGui::End();

        // 【フレームの終わりに1回だけ送る】ウィジェットごとに送ると、スライダーを
        // 動かしている間に数千点のコピーが何度も走る
        if (m_Dirty)
        {
            ApplyToEngine();
            m_Dirty = false;
        }
    }
}
