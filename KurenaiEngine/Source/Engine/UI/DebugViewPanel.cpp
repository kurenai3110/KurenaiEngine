#include "UI/DebugViewPanel.h"

#include <imgui.h>

#include "EngineDefaults.h"
#include "KurenaiEngine3D.h"
#include "UI/UIWidgets.h"

namespace Kurenai::UI
{
    void DebugViewPanel::Draw(const PanelDrawContext& context)
    {
        (void)context;

        if (!ImGui::Begin(GetWindowName(), GetVisiblePtr()))
        {
            ImGui::End();
            return;
        }

        using DebugView = KurenaiEngine3D::DebugView;

        // DebugView::AtmosphereLUTで表示するLUTの選択肢。
        // 並びはKurenaiEngine3D::m_AtmosphereLUTDebugIndexの意味と一致させること
        static const char* kAtmosphereLUTNames[] =
        {
            "Transmittance (256x64)",
            "MultiScattering (32x32)",
            "SkyView (192x108)",
        };

        // 並びはDebugView enumと一致していなければならない(下のstatic_assert参照)
        static const char* kDebugViewNames[] =
        {
            "最終結果 (Final)",
            "アルベド",
            "法線",
            "マテリアル (R=金属度, G=粗さ, B=遮蔽マップ)",
            "自発光",
            "深度",
            "深度 (生値)",
            "直接光",
            "AO/GI - 間接光 (RGB)",
            "AO/GI - 間接光 (RGB, ブラー前)",
            "AO/GI - 遮蔽率 (アルファ)",
            "AO/GI - 遮蔽率 (アルファ, ブラー前)",
            "シャドウマップ",
            "RTシャドウ (太陽の可視率)",
            "SSR (最終結果 + 反射)",
            "Hi-Z (深度ミップチェーン)",
            "IBL - イラディアンス (キューブマップ)",
            "IBL - プリフィルタ済み鏡面 (キューブマップ・ミップチェーン)",
            "IBL - BRDF LUT (X=NdotV, Y=粗さ, RGB=A/B/Eavg)",
            "ブルーム (ピラミッド最上段・半解像度)",
            "ライトタイル (タイルあたりのライト数ヒートマップ)",
            // 反射プローブは鏡面専任なので「プローブ - イラディアンス」は無い
            // (拡散はDDGIへ一本化。「DDGI - イラディアンス」で見る)
            "プローブ - プリフィルタ済み鏡面 (ミップ0=キャプチャ結果)",
            "プローブ - 影響範囲 (プローブごとの色分け)",
            "プローブ - 距離 (キューブマップ配列)",
            "モーションベクター (速度バッファ)",
            "シーンカラー (生HDR・トーンマップなし)",
            "DDGI - イラディアンス (オクタヘドラルアトラス)",
            "DDGI - 距離モーメント (R=平均距離)",
            "bent normal (軸=色 / Gain>1.5で長さ=グレー)",
            "水面マスク (A=水面フラグ)",
            "平面反射 (水面に映る鏡像)",
            "雲の3Dノイズ (スライス表示・2x2タイル)",
            "大気散乱LUT (Transmittance / MultiScattering / SkyView)",
            "DDGI - プローブ裏面率 (壁の内部に埋まったプローブ)",
            "SWラスタ (自前ラスタライザのフラット陰影)",
            "SWラスタ - 深度 (生値)",
            "SWラスタ - 法線",
            "MegaLights (ポイント/スポットの直接光)",
            "MegaLights - 候補プール (タイルへ届いたライト数)",
            "MegaLights - 蓄積平均 (計測用。線形空間でNフレーム平均)",
        };
        static_assert(
            // 末尾のMegaLightsAverageは39
            static_cast<int>(DebugView::MegaLightsAverage) == 39,
            "kDebugViewNamesの並びをDebugView enumと一致させること(末尾はMegaLightsAverage)");
        static_assert(
            IM_ARRAYSIZE(kDebugViewNames) == KurenaiEngine3D::kDebugViewCount,
            "kDebugViewNamesの要素数をDebugViewの総数と一致させること");

        DrawUsageHint();
        BeginParamGroup();

        int currentIndex = static_cast<int>(m_Engine.m_DebugView);
        if (ComboEx(
                "表示するバッファ###View", &currentIndex, kDebugViewNames, IM_ARRAYSIZE(kDebugViewNames),
                static_cast<int>(DebugView::Final),
                "Presentパスで画面に出す内容。最終結果以外を選ぶと各パスの中間結果をそのまま表示する"))
        {
            m_Engine.m_DebugView = static_cast<DebugView>(currentIndex);
        }

        if (m_Engine.m_DebugView == DebugView::HiZ)
        {
            SliderIntEx(
                "Hi-Z ミップレベル###HiZMip", &m_Engine.m_HiZDebugMipLevel, 0,
                static_cast<int>(m_Engine.m_HiZMipLevels) - 1, 0, "表示する深度ミップチェーンの段");
        }

        if (m_Engine.m_DebugView == DebugView::ShadowMap)
        {
            SliderIntEx(
                "シャドウカスケード###ShadowCascade", &m_Engine.m_ShadowDebugCascade, 0,
                static_cast<int>(KurenaiEngine3D::kCascadeCount) - 1, 0,
                "表示するカスケードの番号。0がカメラに最も近い範囲");
        }

        if (m_Engine.m_DebugView == DebugView::IBLPrefilter)
        {
            SliderIntEx(
                "プリフィルタ ミップレベル###PrefilterMip", &m_Engine.m_IBLPrefilterDebugMipLevel, 0,
                static_cast<int>(KurenaiEngine3D::kIBLPrefilterMipLevels) - 1, 0,
                "表示するミップの段。段が進むほど粗い面向けにぼかされている");
        }

        if (m_Engine.m_DebugView == DebugView::ProbePrefilter || m_Engine.m_DebugView == DebugView::ProbeDistance)
        {
            // プローブが1つも無いシーンでもスライダーの範囲が壊れないよう下限を0に保つ
            const int maxProbeIndex =
                m_Engine.m_ReflectionProbes.empty() ? 0 : static_cast<int>(m_Engine.m_ReflectionProbes.size()) - 1;
            SliderIntEx(
                "プローブ番号###ProbeIndex", &m_Engine.m_ProbeDebugIndex, 0, maxProbeIndex, 0,
                "表示する反射プローブの番号。反射プローブパネルの一覧と同じ並び");

            if (m_Engine.m_DebugView == DebugView::ProbePrefilter)
            {
                SliderIntEx(
                    "プローブ プリフィルタ ミップ###ProbePrefilterMip", &m_Engine.m_ProbePrefilterDebugMipLevel, 0,
                    static_cast<int>(KurenaiEngine3D::kIBLPrefilterMipLevels) - 1, 0,
                    "表示するミップの段。ミップ0はぼかす前のキャプチャ結果そのもの");
            }

            if (m_Engine.m_DebugView == DebugView::ProbeDistance)
            {
                // 距離キューブに入っているのは色ではなくワールド距離なので、表示輝度の倍率(1倍以上)
                // ではなく「白になる距離」で正規化する(Render()側でこの逆数をGainとして渡す)
                SliderFloatEx(
                    "白になる距離###ProbeDistanceRange", &m_Engine.m_ProbeDistanceDebugRange, 1.0f, 200.0f,
                    Defaults::ProbeDistanceDebugRange, "%.1f", ImGuiSliderFlags_Logarithmic,
                    "この距離で白飽和するようグレースケール化する。部屋の大きさに合わせると形が読める");
            }
        }

        if (m_Engine.m_DebugView == DebugView::CloudNoiseSlice)
        {
            ImGui::TextWrapped(
                "画面には2x2タイルぶんを表示している。タイル境界に継ぎ目があれば画面中央の十字線として現れる");
            CheckboxEx(
                "ディテール(32^3)を見る###CloudNoiseDetail", &m_Engine.m_CloudNoiseDebugShowDetail, false,
                "オフで形状ノイズ(128^3、RGB=Perlin-Worley/Worley/Worley)、オンで縁を削るディテールノイズ");
            SliderFloatEx(
                "スライス位置###CloudNoiseSlice", &m_Engine.m_CloudNoiseDebugSlice, 0.0f, 1.0f, 0.0f, "%.3f", 0,
                "3Dテクスチャのどの断面を見るか(W座標)。動かして中身が変わらなければ焼けていない");
        }

        if (m_Engine.m_DebugView == DebugView::AtmosphereLUT)
        {
            ImGui::TextWrapped(
                "Transmittance: 横=視線天頂角、縦=高度。地表(下端)から天頂(右端)を見た値が "
                "(0.940, 0.868, 0.762) になるのが解析解との一致条件。"
                "SkyView: 横=太陽の子午線からの方位(左端が太陽側)、"
                "縦=天頂角(上端が天頂、中央が地平線)");
            ComboEx(
                "表示するLUT###AtmosphereLUTIndex", &m_Engine.m_AtmosphereLUTDebugIndex,
                kAtmosphereLUTNames, IM_ARRAYSIZE(kAtmosphereLUTNames), 0,
                "MultiScatteringは値が小さいので表示輝度の倍率を上げて見る");
        }

        // ライトタイルとMegaLightsの候補プールは**同じヒートマップの上限**を共有する。
        // 両者は同じ到達判定を使うので、同じ上限で撮った2枚は画素単位で一致するはずであり、
        // つまみが別々だと比べられなくなる
        if (m_Engine.m_DebugView == DebugView::LightTiles ||
            m_Engine.m_DebugView == DebugView::MegaLightsTilePool)
        {
            if (m_Engine.m_DebugView == DebugView::LightTiles)
            {
                // ヒートマップの色: 黒=0灯、青=少ない、緑、赤=上限以上、マゼンタ=タイル容量超過
                ImGui::TextWrapped(
                    "黒=0灯 / 青→緑→赤=ライトが多い / マゼンタ=タイル容量(%uライト)を超過",
                    KurenaiEngine3D::kLightTileCapacity);
            }
            else
            {
                // 候補プールは容量で打ち切らないのでマゼンタは出ない
                ImGui::TextWrapped(
                    "黒=0灯 / 青→緑→赤=ライトが多い。"
                    "「ライトタイル」と同じ判定・同じ色付けなので、2枚は一致するはず。"
                    "ただし一致するのは到達灯数がタイル容量(%uライト)以下のタイルだけで、"
                    "「ライトタイル」側にマゼンタが出ていたらそこは比べられない。"
                    "また下の上限を上げないと、ライトの多いシーンでは一面が赤に飽和して"
                    "違いが色に出ない(飽和した状態での一致は検出力がほとんど無い)",
                    KurenaiEngine3D::kLightTileCapacity);
            }

            SliderIntEx(
                "ヒートマップの上限###HeatmapMax", &m_Engine.m_LightTileHeatmapMax, 1,
                static_cast<int>(KurenaiEngine3D::kLightTileCapacity), Defaults::LightTileHeatmapMax,
                "この灯数で赤になるようヒートマップを正規化する。ライトが少ないシーンでは下げると差が見える");

            if (m_Engine.m_DebugView == DebugView::LightTiles && !m_Engine.m_LightCullingEnabled)
            {
                ImGui::TextWrapped("タイルドライトカリングが無効のため、ライトグリッドは更新されていません");
            }
            if (m_Engine.m_DebugView == DebugView::MegaLightsTilePool && !m_Engine.ShouldRunMegaLights())
            {
                ImGui::TextWrapped("MegaLightsが無効のため、候補プールは更新されていません");
            }
        }

        if (m_Engine.m_DebugView == DebugView::MotionVector)
        {
            ImGui::TextWrapped(
                "灰色=動いていない / 赤が濃い=画面内容が右へ / 薄い=左へ / 緑が濃い=下へ / 薄い=上へ。"
                "静止していれば全面が均一な灰色になり、色が付いていたら速度バッファが壊れている。"
                "下の表示輝度の倍率で感度を変えられる(既定は約20画素/フレームで飽和)");
            if (!m_Engine.m_TAAEnabled)
            {
                ImGui::TextWrapped("TAAが無効でも速度バッファは常に更新されるため、この表示はそのまま確認できます");
            }
        }

        // AO/GIバッファの間接拡散光のように値が小さいバッファ(暗い室内では0.02〜0.1程度)は
        // 等倍表示だとほぼ真っ黒で階調の粗さが判別できない。持ち上げて表示することで、
        // 8bit格納時のポスタリゼーションが何段あるかを目視で比較できる。
        // 最終結果は見た目そのものを確認する表示なので倍率を適用しない(Render()側で1.0固定)
        if (m_Engine.m_DebugView != DebugView::Final)
        {
            SliderFloatEx(
                "表示輝度の倍率###DebugViewGain", &m_Engine.m_DebugViewGain, 1.0f, 64.0f, Defaults::DebugViewGain,
                "%.1fx", ImGuiSliderFlags_Logarithmic,
                "暗いバッファを持ち上げて表示する倍率。バッファ精度の比較と併用して"
                "ポスタリゼーションの段数を目視で確認する。最終結果には適用されない");
        }

        EndParamGroup();

        if (ImGui::CollapsingHeader("バッファ精度 (A/B比較)###BufferPrecision"))
        {
            DrawBufferPrecisionSection();
        }

        ImGui::End();
    }

    void DebugViewPanel::DrawBufferPrecisionSection()
    {
        using BufferPrecision = KurenaiEngine3D::BufferPrecision;

        // 切り替えるとレンダーターゲットとPSOを作り直す必要があるが、ここで直接作り直すと
        // GPUがまだ読んでいるテクスチャを壊すため、フラグだけ立ててRender()側で
        // (WaitForGPUIdleを挟んで)処理する
        int precisionIndex = static_cast<int>(m_Engine.m_BufferPrecision);
        bool precisionChanged = ImGui::RadioButton("HDR", &precisionIndex, static_cast<int>(BufferPrecision::HDR));
        ItemHelp("自発光=R11G11B10F、AO/GI=RGBA16F。本来採用したい構成");
        ImGui::SameLine();
        precisionChanged |=
            ImGui::RadioButton("Legacy 8bit", &precisionIndex, static_cast<int>(BufferPrecision::Legacy8bit));
        ItemHelp("中間バッファをすべてRGBA8_UNormにする。暗部の階調が粗くなるのを比較するための経路");

        if (precisionChanged && precisionIndex != static_cast<int>(m_Engine.m_BufferPrecision))
        {
            m_Engine.m_BufferPrecision = static_cast<BufferPrecision>(precisionIndex);
            m_Engine.m_BufferPrecisionDirty = true;
        }

        ImGui::TextWrapped(
            "アルベドはどちらの構成でもリニアの8bit。sRGB格納は最終画像への寄与が測定限界以下のため不採用");
    }
}
