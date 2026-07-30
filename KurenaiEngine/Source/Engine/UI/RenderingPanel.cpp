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
        if (ImGui::CollapsingHeader("IBL / 環境光###IBL"))
        {
            DrawIBLSection();
        }
        if (ImGui::CollapsingHeader("スクリーンスペース反射###SSR"))
        {
            DrawSSRSection();
        }

        ImGui::End();
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

        // IBL鏡面・直接光鏡面の両方に効くため、IBLのON/OFFの内側ではなく独立した項目にする
        CheckboxEx(
            "スペキュラのエネルギー補正###SpecularEnergyCompensation", &m_Engine.m_SpecularEnergyCompensationEnabled,
            Defaults::SpecularEnergyCompensationEnabled,
            "粗い金属で単散乱のみのBRDFが失うエネルギーを補う。IBL鏡面と直接光鏡面の両方に効く");

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
