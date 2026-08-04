#include "UI/DroneShowPanel.h"

#include <imgui.h>

#include "DroneShow.h"
#include "EngineDefaults.h"
#include "KurenaiEngine3D.h"
#include "UI/UIWidgets.h"

namespace Kurenai::UI
{
    void DroneShowPanel::Draw(const PanelDrawContext& context)
    {
        (void)context;

        if (!ImGui::Begin(GetWindowName(), GetVisiblePtr()))
        {
            // 折りたたみ中やドックの非アクティブタブでもEndは対で呼ぶ必要がある
            ImGui::End();
            return;
        }

        DrawUsageHint();

        if (ImGui::CollapsingHeader("ショー###DroneShowPlayback", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawShowSection();
        }
        if (ImGui::CollapsingHeader("編隊###DroneShowFormation", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawFormationSection();
        }
        if (ImGui::CollapsingHeader("星空###Starfield"))
        {
            DrawStarsSection();
        }

        ImGui::End();
    }

    void DroneShowPanel::DrawShowSection()
    {
        ImGui::TextWrapped(
            "夜空を編隊飛行する発光ドローン。1機につきカメラ正対のビルボードを1枚、加算合成で描く。"
            "光芒はこのパスでは作らずHDRのまま出力し、ブルームに任せている。");

        BeginParamGroup();

        CheckboxEx(
            "ドローンショーを有効にする###DroneShowEnabled", &m_Engine.m_DroneShowEnabled,
            Defaults::DroneShowEnabled,
            "無効にすると描画パスごと登録されなくなる。エンジンの既定は無効で、"
            "Scenes/DroneShow.ksceneが[DroneShow]Enabled=trueで有効にしている");

        ImGui::SeparatorText("再生");

        // 【この2つはA/B比較に必須】止められないと、スクリーンショットを撮るたびに
        // 別の編隊・別の変形途中を掴むことになり対照が取れない
        // (水面のm_WaterTimeFrozen・雲のm_CloudTimeFrozenとまったく同じ役割)
        CheckboxEx(
            "時間を止める###DroneShowTimeFrozen", &m_Engine.m_DroneShowTimeFrozen, Defaults::DroneShowTimeFrozen,
            "ショーの進行を止める。A/B比較で同じ編隊・同じ変形位相のスクリーンショットを"
            "撮るために使う(水面・雲の凍結トグルと同じ役割)");

        const float loopDuration = m_Engine.m_DroneShow.LoopDuration();
        if (loopDuration > 0.0f)
        {
            // 直接時刻を指定できるようにする。凍結と組み合わせて「この形のこの瞬間」を再現できる
            SliderFloatEx(
                "ショー時刻###DroneShowTime", &m_Engine.m_DroneShowTime, 0.0f, loopDuration, 0.0f, "%.2f s", 0,
                "ショーの進行時刻[秒]。1巡ぶんの範囲で直接指定できる。"
                "「時間を止める」と併用すると、狙った形・狙った変形途中を毎回同じように再現できる");

            ImGui::Text("現在の形: %s", FormationShapeName(m_Engine.m_DroneShow.CurrentShape(m_Engine.m_DroneShowTime)));
            ImGui::Text("1巡: %.1f 秒", loopDuration);
        }

        SliderFloatEx(
            "再生速度###DroneShowSpeed", &m_Engine.m_DroneShowSpeed, 0.0f, 5.0f, Defaults::DroneShowSpeed, "%.2fx", 0,
            "ショーの進行速度倍率。0で停止(「時間を止める」と同じだが、こちらは値が残る)");

        SliderFloatEx(
            "保持時間###DroneShowHold", &m_Engine.m_DroneShowHoldSeconds, 0.0f, 30.0f, Defaults::DroneShowHoldSeconds,
            "%.1f s", 0, "1つの形を保つ時間");

        SliderFloatEx(
            "変形時間###DroneShowMorph", &m_Engine.m_DroneShowMorphSeconds, 0.1f, 30.0f,
            Defaults::DroneShowMorphSeconds, "%.1f s", 0,
            "次の形へ移るのにかける時間。機体ごとに出発を最大25%ずらしてあるので、"
            "端から順に崩れて組み上がる");

        EndParamGroup();
    }

    void DroneShowPanel::DrawFormationSection()
    {
        BeginParamGroup();

        ImGui::SeparatorText("見た目");

        SliderFloatEx(
            "明るさ###DroneShowBrightness", &m_Engine.m_DroneShowBrightness, 0.0f, 400.0f,
            Defaults::DroneShowBrightness, "%.1f", 0,
            "機体の発光強度。実効プリ露出はこれとは別に描画側で掛かるので、"
            "シーンの露出([Scene]Exposure)を変えても機体と背景の明るさの比は保たれる。"
            "上げすぎると芯がACESで白へ脱色し、機体の色が分からなくなる");

        SliderFloatEx(
            "ビルボード半径###DroneShowRadius", &m_Engine.m_DroneShowRadius, 0.05f, 20.0f, Defaults::DroneShowRadius,
            "%.2f m", 0,
            "1機の光点の半径[m]。遠方でこの半径が1画素を割る場合は、下の最小画面半径まで押し上げられる");

        SliderFloatEx(
            "最小画面半径###DroneShowMinScreenRadius", &m_Engine.m_DroneShowMinScreenRadius, 0.0f, 0.02f,
            Defaults::DroneShowMinScreenRadius, "%.4f", 0,
            "画面上でこれ以下の半径(NDC単位)にはしない。0にすると遠方の機体が1画素を割り、"
            "TAAのサブピクセルジッターでフレームごとに点いたり消えたりする");

        SliderFloatEx(
            "揺れの振幅###DroneShowHover", &m_Engine.m_DroneShowHoverAmplitude, 0.0f, 10.0f,
            Defaults::DroneShowHoverAmplitude, "%.2f m", 0,
            "機体ごとのホバリングの揺れ幅。0にすると数学的に完全な位置で静止し、模型のように見える");

        ImGui::SeparatorText("配置(変えると編隊を作り直す)");

        // ここから下は点群の再生成を伴うパラメータ。KurenaiEngine3D::Render側が
        // 「前回Configureしたときの設定」と突き合わせて、変わったときだけ作り直す
        uint32_t count = m_Engine.m_DroneShowCount;
        if (SliderUIntEx("機体数###DroneShowCount", &count, 1, 4096, Defaults::DroneShowCount,
                         "編隊を構成するドローンの数。上限4096はKurenaiEngine3D.cppのkMaxDrones"
                         "(構造化バッファの固定容量)と一致している"))
        {
            m_Engine.m_DroneShowCount = count;
        }

        SliderFloatEx(
            "編隊の大きさ###DroneShowScale", &m_Engine.m_DroneShowScale, 1.0f, 1000.0f, Defaults::DroneShowScale,
            "%.0f m", 0, "編隊の代表半径。各形状はこの長さを基準に組み立てられる");

        if (ImGui::DragFloat3("編隊の中心###DroneShowCenter", &m_Engine.m_DroneShowCenter.x, 1.0f))
        {
            // DragFloat3は値が変わったフレームだけtrueを返す。Render側の差分判定に任せるので
            // ここでは何もしなくてよいが、意図が読めるよう明示しておく
        }
        ItemHelp(
            "編隊の中心(ワールド座標)。水面より十分高く保つこと——"
            "中心のYから編隊の大きさを引いた値が水面より下になると、平面反射のクリップ平面で"
            "編隊の下半分が反射から欠ける");

        EndParamGroup();
    }

    void DroneShowPanel::DrawStarsSection()
    {
        ImGui::TextWrapped(
            "夜空の星。Sky.hlsliが視線方向のハッシュから解析的に描くのでテクスチャは使わない。"
            "IBLキューブには焼かないため、ここを変えても空の焼き直しは起きない。");

        BeginParamGroup();

        CheckboxEx(
            "星を描く###StarsEnabled", &m_Engine.m_StarsEnabled, Defaults::StarsEnabled,
            "昼は太陽の仰角で完全に0までフェードするので、無効にしても昼のシーンの絵は変わらない");

        SliderFloatEx(
            "密度###StarsDensity", &m_Engine.m_StarsDensity, 1.0f, 256.0f, Defaults::StarsDensity, "%.0f", 0,
            "空を分割するセルの細かさ。1セルにつき星1個なので、大きいほど星が増える");

        SliderFloatEx(
            "明るさ###StarsBrightness", &m_Engine.m_StarsBrightness, 0.0f, 20.0f, Defaults::StarsBrightness, "%.2f", 0,
            "星の明るさ倍率。星は見た目だけの項で、夜空の目標照度(星明かりの照度)には影響しないため、"
            "ここを上げても風景の明るさは変わらない");

        SliderFloatEx(
            "またたき###StarsTwinkle", &m_Engine.m_StarsTwinkle, 0.0f, 1.0f, Defaults::StarsTwinkle, "%.2f", 0,
            "またたきの強さ。既定は0(無効)。上げるとTAAがちらつきとして拾い、"
            "A/B比較のスクリーンショットの再現性も落ちる");

        EndParamGroup();
    }
}
