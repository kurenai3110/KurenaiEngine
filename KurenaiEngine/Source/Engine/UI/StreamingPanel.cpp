#include "UI/StreamingPanel.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <DirectXMath.h>

#include "Core/Camera.h"
#include "Core/Logger.h"
#include "KurenaiEngine3D.h"
#include "UI/UIWidgets.h"

namespace Kurenai::UI
{
    namespace
    {
        // 常駐状態ごとの色。
        //
        // 【灰色を「未読み込み」にする】0件のときに何色も塗られないのではなく、
        // 「未読み込みが0件である」と読めるように凡例には常に3色すべてを出す。
        // 色だけで「一度も実行されていない」と「全部常駐している」を取り違えないため
        constexpr ImU32 kColorLoaded   = IM_COL32( 90, 200, 110, 200);  // 常駐 = 緑
        constexpr ImU32 kColorLoading  = IM_COL32(230, 200,  70, 220);  // 読み込み中 = 黄
        constexpr ImU32 kColorUnloaded = IM_COL32(120, 120, 130, 140);  // 未読み込み = 灰

        // LOD段ごとの明度。段が進む(粗くなる)ほど暗くする。
        // 上限を超える段はすべて最も暗い値に丸める(段数の上限はモデルLOD側が決めるため、
        // ここで頭打ちにしても表示が壊れない)
        constexpr std::array<float, 4> kLODBrightness = { 1.0f, 0.78f, 0.60f, 0.45f };

        ImU32 ResidencyColor(Assets::ResidencyState state, uint32_t lodLevel)
        {
            ImU32 base = kColorUnloaded;
            switch (state)
            {
            case Assets::ResidencyState::Loaded:   base = kColorLoaded;   break;
            case Assets::ResidencyState::Loading:  base = kColorLoading;  break;
            case Assets::ResidencyState::Unloaded: base = kColorUnloaded; break;
            }

            const size_t lodIndex = (std::min)(static_cast<size_t>(lodLevel), kLODBrightness.size() - 1);
            const float brightness = kLODBrightness[lodIndex];
            if (brightness >= 1.0f)
            {
                return base;
            }

            // アルファはそのまま残し、RGBだけを暗くする(段が進んでも「そこに何かある」ことは
            // 同じ濃さで見えていてほしいため)
            const auto scale = [brightness](ImU32 color, int shift)
            {
                const float channel = static_cast<float>((color >> shift) & 0xFFu) * brightness;
                return static_cast<ImU32>(std::lround(channel)) << shift;
            };
            return (base & IM_COL32_A_MASK) | scale(base, IM_COL32_R_SHIFT) | scale(base, IM_COL32_G_SHIFT) |
                   scale(base, IM_COL32_B_SHIFT);
        }

        // 目盛りに使う「きりのいい」長さ[m]を求める。1/2/5 × 10^n の中から
        // 与えられた上限を超えない最大のものを返す
        float NiceScaleLength(float maxLength)
        {
            if (!(maxLength > 0.0f))
            {
                return 1.0f;
            }
            const float exponent = std::floor(std::log10(maxLength));
            const float base = std::pow(10.0f, exponent);
            for (const float multiplier : { 5.0f, 2.0f, 1.0f })
            {
                if (base * multiplier <= maxLength)
                {
                    return base * multiplier;
                }
            }
            return base;
        }
    }

    void StreamingPanel::Draw(const PanelDrawContext& context)
    {
        if (!ImGui::Begin(GetWindowName(), GetVisiblePtr()))
        {
            // 折りたたまれていてもEnd()は必ず対で呼ぶ(ImGuiのBegin/Endの規約)
            ImGui::End();
            return;
        }

        const std::vector<Assets::ModelInstance>& instances = m_Engine.m_Scene.Instances;

        // --- 常駐状態ごとの件数 ---
        //
        // 【地図の前に数を出す】色が全部同じでも「1種類しか無い」のか「判定が動いていない」のかは
        // 見た目では区別できない。件数を先に出しておけば、0/非0が数字で分かる
        uint32_t loadedCount = 0;
        uint32_t loadingCount = 0;
        uint32_t unloadedCount = 0;
        uint32_t maxLODLevel = 0;
        for (const Assets::ModelInstance& instance : instances)
        {
            switch (instance.Residency)
            {
            case Assets::ResidencyState::Loaded:   ++loadedCount;   break;
            case Assets::ResidencyState::Loading:  ++loadingCount;  break;
            case Assets::ResidencyState::Unloaded: ++unloadedCount; break;
            }
            maxLODLevel = (std::max)(maxLODLevel, instance.LODLevel);
        }

        ImGui::SeparatorText("常駐状態");
        ImGui::Text(
            "インスタンス %zu 件 (常駐 %u / 読み込み中 %u / 未読み込み %u)  最大LOD段 %u",
            instances.size(), loadedCount, loadingCount, unloadedCount, maxLODLevel);
        ItemHelp(
            "[Scene]StreamingDistanceを書いたシーンだけが動く。書かなければ全件が読み込み時から"
            "「常駐」のまま変化しない(従来どおりの全常駐)。"
            "LOD段は[Model]LODPath/LODDistanceを書いたインスタンスだけが0以外になる");

        // --- 俯瞰(XZ平面)の地図 ---
        //
        // 【北を上にする】画面の横=ワールドX(東が右)、画面の縦=ワールドZを反転(北が上)。
        // PLATEAUの取り込みは X=東 / Y=標高 / Z=北 なので、これで地図と同じ向きになる
        ImGui::SeparatorText("俯瞰図 (上が +Z / 右が +X)");

        BeginParamGroup();
        SliderFloatEx(
            "拡大率###StreamingMapZoom", &m_MapZoom, 1.0f, 200.0f, 1.0f, "%.1f x", ImGuiSliderFlags_Logarithmic,
            "1.0でシーン全体がちょうど収まる。上げると中心のまわりを拡大する");
        CheckboxEx(
            "カメラに追従###StreamingMapFollowCamera", &m_FollowCamera, false,
            "オンにするとカメラ位置を地図の中心に置き続ける。"
            "32km四方のシーンを拡大して見るときは、追従していないとすぐ画面外へ出る");
        EndParamGroup();

        const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        // 縦が潰れているドック配置でも最低限の高さを確保する。横も同様
        canvasSize.x = (std::max)(canvasSize.x, 120.0f);
        canvasSize.y = (std::max)(canvasSize.y, 120.0f);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        if (drawList == nullptr)
        {
            // ImGuiのウィンドウ内なら常に取れるが、取れない場合に黙って何も描かないと
            // 「地図が空 = 全部未読み込み」と誤読されるためログに残す
            Core::Logger::Error("StreamingPanel", "ImDrawListを取得できないため俯瞰図を描けません");
            ImGui::End();
            return;
        }

        const ImVec2 canvasEnd(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y);
        drawList->AddRectFilled(canvasPos, canvasEnd, IM_COL32(24, 26, 30, 255));
        drawList->AddRect(canvasPos, canvasEnd, IM_COL32(90, 95, 105, 255));

        if (instances.empty())
        {
            // シーンが空(読み込み中・読み込み失敗)のときは、地図を描かずにそう書く。
            // 空の枠だけを出すと「全部未読み込み」と取り違えられる
            ImGui::Dummy(canvasSize);
            ImGui::TextDisabled("シーンにモデルインスタンスがありません");
            ImGui::End();
            return;
        }

        // シーンAABBのXZから、正方形の表示範囲を作る。長辺に合わせることで縦横比を保つ
        // (合わせないと東西に長いシーンが縦に引き伸ばされて、タイルの並びが読めなくなる)
        const float sceneSizeX = m_Engine.m_Scene.BoundsMax[0] - m_Engine.m_Scene.BoundsMin[0];
        const float sceneSizeZ = m_Engine.m_Scene.BoundsMax[2] - m_Engine.m_Scene.BoundsMin[2];
        // 1点しか無いシーン(サイズ0)でもゼロ除算しないよう下限を置く
        const float sceneExtent = (std::max)({ sceneSizeX, sceneSizeZ, 0.001f });
        const float viewExtent = sceneExtent / (std::max)(m_MapZoom, 0.001f);

        float centerX = (m_Engine.m_Scene.BoundsMin[0] + m_Engine.m_Scene.BoundsMax[0]) * 0.5f;
        float centerZ = (m_Engine.m_Scene.BoundsMin[2] + m_Engine.m_Scene.BoundsMax[2]) * 0.5f;
        if (m_FollowCamera && context.Camera != nullptr)
        {
            centerX = context.Camera->GetPosition().x;
            centerZ = context.Camera->GetPosition().z;
        }

        // 正方形の表示範囲をキャンバスの短辺へ収め、余りは中央に置く
        const float canvasShortSide = (std::min)(canvasSize.x, canvasSize.y);
        const float pixelsPerMeter = canvasShortSide / viewExtent;
        const ImVec2 canvasCenter(canvasPos.x + canvasSize.x * 0.5f, canvasPos.y + canvasSize.y * 0.5f);

        // ワールドXZ → 画面座標。Zは反転する(北を上にするため)
        const auto toScreen = [&](float worldX, float worldZ)
        {
            return ImVec2(
                canvasCenter.x + (worldX - centerX) * pixelsPerMeter,
                canvasCenter.y - (worldZ - centerZ) * pixelsPerMeter);
        };

        drawList->PushClipRect(canvasPos, canvasEnd, true);

        // シーン全体のAABBの枠。拡大したときに「いまシーンのどのあたりを見ているか」の手がかりになる
        drawList->AddRect(
            toScreen(m_Engine.m_Scene.BoundsMin[0], m_Engine.m_Scene.BoundsMax[2]),
            toScreen(m_Engine.m_Scene.BoundsMax[0], m_Engine.m_Scene.BoundsMin[2]),
            IM_COL32(70, 75, 85, 255));

        for (const Assets::ModelInstance& instance : instances)
        {
            const ImVec2 topLeft = toScreen(instance.WorldBoundsMin[0], instance.WorldBoundsMax[2]);
            const ImVec2 bottomRight = toScreen(instance.WorldBoundsMax[0], instance.WorldBoundsMin[2]);

            // 縮尺によっては1タイルが1px未満になる。AddRectFilledは幅0の矩形を描かないため、
            // 最低1pxを確保する(そうしないと引きの絵で常駐タイルが丸ごと消えて見える)
            const ImVec2 clampedBottomRight(
                (std::max)(bottomRight.x, topLeft.x + 1.0f), (std::max)(bottomRight.y, topLeft.y + 1.0f));

            drawList->AddRectFilled(topLeft, clampedBottomRight, ResidencyColor(instance.Residency, instance.LODLevel));
        }

        // カメラの位置と向き。位置は円、向きは前方ベクトルのXZ成分をそのまま線にする
        // (Yawから三角関数で組み直すと規約を取り違えうるので、Camera::GetForwardをそのまま使う)
        if (context.Camera != nullptr)
        {
            const DirectX::XMFLOAT3& cameraPosition = context.Camera->GetPosition();
            const DirectX::XMFLOAT3 forward = context.Camera->GetForward();
            const ImVec2 cameraScreen = toScreen(cameraPosition.x, cameraPosition.z);

            const float forwardLengthXZ = std::sqrt(forward.x * forward.x + forward.z * forward.z);
            if (forwardLengthXZ > 1e-4f)
            {
                // 真下/真上を向いているとXZ成分がほぼ0になる。そのときは向きの線を描かない
                // (0除算で線があらぬ方向へ飛ぶより、出さないほうが誤読が無い)
                constexpr float kHeadingLengthPixels = 22.0f;
                const ImVec2 headingEnd(
                    cameraScreen.x + forward.x / forwardLengthXZ * kHeadingLengthPixels,
                    cameraScreen.y - forward.z / forwardLengthXZ * kHeadingLengthPixels);
                drawList->AddLine(cameraScreen, headingEnd, IM_COL32(255, 90, 90, 255), 2.0f);
            }
            drawList->AddCircleFilled(cameraScreen, 4.0f, IM_COL32(255, 90, 90, 255));
        }

        drawList->PopClipRect();

        // 縮尺の目盛り。「地図に見えるが何メートルか分からない」状態を避けるために必ず出す
        {
            const float maxBarMeters = viewExtent * 0.3f;
            const float barMeters = NiceScaleLength(maxBarMeters);
            const float barPixels = barMeters * pixelsPerMeter;
            const ImVec2 barStart(canvasPos.x + 10.0f, canvasEnd.y - 14.0f);
            const ImVec2 barEnd(barStart.x + barPixels, barStart.y);
            drawList->AddLine(barStart, barEnd, IM_COL32(220, 220, 230, 255), 2.0f);
            drawList->AddLine(
                ImVec2(barStart.x, barStart.y - 4.0f), ImVec2(barStart.x, barStart.y + 4.0f),
                IM_COL32(220, 220, 230, 255), 2.0f);
            drawList->AddLine(
                ImVec2(barEnd.x, barEnd.y - 4.0f), ImVec2(barEnd.x, barEnd.y + 4.0f),
                IM_COL32(220, 220, 230, 255), 2.0f);

            char scaleText[64];
            if (barMeters >= 1000.0f)
            {
                std::snprintf(scaleText, sizeof(scaleText), "%.0f km", barMeters / 1000.0f);
            }
            else
            {
                std::snprintf(scaleText, sizeof(scaleText), "%.0f m", barMeters);
            }
            drawList->AddText(ImVec2(barStart.x, barStart.y - 20.0f), IM_COL32(220, 220, 230, 255), scaleText);
        }

        // キャンバスぶんの領域を消費して、この後のウィジェットが重ならないようにする
        ImGui::Dummy(canvasSize);

        // 凡例。3色すべてを常に出す(件数0の状態と、そもそも判定が動いていない状態を取り違えないため)
        const auto legendEntry = [](ImU32 color, const char* label)
        {
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            const float size = ImGui::GetTextLineHeight();
            ImDrawList* legendDrawList = ImGui::GetWindowDrawList();
            if (legendDrawList != nullptr)
            {
                legendDrawList->AddRectFilled(cursor, ImVec2(cursor.x + size, cursor.y + size), color);
            }
            ImGui::Dummy(ImVec2(size, size));
            ImGui::SameLine();
            ImGui::TextUnformatted(label);
        };
        legendEntry(kColorLoaded, "常駐");
        ImGui::SameLine();
        legendEntry(kColorLoading, "読み込み中");
        ImGui::SameLine();
        legendEntry(kColorUnloaded, "未読み込み");
        ImGui::TextDisabled("LOD段が進むほど暗くなる(赤い点と線はカメラの位置と向き)");

        ImGui::End();
    }
}
