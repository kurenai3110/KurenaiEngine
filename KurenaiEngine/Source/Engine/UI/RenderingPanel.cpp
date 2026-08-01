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
        if (ImGui::CollapsingHeader("DDGI (拡散グローバルイルミネーション)###DDGI"))
        {
            DrawDDGISection();
        }
        // ###以降のIDはimgui.iniのキーになるため、表示名だけ変えてIDはSSRのまま据え置く
        if (ImGui::CollapsingHeader("反射###SSR"))
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

        // 遮蔽マップは上のAO/間接光とは別系統(アセットに焼き込まれた遮蔽)なので、
        // AOを切っていても操作できるよう早期returnより前に置く
        CheckboxEx(
            "マテリアルの遮蔽マップを使う###UseOcclusionMap", &m_Engine.m_OcclusionMapEnabled,
            Defaults::OcclusionMapEnabled,
            "アセットに焼き込まれた遮蔽(glTFのocclusionTexture)を間接光へ掛けるか。"
            "上のAO / 間接光とは独立した別系統で、そちらを無効にしても遮蔽マップは効き続ける。"
            "内容はデバッグ表示の「マテリアル」のBチャンネルで確認できる。"
            "反射プローブへ反映するにはプローブの焼き直しが必要(焼いた時点の値が入っているため)");

        if (!m_Engine.m_AOEnabled)
        {
            EndParamGroup();
            return;
        }

        // レイトレーシングは非対応の環境(DX11、あるいはDXR Tier 1.1に達していないDX12)では
        // 選択肢そのものを出さない(影・反射の手法選択と同じ方針)
        static const char* kTechniqueNamesWithRT[] =
        {
            "SSAO", "SSIL (Visibility Bitmask)", "レイトレーシング (RTAO/RTGI)"
        };
        static const char* kTechniqueNamesWithoutRT[] = { "SSAO", "SSIL (Visibility Bitmask)" };

        const bool rtAvailable = m_Engine.m_RaytracingAvailable;
        const char* const* techniqueNames = rtAvailable ? kTechniqueNamesWithRT : kTechniqueNamesWithoutRT;
        const int techniqueCount =
            rtAvailable ? IM_ARRAYSIZE(kTechniqueNamesWithRT) : IM_ARRAYSIZE(kTechniqueNamesWithoutRT);

        int techniqueIndex = static_cast<int>(m_Engine.m_AOTechnique);
        if (ComboEx(
                "手法###Technique", &techniqueIndex, techniqueNames, techniqueCount,
                static_cast<int>(AOTechnique::SSAO),
                "SSAOは遮蔽率だけを求める。SSILは遮蔽率に加えて近傍サーフェスからの"
                "間接拡散光(バウンス光)も計算するため重いが、暗部の色づきが自然になる。"
                "レイトレーシングは同じものを深度バッファではなくシーン全体への交差判定で求めるため、"
                "画面に映っていない遮蔽物・反射面も効く"))
        {
            m_Engine.m_AOTechnique = static_cast<AOTechnique>(techniqueIndex);
        }

        if (!rtAvailable)
        {
            ImGui::TextDisabled("レイトレーシングは利用できません(DX12かつDXR Tier 1.1が必要)");
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
        else if (m_Engine.m_AOTechnique == AOTechnique::Raytraced)
        {
            SliderFloatSceneDependent(
                "RT 最大距離###RTAOMaxDistance", &m_Engine.m_RTAOMaxDistance, 0.05f, 10.0f, recalcRequested, "%.3f",
                "遮蔽とバウンス光を探すレイの最大距離(ワールド単位)。シーン読み込み時に"
                "シーンの対角長から自動設定される。これより遠くにある面は遮蔽物にならず、"
                "間接光の光源にもならない");
            SliderIntEx(
                "RT サンプル数###RTAOSampleCount", &m_Engine.m_RTAOSampleCount, 1, 32, Defaults::RTAOSampleCount,
                "1ピクセルあたりに半球へ撃つレイの本数。デノイザを持たずブラーだけで均すため、"
                "少なすぎるとブラー後もノイズが残る");
            SliderFloatEx(
                "RT 間接光の強さ###RTAOIntensity", &m_Engine.m_RTAOIntensity, 0.0f, 8.0f, Defaults::RTAOIntensity,
                "%.3f", 0,
                "バウンス面から拾った間接拡散光にかける倍率。1.0が物理的に正しい値");
            SliderFloatEx(
                "RT 遮蔽の強さ###RTAOPower", &m_Engine.m_RTAOPower, 0.1f, 4.0f, Defaults::RTAOPower, "%.3f", 0,
                "遮蔽率にかける指数。大きいほど陰影が濃くなる");
            CheckboxEx(
                "バウンス面に影を落とす###RTAOBounceShadowRay", &m_Engine.m_RTAOBounceShadowRayEnabled,
                Defaults::RTAOBounceShadowRayEnabled,
                "バウンス面から太陽へ影レイを撃つ。切ると日陰の面まで間接光を放つようになるが、その分速い");
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
        using ShadowMode = KurenaiEngine3D::ShadowMode;

        BeginParamGroup();

        // 手法の選択。反射(DrawSSRSection)とまったく同じ方針で、レイトレーシング非対応の環境では
        // 選択肢そのものを出さない
        static const char* kModeNamesWithRT[] =
        {
            "なし", "カスケードシャドウマップ (CSM)", "レイトレーシング (RT)"
        };
        static const char* kModeNamesWithoutRT[] = { "なし", "カスケードシャドウマップ (CSM)" };

        const bool rtAvailable = m_Engine.m_RaytracingAvailable;
        const char* const* modeNames = rtAvailable ? kModeNamesWithRT : kModeNamesWithoutRT;
        const int modeCount = rtAvailable ? IM_ARRAYSIZE(kModeNamesWithRT) : IM_ARRAYSIZE(kModeNamesWithoutRT);

        int modeIndex = static_cast<int>(m_Engine.m_ShadowMode);
        if (ComboEx(
                "影の手法###ShadowMode", &modeIndex, modeNames, modeCount,
                static_cast<int>(KurenaiEngine3D::DefaultShadowMode(rtAvailable)),
                "平行光(太陽)の影の求め方。CSMはライト視点の深度バッファを4枚描いて深度比較する。"
                "レイトレーシングはピクセルごとに太陽へ影レイを撃つため、カスケードの境界も"
                "ピーターパン(接地部の浮き)もアクネも出ない"))
        {
            m_Engine.m_ShadowMode = static_cast<ShadowMode>(modeIndex);
        }

        if (!rtAvailable)
        {
            ImGui::TextDisabled("レイトレーシングは利用できません(DX12かつDXR Tier 1.1が必要)");
        }

        if (m_Engine.m_ShadowMode == ShadowMode::CascadedShadowMap)
        {
            SliderFloatEx(
                "PCSS ライトサイズ###ShadowLightSize", &m_Engine.m_ShadowLightSize, 0.001f, 0.05f,
                Defaults::ShadowLightSize, "%.4f", 0,
                "シャドウマップUV空間でのブロッカーサーチ半径。大きいほど半影が広く柔らかくなる");
        }
        else if (m_Engine.m_ShadowMode == ShadowMode::Raytraced)
        {
            SliderIntEx(
                "RT サンプル数###RTShadowSampleCount", &m_Engine.m_RTShadowSampleCount, 1, 16,
                Defaults::RTShadowSampleCount,
                "1ピクセルあたりに撃つ影レイの本数。デノイザを持たないため、太陽を大きくするほど"
                "ここを増やさないと半影にノイズが出る");
            SliderFloatEx(
                "RT 太陽の角半径###RTShadowSunAngularRadius", &m_Engine.m_RTShadowSunAngularRadiusDegrees,
                0.0f, 5.0f, Defaults::RTShadowSunAngularRadiusDegrees, "%.3f度", 0,
                "太陽の見かけの半径。実際の太陽は視直径約0.53度なので既定値はその半分。"
                "大きくすると半影が広く柔らかくなる");
            ImGui::TextDisabled("半透明と反射プローブの影は常にCSMを使います");
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

            if (m_Engine.m_IBLUseDedicatedIrradiance)
            {
                // M11 Stage 4a: 専用イラディアンスマップを焼く2つの経路(旧: 総当たり積分/
                // 新: 球面調和関数L2)をA/B比較できるようにする。既定は旧経路(false)。
                //
                // 【値が変わったら焼き直しを要求すること】イラディアンスマップは空が焼き直された
                // ときにしか作り直されない(KurenaiEngine3D::Render の m_IBLIrradianceBaked 参照)。
                // ここでフラグを倒さないと、トグルを切り替えても画面はまったく変わらず、
                // 別の理由で空が焼き直されるまで切り替え前の経路の結果が出続ける
                // ——つまりA/B比較のために付けたつまみが機能しない(実機で確認した)
                if (CheckboxEx(
                        "球面調和関数(SH)で焼く###UseSHIrradiance", &m_Engine.m_IBLUseSHIrradiance,
                        Defaults::IBLUseSHIrradiance,
                        "拡散イラディアンスを球面調和関数L2(9項)で焼く高速な経路。理論上どんな照明でも"
                        "数%以内の誤差に収まるが、エミッシブ帯のような小さく明るい光源では暗部が"
                        "わずかに負へオーバーシュートする(リンギング)ことがある。"
                        "無効なら従来の総当たり積分(1テクセルあたり15,876サンプル)を使う"))
                {
                    m_Engine.m_IBLIrradianceBaked = false;
                }
                if (m_Engine.m_IBLUseSHIrradiance)
                {
                    if (SliderFloatEx(
                            "SHウィンドウ強度###SHWindowLambda", &m_Engine.m_SHWindowLambda, 0.0f, 0.1f,
                            Defaults::SHWindowLambda, "%.4f", 0,
                            "リンギング対策。大きくするほど高次バンドを減衰させ、ボケと引き換えに"
                            "暗部の負のオーバーシュートを抑える。0=無効"))
                    {
                        m_Engine.m_IBLIrradianceBaked = false;
                    }
                }
            }
        }
        else
        {
            SliderFloatEx(
                "環境光の強さ###AmbientScale", &m_Engine.m_AmbientScale, 0.0f, 3.0f, Defaults::AmbientScale, "%.3f", 0,
                "IBLを使わないときの、方向を持たない一様な環境光の強さ");
        }

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

    void RenderingPanel::DrawDDGISection()
    {
        ImGui::TextWrapped(
            "プローブを格子状に敷き詰めて、位置ごとに違う拡散の間接光を与える(22章)。"
            "反射プローブが鏡面を担うのに対し、こちらは「壁の色が床へ回り込む」ような"
            "間接拡散光を担当する。有効にすると拡散の環境光がグローバルIBL/反射プローブから"
            "この格子由来のものへ差し替わる(加算ではない)");

        if (!m_Engine.m_HasGIVolume)
        {
            ImGui::TextWrapped("このシーンには[GIVolume]が無いため、DDGIは動作しない");
            return;
        }

        BeginParamGroup();

        CheckboxEx(
            "DDGIを有効にする###EnableDDGI", &m_Engine.m_DDGIEnabled, Defaults::DDGIEnabled,
            "無効にすると拡散の環境光が従来どおりグローバルIBL/反射プローブのイラディアンスに戻る");

        ImGui::BeginDisabled(!m_Engine.m_DDGIEnabled);
        SliderFloatEx(
            "DDGI 強度###DDGIIntensity", &m_Engine.m_DDGIIntensity, 0.0f, 2.0f, Defaults::DDGIIntensity, "%.3f", 0,
            "拡散間接光の倍率。SSILと寄与が重なるぶんを実測で調整するためのつまみ");
        SliderIntEx(
            "1フレームの更新プローブ数###DDGIProbesPerFrame", &m_Engine.m_DDGIProbesPerFrame, 1, 64,
            Defaults::DDGIProbesPerFrame,
            "多いほど光の変化への追従が速くなるが、1プローブにつきシーンを6回描くため負荷も比例して上がる");
        ImGui::EndDisabled();

        ImGui::Text(
            "プローブ数: %u (%u x %u x %u)", m_Engine.m_DDGIProbeCount,
            m_Engine.m_GIVolume.ProbeCounts[0], m_Engine.m_GIVolume.ProbeCounts[1], m_Engine.m_GIVolume.ProbeCounts[2]);
        ImGui::Text(m_Engine.m_DDGIWarmingUp ? "初回の一巡を実行中" : "初回の一巡は完了");

        EndParamGroup();
    }

    void RenderingPanel::DrawSSRSection()
    {
        using ReflectionMode = KurenaiEngine3D::ReflectionMode;

        BeginParamGroup();

        // 手法の選択。Raytracedはレイトレーシング非対応の環境(DX11、あるいはDXR Tier 1.1に
        // 達していないDX12)では選べないため、選択肢そのものを出さない。
        // 「出ているのに選ぶと何も起きない」より「出ていない」ほうが誤解が少ない
        static const char* kModeNamesWithRT[] = { "なし", "スクリーンスペース (SSR)", "レイトレーシング (RT)" };
        static const char* kModeNamesWithoutRT[] = { "なし", "スクリーンスペース (SSR)" };

        const bool rtAvailable = m_Engine.m_RaytracingAvailable;
        const char* const* modeNames = rtAvailable ? kModeNamesWithRT : kModeNamesWithoutRT;
        const int modeCount = rtAvailable ? IM_ARRAYSIZE(kModeNamesWithRT) : IM_ARRAYSIZE(kModeNamesWithoutRT);

        int modeIndex = static_cast<int>(m_Engine.m_ReflectionMode);
        if (ComboEx(
                "反射の手法###ReflectionMode", &modeIndex, modeNames, modeCount,
                static_cast<int>(KurenaiEngine3D::DefaultReflectionMode(rtAvailable)),
                "SSRは画面に映っているものだけを反射に映す(画面外は反射プローブ/IBLに任せる)。"
                "レイトレーシングはシーン全体へレイを飛ばすため画面外のものも映るが、"
                "ヒット面のテクスチャは読めないためマテリアルの定数色になる"))
        {
            m_Engine.m_ReflectionMode = static_cast<ReflectionMode>(modeIndex);
        }

        if (!rtAvailable)
        {
            ImGui::TextDisabled("レイトレーシングは利用できません(DX12かつDXR Tier 1.1が必要)");
        }

        bool recalcRequested = false;

        if (m_Engine.m_ReflectionMode == ReflectionMode::ScreenSpace)
        {
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
        }
        else if (m_Engine.m_ReflectionMode == ReflectionMode::Raytraced)
        {
            SliderFloatSceneDependent(
                "RT 最大距離###RTReflectionMaxDistance", &m_Engine.m_RTReflectionMaxDistance, 1.0f, 500.0f,
                recalcRequested, "%.3f",
                "反射レイを追跡する最大距離(ワールド単位)。シーン読み込み時に対角長から自動設定される。"
                "短くすると速くなるが、本来映るはずの遠景が空に置き換わる");
            SliderFloatEx(
                "RT 粗さのしきい値###RTReflectionRoughnessCutoff", &m_Engine.m_RTReflectionRoughnessCutoff,
                0.05f, 1.0f, Defaults::RTReflectionRoughnessCutoff, "%.3f", 0,
                "この粗さを超えるマテリアルではレイを撃たない。鏡面レイ1本しか撃たないため、"
                "粗い面では反射プローブ/IBLに任せたほうが正しい");
            CheckboxEx(
                "反射先に影を落とす###RTReflectionShadowRay", &m_Engine.m_RTReflectionShadowRayEnabled,
                Defaults::RTReflectionShadowRayEnabled,
                "反射に映る面から太陽へ影レイを撃つ。切ると反射の中だけ影が消えるが、その分速い");
        }

        if (recalcRequested)
        {
            m_Engine.ResetSceneDependentParams();
        }

        EndParamGroup();
    }
}
