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
        if (ImGui::CollapsingHeader("TAA###TAA"))
        {
            DrawTAASection();
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

        SliderFloatEx(
            "薄明視###MesopicVision", &m_Engine.m_MesopicStrength, 0.0f, 1.0f, Defaults::MesopicStrength, "%.2f", 0,
            "暗所視の再現量。0で無効。実際の輝度が0.01cd/m^2を下回ると桿体だけの視覚になり色が"
            "判別できなくなる(3cd/m^2以上は通常の錐体視のまま。間は対数で補間)。"
            "桿体の分光感度が短波長寄りなので、赤は沈み青は明るく見える(プルキンエ現象)。"
            "露出を下げるだけでは「暗いが色鮮やかな夜」になり肉眼の見え方と合わない");

        CheckboxEx(
            "出力ディザリング###OutputDithering", &m_Engine.m_DitherEnabled, Defaults::DitherEnabled,
            "最終的な8bit量子化の直前に微小なノイズを加え、暗部グラデーションのバンディングを解消する。"
            "暗部のバンディングは中間バッファの精度ではなくこの8bit量子化が主因であることを実測で確認済み");

        EndParamGroup();
    }

    void PostProcessPanel::DrawTAASection()
    {
        BeginParamGroup();

        if (CheckboxEx(
                "TAAを有効にする###EnableTAA", &m_Engine.m_TAAEnabled, Defaults::TAAEnabled,
                "毎フレーム投影行列を1ピクセル未満だけずらし、モーションベクターで前フレームの結果を"
                "再投影して蓄積する。斜めエッジのジャギーと、スクリーンスペース系パスの"
                "サンプリングノイズが時間方向に平均されて消える"))
        {
            // 切り替えた瞬間に履歴を捨てる。無効の間は履歴が更新されないため、
            // 落とさずに再度有効化すると数フレーム前の古い絵が一度だけ混ざる
            m_Engine.m_TAAHistoryValid.store(false, std::memory_order_relaxed);
        }

        if (m_Engine.m_TAAEnabled)
        {
            SliderFloatEx(
                "履歴ブレンド率###TAABlendWeight", &m_Engine.m_TAABlendWeight, 0.02f, 0.5f, Defaults::TAABlendWeight,
                "%.3f", 0,
                "今フレームの色を混ぜる割合。小さいほど多くのフレームが平均されて滑らかになるが、"
                "遮蔽が変わったときの追従が遅くなり残像が出やすくなる。"
                "フレームレートに対して固定の割合なので、fpsが変わると残像の長さも変わる");
            SliderFloatEx(
                "ジッター強度###TAAJitterScale", &m_Engine.m_TAAJitterScale, 0.0f, 1.5f, Defaults::TAAJitterScale,
                "%.2f", 0,
                "サンプル位置を散らす幅の倍率(1.0でピクセル内いっぱい)。0にすると時間方向の"
                "スーパーサンプリング効果だけが消え、再投影と蓄積によるノイズ低減は残る");
            SliderFloatEx(
                "シャープネス###TAASharpness", &m_Engine.m_TAASharpness, 0.0f, 1.0f, Defaults::TAASharpness, "%.2f", 0,
                "蓄積で失われる高域を戻す量。上げすぎると輪郭に白いふちが出る。"
                "トーンマップ後の最終出力にのみ掛かるため、上げてもちらつきは増えない");

            SliderFloatEx(
                "静止時のちらつき抑制###TAAAntiFlicker", &m_Engine.m_TAAAntiFlicker, 0.0f, 1.0f,
                Defaults::TAAAntiFlicker, "%.2f", 0,
                "止まっている画素に限って履歴ブレンド率を下げ、履歴の棄却判定を緩める。"
                "速度0の画素では再投影のずれが原理的に起きないため棄却は害にしかならず、"
                "エッジのちらつきの主な発生源になっている。"
                "動いている画素の扱いは変わらないので、ゴーストの出方は0にしたときと同じ");

            using TAAClipMode = KurenaiEngine3D::TAAClipMode;
            static const char* kClipModeNames[] = { "クリップしない (検証用)", "分散のみ", "分散 + 近傍の最小最大" };
            int clipModeIndex = static_cast<int>(m_Engine.m_TAAClipMode);
            if (ComboEx(
                    "履歴の棄却方法###TAAClipMode", &clipModeIndex, kClipModeNames, IM_ARRAYSIZE(kClipModeNames),
                    static_cast<int>(TAAClipMode::Clamped),
                    "再投影した履歴が「今このあたりにあり得ない色」だったときに捨てる判定の作り方。"
                    "狭いほどゴーストに強いが、判定の箱がジッターで毎フレーム動くぶんちらつきが増える。"
                    "「クリップしない」は原因の切り分け用で、常用するとゴーストが激しく出る"))
            {
                m_Engine.m_TAAClipMode = static_cast<TAAClipMode>(clipModeIndex);
            }

            if (m_Engine.m_TAAClipMode != TAAClipMode::None)
            {
                SliderFloatEx(
                    "履歴の許容幅###TAAClipGamma", &m_Engine.m_TAAClipGamma, 0.5f, 3.0f, Defaults::TAAClipGamma,
                    "%.2f", 0,
                    "近傍の標準偏差の何倍まで履歴を許容するか。下げるとゴーストに強くなる代わりに"
                    "細い構造物(アンテナ・手すり・窓枠)のちらつきが増える");
            }
        }

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
                "測光上限(基準EVから)###AEKeyCeiling", &m_Engine.m_AutoExposureKeyCeilingEV, -4.0f, 16.0f,
                Defaults::AutoExposureKeyCeilingEV, "%.2f EV", 0,
                "測光値が「シーンの基準EV」から何段上まで行くのを許すか。基準EVは太陽・月・空の照度から"
                "求めるため画面の構図に依存しない。小さくするほど、空が画面に占める割合で露出が振れるのを"
                "抑えられる。16まで上げるとヒストグラムだけで決まる挙動に戻る。屋内が暗くならないよう、"
                "止めるのは上側だけ(下限はEV100 下限が効く)");
            SliderFloatEx(
                "夜のロールオフ###AENightRolloff", &m_Engine.m_AutoExposureNightRolloffEV, 0.0f, 8.0f,
                Defaults::AutoExposureNightRolloffEV, "%.2f EV", 0,
                "暗いシーンをわざと暗いまま写す量。自動露出は測ったものを中庸なグレーへ持ち上げるため、"
                "0にすると夜が昼と同じ明るさで出る。測定EV100が下側の折れ点以下で最大、"
                "上側の折れ点以上で0、間は線形。薄明視とセットで意味を持つ");
            SliderFloatEx(
                "ロールオフ 暗側の折れ点###AENightRolloffDark", &m_Engine.m_AutoExposureNightRolloffDarkEV100, -8.0f,
                8.0f, Defaults::AutoExposureNightRolloffDarkEV100, "%.1f", 0,
                "測定EV100がこれ以下なら夜のロールオフが最大量かかる。既定の-2は満月の夜の地表のすぐ上");
            SliderFloatEx(
                "ロールオフ 明側の折れ点###AENightRolloffBright", &m_Engine.m_AutoExposureNightRolloffBrightEV100,
                -8.0f, 20.0f, Defaults::AutoExposureNightRolloffBrightEV100, "%.1f", 0,
                "測定EV100がこれ以上なら夜のロールオフは掛からない。既定の10は曇天の屋外あたり");
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
