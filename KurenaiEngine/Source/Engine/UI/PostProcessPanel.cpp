#include "UI/PostProcessPanel.h"

#include <imgui.h>

#include "EngineDefaults.h"
#include "KurenaiEngine3D.h"
#include "UI/UIWidgets.h"

namespace Kurenai::UI
{
    void PostProcessPanel::Draw(const PanelDrawContext& context)
    {
        (void)context;

        if (!ImGui::Begin(GetWindowName(), GetVisiblePtr()))
        {
            ImGui::End();
            return;
        }

        DrawUsageHint();

        if (ImGui::CollapsingHeader("トーンマップ###Tonemap", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawTonemapSection();
        }
        if (ImGui::CollapsingHeader("ブルーム###Bloom"))
        {
            DrawBloomSection();
        }
        if (ImGui::CollapsingHeader("自動露出###AutoExposure"))
        {
            DrawAutoExposureSection();
        }

        ImGui::End();
    }

    void PostProcessPanel::DrawTonemapSection()
    {
        using TonemapCurve = KurenaiEngine3D::TonemapCurve;

        BeginParamGroup();

        static const char* kTonemapCurveNames[] = { "Reinhard", "ACES", "AgX" };
        int curveIndex = static_cast<int>(m_Engine.m_TonemapCurve);
        if (ComboEx(
                "カーブ###TonemapCurve", &curveIndex, kTonemapCurveNames, IM_ARRAYSIZE(kTonemapCurveNames),
                static_cast<int>(TonemapCurve::AgX),
                "HDRの輝度を表示可能な範囲へ写す曲線。既定のAgXは、飽和した明るい色でACESに出る"
                "色相シフト(赤→オレンジ)を避けられる。Reinhardは比較用のリファレンス"))
        {
            m_Engine.m_TonemapCurve = static_cast<TonemapCurve>(curveIndex);
        }

        CheckboxEx(
            "出力ディザリング###OutputDithering", &m_Engine.m_DitherEnabled, Defaults::DitherEnabled,
            "最終的な8bit量子化の直前に微小なノイズを加え、暗部グラデーションのバンディングを解消する。"
            "暗部のバンディングは中間バッファの精度ではなくこの8bit量子化が主因であることを実測で確認済み");

        EndParamGroup();
    }

    void PostProcessPanel::DrawBloomSection()
    {
        BeginParamGroup();

        CheckboxEx(
            "ブルームを有効にする###EnableBloom", &m_Engine.m_BloomEnabled, Defaults::BloomEnabled,
            "明るい部分の光がにじむレンズ散乱を再現する");

        if (m_Engine.m_BloomEnabled)
        {
            SliderFloatEx(
                "強さ###BloomStrength", &m_Engine.m_BloomStrength, 0.0f, 0.5f, Defaults::BloomStrength, "%.3f", 0,
                "元の映像へブルームを合成する比率");
            SliderFloatEx(
                "しきい値###BloomThreshold", &m_Engine.m_BloomThreshold, 0.0f, 8.0f, Defaults::BloomThreshold, "%.2f",
                0,
                "この輝度を超えた分だけをブルームの元にする。物理的にはレンズ散乱なので全輝度に"
                "かかるのが正しく、既定は低めにしてある");
            SliderFloatEx(
                "ソフトニー###BloomSoftKnee", &m_Engine.m_BloomSoftKnee, 0.0f, 1.0f, Defaults::BloomSoftKnee, "%.2f",
                0, "しきい値付近の切れ方の滑らかさ。0で硬く切り、1で緩やかに立ち上がる");
        }

        EndParamGroup();
    }

    void PostProcessPanel::DrawAutoExposureSection()
    {
        BeginParamGroup();

        CheckboxEx(
            "自動露出を有効にする###EnableAutoExposure", &m_Engine.m_AutoExposureEnabled, Defaults::AutoExposureEnabled,
            "画面の輝度分布から露出を自動で決める。無効にするとライティングパネルのEV100が"
            "そのまま最終露出になる");

        if (m_Engine.m_AutoExposureEnabled)
        {
            SliderFloatEx(
                "EV100 下限###AEMinEV100", &m_Engine.m_AutoExposureMinEV100, -8.0f, 20.0f,
                Defaults::AutoExposureMinEV100, "%.1f", 0, "自動露出が選べるEV100の下限(暗い側の限界)");
            SliderFloatEx(
                "EV100 上限###AEMaxEV100", &m_Engine.m_AutoExposureMaxEV100, -8.0f, 20.0f,
                Defaults::AutoExposureMaxEV100, "%.1f", 0, "自動露出が選べるEV100の上限(明るい側の限界)");
            SliderFloatEx(
                "露出補正###AECompensation", &m_Engine.m_AutoExposureCompensation, -4.0f, 4.0f,
                Defaults::AutoExposureCompensation, "%.2f EV", 0,
                "自動で決まった露出へ加えるオフセット。写真の露出補正と同じ意味で、"
                "+1で1段(2倍)明るくなる");
            SliderFloatEx(
                "明順応の速さ###AESpeedUp", &m_Engine.m_AutoExposureSpeedUp, 0.1f, 10.0f, Defaults::AutoExposureSpeedUp,
                "%.2f", 0, "暗い場所から明るい場所へ移ったときに露出が追従する速さ");
            SliderFloatEx(
                "暗順応の速さ###AESpeedDown", &m_Engine.m_AutoExposureSpeedDown, 0.1f, 10.0f,
                Defaults::AutoExposureSpeedDown, "%.2f", 0,
                "明るい場所から暗い場所へ移ったときに露出が追従する速さ。実際の目と同様、"
                "明順応より遅くするのが自然");
            SliderFloatEx(
                "測光 下側除外率###AELowPercentile", &m_Engine.m_AutoExposureLowPercentile, 0.0f, 1.0f,
                Defaults::AutoExposureLowPercentile, "%.2f", 0,
                "輝度ヒストグラムの下側何割を露出の平均から除外するか。暗すぎる画素に露出が"
                "引きずられるのを防ぐ");
            SliderFloatEx(
                "測光 上側除外位置###AEHighPercentile", &m_Engine.m_AutoExposureHighPercentile, 0.0f, 1.0f,
                Defaults::AutoExposureHighPercentile, "%.2f", 0,
                "輝度ヒストグラムのどこまでを露出の平均に使うか。これより上のハイライトは除外され、"
                "小さな光源に露出が引きずられるのを防ぐ");
        }

        EndParamGroup();
    }
}
