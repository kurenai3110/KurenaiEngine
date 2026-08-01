#include "UI/RenderingPanel.h"

#include <imgui.h>

#include "EngineDefaults.h"
#include "KurenaiEngine3D.h"
#include "UI/UIWidgets.h"

namespace Kurenai::UI
{
    void RenderingPanel::Draw(const PanelDrawContext& context)
    {
        (void)context;

        if (!ImGui::Begin(GetWindowName(), GetVisiblePtr()))
        {
            // 折りたたみ中やドックの非アクティブタブでもEndは対で呼ぶ必要がある
            ImGui::End();
            return;
        }

        DrawUsageHint();

        if (ImGui::CollapsingHeader("AO / 間接光###AO", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawAOSection();
        }
        if (ImGui::CollapsingHeader("シャドウ###Shadow"))
        {
            DrawShadowSection();
        }
        if (ImGui::CollapsingHeader("スクリーンスペースシャドウ###ScreenSpaceShadow"))
        {
            DrawScreenSpaceShadowSection();
        }
        if (ImGui::CollapsingHeader("IBL / 環境光###IBL"))
        {
            DrawIBLSection();
        }
        if (ImGui::CollapsingHeader("スクリーンスペース反射###SSR"))
        {
            DrawSSRSection();
        }
        if (ImGui::CollapsingHeader("タイルドライトカリング###LightCulling"))
        {
            DrawLightCullingSection();
        }

        ImGui::End();
    }

    void RenderingPanel::DrawScreenSpaceShadowSection()
    {
        ImGui::TextWrapped(
            "シャドウマップを使わずにポイント/スポットライトの影を出す。深度バッファをライトへ向かって"
            "レイマーチするため、画面に写っていない遮蔽物(画面外や手前の面に隠れたもの)は影を落とさない。"
            "得られるのは完全な影ではなく接触影・中距離の遮蔽。どのライトが影を落とすかは"
            "ライティングパネルのライトごとの「影を落とす」で決める");

        BeginParamGroup();

        CheckboxEx(
            "スクリーンスペースシャドウを有効にする###EnableSSS", &m_Engine.m_ScreenSpaceShadowEnabled,
            Defaults::ScreenSpaceShadowEnabled, "無効にするとポイント/スポットライトの影が一切出なくなる");

        ImGui::BeginDisabled(!m_Engine.m_ScreenSpaceShadowEnabled);
        // 上限はScreenSpaceShadow.hlsliのkSSSMaxStepCountと揃える
        SliderIntEx(
            "レイのステップ数###SSSSteps", &m_Engine.m_ScreenSpaceShadowStepCount, 1, 64,
            Defaults::ScreenSpaceShadowStepCount,
            "1本のレイを何回に分けて進めるか。多いほど細い遮蔽物を拾えるが負荷が上がる");
        SliderFloatEx(
            "レイの最大長###SSSMaxRayLength", &m_Engine.m_ScreenSpaceShadowMaxRayLength, 0.05f, 20.0f,
            Defaults::ScreenSpaceShadowMaxRayLength, "%.2f", ImGuiSliderFlags_Logarithmic,
            "レイを飛ばすワールド距離の上限。長くすると遠くの遮蔽も拾えるが、"
            "同じステップ数ではサンプル間隔が粗くなる");
        SliderFloatEx(
            "厚み###SSSThickness", &m_Engine.m_ScreenSpaceShadowThickness, 0.01f, 5.0f,
            Defaults::ScreenSpaceShadowThickness, "%.3f", ImGuiSliderFlags_Logarithmic,
            "深度バッファの面をどれだけの厚みを持つ物体とみなすか。深度しか無いため厚みは推定するしかない");
        SliderFloatEx(
            "法線バイアス###SSSNormalBias", &m_Engine.m_ScreenSpaceShadowNormalBias, 0.0f, 0.02f,
            Defaults::ScreenSpaceShadowNormalBias, "%.4f", 0,
            "レイの始点を法線方向へずらす量。自分自身を遮蔽物と誤検出するアクネを防ぐ");
        SliderFloatEx(
            "画面端のフェード###SSSEdgeFade", &m_Engine.m_ScreenSpaceShadowEdgeFade, 0.01f, 0.5f,
            Defaults::ScreenSpaceShadowEdgeFade, "%.3f", 0,
            "レイが画面外へ出る手前で影を薄くする幅。情報が無くなる境界で影が唐突に切れるのを防ぐ");
        // 0にすると全ライトで影が消える。ライトを増やしたときのコスト上限を決めるつまみ
        SliderIntEx(
            "影を落とすライト数の上限###SSSMaxLights", &m_Engine.m_ScreenSpaceShadowMaxLightsPerPixel, 0, 16,
            Defaults::ScreenSpaceShadowMaxLightsPerPixel,
            "1ピクセルあたり何灯までシャドウレイを飛ばすか。0にすると影が出なくなる。"
            "ライトを増やしたときの負荷の上限を決めるつまみ");
        ImGui::EndDisabled();

        EndParamGroup();
    }

    void RenderingPanel::DrawLightCullingSection()
    {
        ImGui::TextWrapped(
            "画面を16x16ピクセルのタイルに分け、タイルへ届くライトの一覧をあらかじめ作る。"
            "ライティングパスはシーン全体ではなくタイル内のライトだけをループする。"
            "純粋な最適化であり、有効/無効で最終画像が変わってはならない。"
            "グリッドの中身はデバッグ表示の「ライトタイル」で確認できる");

        BeginParamGroup();

        CheckboxEx(
            "タイルドライトカリングを有効にする###EnableLightCulling", &m_Engine.m_LightCullingEnabled,
            Defaults::LightCullingEnabled,
            "無効にすると各ピクセルがシーン中の全ライトをループする。画は変わらず負荷だけが変わる");

        EndParamGroup();

        ImGui::Text(
            "タイル: %u x %u (1タイルあたり最大%uライト)", m_Engine.m_LightTileCountX, m_Engine.m_LightTileCountY,
            KurenaiEngine3D::kLightTileCapacity);
    }

    void RenderingPanel::DrawAOSection()
    {
        using AOTechnique = KurenaiEngine3D::AOTechnique;

        BeginParamGroup();

        CheckboxEx(
            "AO / 間接光を有効にする###EnableAO", &m_Engine.m_AOEnabled, Defaults::AOEnabled,
            "遮蔽(アンビエントオクルージョン)と、手法によっては近傍サーフェスからの間接拡散光を計算する。"
            "無効にすると遮蔽なし・間接光なしのテクスチャがライティングパスへ渡る");

        if (!m_Engine.m_AOEnabled)
        {
            EndParamGroup();
            return;
        }

        static const char* kTechniqueNames[] = { "SSAO", "SSIL (Visibility Bitmask)" };
        int techniqueIndex = static_cast<int>(m_Engine.m_AOTechnique);
        if (ComboEx(
                "手法###Technique", &techniqueIndex, kTechniqueNames, IM_ARRAYSIZE(kTechniqueNames),
                static_cast<int>(AOTechnique::SSAO),
                "SSAOは遮蔽率だけを求める。SSILは遮蔽率に加えて近傍サーフェスからの"
                "間接拡散光(バウンス光)も計算するため重いが、暗部の色づきが自然になる"))
        {
            m_Engine.m_AOTechnique = static_cast<AOTechnique>(techniqueIndex);
        }

        // 半径・厚みはシーン読み込み時に対角長から決まるため固定の既定値を持たない。
        // 右クリックは「既定値に戻す」ではなく「シーンから再計算」にする
        bool recalcRequested = false;

        if (m_Engine.m_AOTechnique == AOTechnique::SSAO)
        {
            SliderFloatSceneDependent(
                "SSAO 半径###SSAORadius", &m_Engine.m_SSAORadius, 0.01f, 5.0f, recalcRequested, "%.3f",
                "遮蔽を探すサンプリング半径(ワールド単位)。シーン読み込み時にシーンの対角長から"
                "自動設定されるため、既定値ではなく「シーンから再計算」で戻す");
            SliderFloatEx(
                "SSAO 強度###SSAOPower", &m_Engine.m_SSAOPower, 0.1f, 4.0f, Defaults::SSAOPower, "%.3f", 0,
                "遮蔽率にかける指数。大きいほど陰影が濃くなる");
        }
        else
        {
            SliderFloatSceneDependent(
                "SSIL 半径###SSILRadius", &m_Engine.m_SSILRadius, 0.01f, 5.0f, recalcRequested, "%.3f",
                "間接光と遮蔽を探すサンプリング半径(ワールド単位)。シーン読み込み時に"
                "シーンの対角長から自動設定される");
            SliderFloatSceneDependent(
                "SSIL 厚み###SSILThickness", &m_Engine.m_SSILThickness, 0.01f, 2.0f, recalcRequested, "%.3f",
                "深度バッファ上の1点が持つと仮定する奥行きの厚み。小さすぎると遮蔽が抜け、"
                "大きすぎると本来遮蔽していない面まで遮蔽扱いになる");
            SliderFloatEx(
                "SSIL 間接光の強さ###SSILIntensity", &m_Engine.m_SSILIntensity, 0.0f, 8.0f, Defaults::SSILIntensity,
                "%.3f", 0, "近傍サーフェスから拾った間接拡散光にかける倍率");
            SliderFloatEx(
                "SSIL 遮蔽の強さ###SSILPower", &m_Engine.m_SSILPower, 0.1f, 4.0f, Defaults::SSILPower, "%.3f", 0,
                "遮蔽率にかける指数。大きいほど陰影が濃くなる");
            SliderUIntEx(
                "SSIL スライス数###SSILSlices", &m_Engine.m_SSILSliceCount, 1, 8, Defaults::SSILSliceCount,
                "半球を何枚の方位スライスに分けてサンプリングするか。多いほど品質が上がり負荷も上がる");
            SliderUIntEx(
                "SSIL ステップ数###SSILSteps", &m_Engine.m_SSILStepCount, 1, 16, Defaults::SSILStepCount,
                "1スライスあたり半径方向に何点サンプリングするか。多いほど品質が上がり負荷も上がる");
        }

        if (recalcRequested)
        {
            m_Engine.ResetSceneDependentParams();
        }

        EndParamGroup();
    }

    void RenderingPanel::DrawShadowSection()
    {
        BeginParamGroup();

        CheckboxEx(
            "シャドウを有効にする###EnableShadow", &m_Engine.m_ShadowEnabled, Defaults::ShadowEnabled,
            "平行光(太陽)のカスケードシャドウマップを描画する");

        if (m_Engine.m_ShadowEnabled)
        {
            SliderFloatEx(
                "PCSS ライトサイズ###ShadowLightSize", &m_Engine.m_ShadowLightSize, 0.001f, 0.05f,
                Defaults::ShadowLightSize, "%.4f", 0,
                "シャドウマップUV空間でのブロッカーサーチ半径。大きいほど半影が広く柔らかくなる");
        }

        EndParamGroup();
    }

    void RenderingPanel::DrawIBLSection()
    {
        BeginParamGroup();

        CheckboxEx(
            "IBLを有効にする###EnableIBL", &m_Engine.m_IBLEnabled, Defaults::IBLEnabled,
            "空(スカイボックス)を環境光源として使う。無効にすると代わりに一様な環境光を使う");

        if (m_Engine.m_IBLEnabled)
        {
            SliderFloatEx(
                "IBL 強度###IBLIntensity", &m_Engine.m_IBLIntensity, 0.0f, 2.0f, Defaults::IBLIntensity, "%.3f", 0,
                "環境光として加える量の倍率");
            CheckboxEx(
                "専用イラディアンスマップを使う###UseDedicatedIrradiance", &m_Engine.m_IBLUseDedicatedIrradiance,
                Defaults::IBLUseDedicatedIrradiance,
                "既定では拡散イラディアンスをプリフィルタ済み鏡面の最終ミップ(粗さ1)から得る。"
                "これを有効にすると従来の専用イラディアンスマップをその場で焼いて切り替える(検証用)");
        }
        else
        {
            SliderFloatEx(
                "環境光の強さ###AmbientScale", &m_Engine.m_AmbientScale, 0.0f, 3.0f, Defaults::AmbientScale, "%.3f", 0,
                "IBLを使わないときの、方向を持たない一様な環境光の強さ");
        }

        // IBLの有効/無効どちらでも効くため、上の分岐の外に置く。
        // 「IBL 強度」が拡散と鏡面へ一様に掛かるのに対し、この2つは両者の比率を崩すためのもの
        SliderFloatEx(
            "環境光の拡散倍率###AmbientDiffuseScale", &m_Engine.m_AmbientDiffuseScale, 0.0f, 2.0f,
            Defaults::AmbientDiffuseScale, "%.3f", 0,
            "環境光(間接光)の拡散成分だけに掛かる倍率。0にすると環境からの照り返しが消え、"
            "映り込みだけが残る。直接光・自発光・SSILの間接光には掛からない");
        SliderFloatEx(
            "環境光の鏡面倍率###AmbientSpecularScale", &m_Engine.m_AmbientSpecularScale, 0.0f, 2.0f,
            Defaults::AmbientSpecularScale, "%.3f", 0,
            "環境光(間接光)の鏡面成分だけに掛かる倍率。金属やガラスの映り込みの強さを、"
            "環境からの照り返しを保ったまま増減できる。SSRと反射プローブにも同じ倍率が効く");

        // IBL鏡面・直接光鏡面の両方に効くため、IBLのON/OFFの内側ではなく独立した項目にする。
        // 並びはKurenaiEngine3D::SpecularCompensationModeの値と一致させること
        {
            static const char* const kCompensationModes[] = {
                "補正なし",
                "Linear  1+F0(1/Ess-1)",
                "Series  1/(1-F0(1-Ess))",
                "Kulla-Conty(加算ローブ)",
            };
            int mode = static_cast<int>(m_Engine.m_SpecularCompensationMode);
            if (ComboEx(
                    "スペキュラのエネルギー補正###SpecularEnergyCompensation", &mode, kCompensationModes,
                    IM_ARRAYSIZE(kCompensationModes), Defaults::SpecularCompensationMode,
                    "粗い金属で単散乱のみのBRDFが失うエネルギーを補う。IBL鏡面と直接光鏡面の両方に効く。\n"
                    "LinearとSeriesは失われた分を「1回だけ」跳ね返すか「無限回」跳ね返すかの違いで、"
                    "F0=1では数学的に一致する。Kulla-Contyは乗算ではなく広い加算ローブを足す本来の形で、"
                    "相反性を満たす代わりに直接光でライト1灯あたりLUTフェッチが1回増える。\n"
                    "既定のLinearは実使用域で最も真値に近い(14.9.8節)"))
            {
                m_Engine.m_SpecularCompensationMode =
                    static_cast<KurenaiEngine3D::SpecularCompensationMode>(mode);
            }
        }

        EndParamGroup();
    }

    void RenderingPanel::DrawSSRSection()
    {
        BeginParamGroup();

        CheckboxEx(
            "SSRを有効にする###EnableSSR", &m_Engine.m_SSREnabled, Defaults::SSREnabled,
            "スクリーンスペースで反射をレイマーチする。画面外のものは反射に映らない");

        if (m_Engine.m_SSREnabled)
        {
            bool recalcRequested = false;
            SliderFloatSceneDependent(
                "SSR 最大距離###SSRMaxDistance", &m_Engine.m_SSRMaxDistance, 0.1f, 100.0f, recalcRequested, "%.3f",
                "反射レイを追跡する最大距離(ワールド単位)。シーン読み込み時に対角長から自動設定される");
            SliderFloatSceneDependent(
                "SSR 厚み###SSRThickness", &m_Engine.m_SSRThickness, 0.01f, 2.0f, recalcRequested, "%.3f",
                "深度バッファ上の1点が持つと仮定する奥行きの厚み。ヒット判定の許容量になる");
            SliderFloatEx(
                "SSR 粗さのしきい値###SSRRoughnessCutoff", &m_Engine.m_SSRRoughnessCutoff, 0.05f, 1.0f,
                Defaults::SSRRoughnessCutoff, "%.3f", 0,
                "この粗さを超えるマテリアルではSSRを行わない。粗い面ではノイズが目立ち負荷に見合わないため");

            if (recalcRequested)
            {
                m_Engine.ResetSceneDependentParams();
            }
        }

        EndParamGroup();
    }
}
