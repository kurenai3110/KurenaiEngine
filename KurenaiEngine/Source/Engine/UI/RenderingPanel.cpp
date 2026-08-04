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
        if (ImGui::CollapsingHeader("メッシュレット###Meshlet"))
        {
            DrawMeshletSection();
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
        if (ImGui::CollapsingHeader("水面###Water"))
        {
            DrawWaterSection();
        }
        if (ImGui::CollapsingHeader("雲###Clouds"))
        {
            DrawCloudSection();
        }
        if (ImGui::CollapsingHeader("大気遠近###Fog"))
        {
            DrawFogSection();
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

    void RenderingPanel::DrawMeshletSection()
    {
        ImGui::TextWrapped(
            "メッシュを頂点64個・三角形124個までの塊(メッシュレット)に分け、"
            "増幅シェーダーが塊ごとに錐台カリングと法線コーンによる背面カリングを行ってから、"
            "生き残った塊だけをメッシュシェーダーがラスタライザへ流す。"
            "従来の描画はDrawIndexed 1回=メッシュ全体が単位で、画面外の三角形も"
            "すべてラスタライザまで到達していた。"
            "タイルドライトカリングと同じく純粋な最適化であり、"
            "有効/無効で最終画像が変わってはならない");

        if (!m_Engine.m_MeshShaderAvailable)
        {
            // 非対応環境(DX11、メッシュシェーダーTier 1未満、bindless非対応)。
            // 影・反射の手法選択と同じく、選べないものは操作させずに理由だけ示す
            ImGui::TextWrapped(
                "この環境ではメッシュシェーダーを使えないため、常に従来の頂点シェーダーで描画する。"
                "DX12かつメッシュシェーダー Tier 1・シェーダーモデル6.6に対応したGPUが必要");
            return;
        }

        BeginParamGroup();

        CheckboxEx(
            "メッシュレット描画を有効にする###EnableMeshlet", &m_Engine.m_MeshletRenderingEnabled, true,
            "無効にすると従来の頂点シェーダー + DrawIndexedで描く。"
            "切り替えても見た目は一致するはずで、変わる場合はメッシュシェーダー側の変換が"
            "頂点シェーダーとずれている");

        CheckboxEx(
            "メッシュレットを色分けして表示###MeshletDebugView", &m_Engine.m_MeshletDebugViewEnabled, false,
            "塊ごとに違う色でアルベドを塗る。分割のされ方を目で確かめるためのもので、"
            "法線・深度・モーションベクターは通常どおり書くため他のパスは破綻しない。"
            "灰色に見える面はメッシュレットを経由していない(メッシュレットが焼かれていない"
            "モデル、または水面)。上の「メッシュレット描画」が有効なときだけ効く");

        EndParamGroup();

        // .kmodelが--no-meshletsで焼かれていると、対応環境でも0のままになる。
        // 「有効にしたのに何も変わらない」ときの切り分けに要るので数を出しておく
        size_t meshletCount = 0;
        size_t meshletMeshCount = 0;
        size_t meshCount = 0;
        for (const auto& instance : m_Engine.m_Scene.Instances)
        {
            for (const auto& mesh : instance.Model.Meshes)
            {
                ++meshCount;
                meshletCount += mesh.MeshletCount;
                if (mesh.MeshletCount > 0)
                {
                    ++meshletMeshCount;
                }
            }
        }
        ImGui::Text(
            "メッシュレット: %zu (メッシュ %zu / %zu が保持)", meshletCount, meshletMeshCount, meshCount);
        if (meshCount > 0 && meshletMeshCount == 0)
        {
            ImGui::TextWrapped(
                "このシーンのモデルはメッシュレットを持っていない。KurenaiPackerで"
                "--no-meshletsを付けずに再パックすること");
        }
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

        // bent normalによる遮蔽(34章)。ベイク済みのbent normalを持つモデルでのみ効く
        // (持たないマテリアルは黒1x1へフォールバックし、どのトグルでも見た目が変わらない)
        CheckboxEx(
            "ディフューズAOにbent normalを使う###BentNormalAOSource", &m_Engine.m_BentNormalAOSource,
            Defaults::BentNormalAOSource,
            "拡散光の遮蔽を aoN = dot(N, bRaw) から求める。無効にすると従来のベイク済みAOを使う。"
            "どちらも同じ積分の別推定量なので、切り替えても見た目はほとんど変わらないのが正常");
        {
            static const char* const kSpecularOcclusionModes[] = {
                "Frostbite近似(方向を見ない)",
                "球冠交差",
                "球面ガウス(推奨)",
            };
            int mode = static_cast<int>(m_Engine.m_SpecularOcclusionMode);
            if (ComboEx(
                    "スペキュラ遮蔽の方式###SpecularOcclusionMode", &mode, kSpecularOcclusionModes,
                    IM_ARRAYSIZE(kSpecularOcclusionModes), Defaults::SpecularOcclusionMode,
                    "鏡面の遮蔽方式。壁際で、壁を向いた反射だけが暗くなるのが「Frostbite近似」以外の"
                    "共通の挙動。「球冠交差」は可視性を二値の球冠として扱うため、遮蔽が強い金属の凹部が"
                    "純黒へ潰れることがある(34.10節)。「球面ガウス」は同じ交差を柔らかい分布に"
                    "置き換えたもので、方向性を保ったまま潰れを避ける(34.11節)。既定は球面ガウス"))
            {
                m_Engine.m_SpecularOcclusionMode =
                    static_cast<KurenaiEngine3D::SpecularOcclusionMode>(mode);
            }
        }
        CheckboxEx(
            "multi-bounce AO###MultiBounceAO", &m_Engine.m_MultiBounceAOEnabled,
            Defaults::MultiBounceAOEnabled,
            "アルベドが明るいほどAOを弱める補正(Jimenez 2016)。物理的にはより正しいが"
            "見た目を大きく変えるため既定では無効");

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
                // 戻る先はエンジンの既定ではなく「このシーンを読み込んだ直後」
                // (m_SceneDefaultReflectionModeのコメント参照)
                static_cast<int>(m_Engine.m_SceneDefaultReflectionMode),
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

    void RenderingPanel::DrawWaterSection()
    {
        ImGui::TextWrapped(
            "水面(.ksceneの[Model]Water=trueで指定されたインスタンス)の波の見た目。"
            "反射自体は他の不透明マテリアルと同じSSR/RTレイトレーシング反射パスがそのまま適用される"
            "(下の「水面の反射に解析的な空を使う」は、SSRが画面外へ抜けた・判定がつかなかった"
            "水面画素だけを対象にしたフォールバック先の差し替え)。"
            "デバッグ表示の「水面マスク」で対象インスタンスを確認できる。"
            "下の「平面反射」は不透明ジオメトリの鏡像を映す専用パスで、"
            "反射の手法がSSR(上の「反射」セクション)のときだけ効く"
            "(SSR.hlslがこのパスの結果を消費するため。レイトレーシング反射・反射なしのときは"
            "何も起きない)");

        BeginParamGroup();

        CheckboxEx(
            "水面アニメを止める###FreezeWaterTime", &m_Engine.m_WaterTimeFrozen, Defaults::WaterTimeFrozen,
            "水面法線マップのスクロールを止める。A/B比較などスクロールが揺れると困る場面で使う");

        CheckboxEx(
            "水面の反射に解析的な空を使う###WaterAnalyticSkyReflection",
            &m_Engine.m_WaterAnalyticSkyReflection, Defaults::WaterAnalyticSkyReflection,
            "水面のSSRレイが画面外へ抜けた、または最大距離まで判定がつかなかったとき、"
            "プリフィルタ済み鏡面IBL(128pxベースのキューブマップをラフネス由来のミップで引く)の"
            "代わりに、Perez分布(手続き空)を画面解像度で直接評価した空を映す。"
            "空が滑らかな勾配しか持たない間は両者の差はごくわずかで、差が出るのは空に"
            "雲のような高周波の要素が入ってから。効果が出るのは「反射の手法」がSSRのときだけ"
            "(レイトレーシング反射・反射なしのときは何も起きない)。既定でSSRは無効なので、"
            "このトグルの効果を確認するには上の「反射」セクションの「反射の手法」でSSRを"
            "選ぶ必要がある。手続き空が無効(.ksceneでスカイボックス画像を明示したシーン)なときは、"
            "このトグルの値に関わらず従来どおりプリフィルタ済み鏡面のままになる");

        // WaveScale/WaveStrengthはFrameConstants.TimeParams.y/zとしてWater.hlslへ渡っている
        // (KurenaiEngine3D::RenderThreadMainのTimeParams構築箇所、Water.hlslのPSMain参照)。
        // つまみ自体はScene::WaterWaveScale等から読み込める値をUIで確認・上書きできるよう用意してある
        SliderFloatEx(
            "波のスケール###WaterWaveScale", &m_Engine.m_WaterWaveScale, 1.0f, 64.0f, Defaults::WaterWaveScale,
            "%.2f", ImGuiSliderFlags_Logarithmic,
            "水面法線マップのタイリングスケール。値が大きいほど波紋の繰り返しが細かくなる");
        SliderFloatEx(
            "波の速度###WaterWaveSpeed", &m_Engine.m_WaterWaveSpeed, 0.0f, 1.0f, Defaults::WaterWaveSpeed, "%.3f", 0,
            "水面法線マップのスクロール速度。m_WaterScrollOffsetの進行速度に直接効く");
        SliderFloatEx(
            "波の強さ###WaterWaveStrength", &m_Engine.m_WaterWaveStrength, 0.0f, 1.0f, Defaults::WaterWaveStrength,
            "%.3f", 0, "波打ちの振幅。0で波が完全に消え平坦な鏡面、1で最大の揺らぎになる");

        // --- 水中項(P8) ---
        // ColorEdit3系のヘルパはUIWidgets.hに無いため、既存パネルと同じSliderFloatExを3本並べる
        ImGui::TextWrapped(
            "水面メッシュのAlbedo(誘電体でほぼ黒に焼かれている)の代わりに使う、水体の拡散反射色"
            "(リニア)。屈折・水深依存の減衰は含まない粗い近似(G-Bufferが水深の情報を持たないため)。"
            "見下ろすと水の色、すれすれだと鏡になる切り替わりはFresnelの式(DeferredLighting.hlsl)が"
            "そのまま担当する");
        SliderFloatEx(
            "水体の色 R###WaterBodyColorR", &m_Engine.m_WaterBodyColor.x, 0.0f, 0.5f, Defaults::WaterBodyColorR,
            "%.4f", 0, "水体の拡散反射色(リニア)の赤成分");
        SliderFloatEx(
            "水体の色 G###WaterBodyColorG", &m_Engine.m_WaterBodyColor.y, 0.0f, 0.5f, Defaults::WaterBodyColorG,
            "%.4f", 0, "水体の拡散反射色(リニア)の緑成分");
        SliderFloatEx(
            "水体の色 B###WaterBodyColorB", &m_Engine.m_WaterBodyColor.z, 0.0f, 0.5f, Defaults::WaterBodyColorB,
            "%.4f", 0, "水体の拡散反射色(リニア)の青成分");

        // --- 平面反射(P6) ---
        CheckboxEx(
            "平面反射を有効にする###PlanarReflectionEnabled", &m_Engine.m_PlanarReflectionEnabled,
            Defaults::PlanarReflectionEnabled,
            "水面に不透明ジオメトリの鏡像を映す専用のフォワードパス(鏡映カメラで景色を描き直す)。"
            "効果が出るのは上の「反射」セクションで手法にSSRを選んだときだけ"
            "(SSR.hlslがこのパスの結果を消費するため)。無効時は解析空(有効なら)へフォールバックする");

        {
            static const char* kPlanarReflectionResolutionNames[] = { "1/1", "1/2", "1/4" };
            static const float kPlanarReflectionResolutionValues[] = { 1.0f, 0.5f, 0.25f };
            constexpr int kDefaultResolutionIndex = 1; // Defaults::PlanarReflectionResolutionScale = 0.5f = 1/2

            int resolutionIndex = kDefaultResolutionIndex;
            for (int i = 0; i < IM_ARRAYSIZE(kPlanarReflectionResolutionValues); ++i)
            {
                if (m_Engine.m_PlanarReflectionResolutionScale == kPlanarReflectionResolutionValues[i])
                {
                    resolutionIndex = i;
                    break;
                }
            }

            if (ComboEx(
                    "反射解像度###PlanarReflectionResolution", &resolutionIndex, kPlanarReflectionResolutionNames,
                    IM_ARRAYSIZE(kPlanarReflectionResolutionNames), kDefaultResolutionIndex,
                    "平面反射パスの解像度をレンダー解像度に対する倍率で指定する。水面は波で歪むため"
                    "等倍(1/1)にしても劇的には変わらない一方、フォワードパスをもう1回走らせる"
                    "コストは解像度の2乗で効く"))
            {
                // レンダーターゲットの作り直しはGPUがまだ参照しているかもしれないため、
                // ここでは要求を記録するだけにする(Render()の先頭でまとめて反映)
                m_Engine.RequestPlanarReflectionResolutionScale(kPlanarReflectionResolutionValues[resolutionIndex]);
            }
        }

        SliderFloatEx(
            "波による歪み###PlanarReflectionDistortion", &m_Engine.m_PlanarReflectionDistortion, 0.0f, 0.1f,
            Defaults::PlanarReflectionDistortion, "%.4f", 0,
            "波の法線ベクトル(N.xz)を画面UVのずらし量に変換する係数。0で歪みなし(完全な鏡)、"
            "大きいほど波打ちが強く見える");

        EndParamGroup();
    }

    void RenderingPanel::DrawCloudSection()
    {
        ImGui::TextWrapped(
            "積雲(低層)+巻雲(高層)の2層レイヤーモデル(Sky.hlsliが背景・水面反射・IBLベイクの"
            "3者で共有する空モデルへ足したもの)。背景と水面の映り込みの両方にそのまま出る一方、"
            "IBL(反射プローブ・拡散イラディアンス)には焼き込まれない。雲は風で動くたびに"
            "キューブマップを焼き直すと無駄が大きいためで、代わりに両層の被覆率から求めた"
            "全天の平均透過率でキューブ全体の明るさを一括して暗くするだけに留めている"
            "(そのためIBLに映る雲の「形」は無い)。太陽の直接光やシャドウは雲による減光の対象外。"
            "合成は高い層(巻雲)から手前(積雲)へ行い、巻雲の光は積雲を透過して初めて届く"
            "(積雲に隠れる巻雲が透けて見えないようにするため)");

        BeginParamGroup();

        ImGui::SeparatorText("積雲(低層)");

        // このトグルはCloudCoverageスライダーと同じくIBLキューブの明るさ(平均透過率)に効く
        // (m_CloudEnabled=falseのときComputeCloudAverageTransmittanceは常に1.0を返す)。
        // 立てないと、無効にした直後もIBLが「有効だったときの暗さ」のまま次の自然な再ベイク
        // (太陽が動く・露出が変わる等)まで取り残されてしまうため、被覆率スライダーと同じ扱いにする
        if (CheckboxEx(
                "雲を有効にする###CloudEnabled", &m_Engine.m_CloudEnabled, Defaults::CloudEnabled,
                "無効にすると被覆率0と同じ扱いになり、Sky.hlsli側の雲の計算(密度・自己影・位相関数)を"
                "一切行わない"))
        {
            m_Engine.m_SkyBakeDirty = true;
        }
        CheckboxEx(
            "雲を止める(凍結)###FreezeCloudTime", &m_Engine.m_CloudTimeFrozen, Defaults::CloudTimeFrozen,
            "風によるノイズのスクロールを止める。積雲・巻雲の両方に効く(片方にしか効かないと"
            "A/B比較でスクロールが揺れる側だけ残ってしまい対照が取れなくなるため)。"
            "水面の「水面アニメを止める」と同じ位置づけ");

        // 被覆率(と上の有効トグル)だけがIBLキューブの明るさ(平均透過率)に効くため、これが
        // 変わったときだけ手続き空の再ベイクを要求する。他のつまみ(高度・UVスケール・密度・風・
        // 位相関数)は背景・水面反射の見た目にしか影響せずキューブの中身(IBLベイク結果)には
        // 影響しないため、ここでm_SkyBakeDirtyを立てると無関係な再ベイク(空生成6回+
        // プリフィルタ36回のディスパッチ)が余計に走ってしまう(m_ProceduralSkyEnabledトグルと
        // 同じ判断基準)
        if (SliderFloatEx(
                "被覆率###CloudCoverage", &m_Engine.m_CloudCoverage, 0.0f, 1.0f, Defaults::CloudCoverage, "%.2f", 0,
                "0=雲なし、1=全天が雲。IBLキューブへ焼く天頂輝度にだけ、この値から求めた平均透過率を"
                "掛けて全体を暗くする(背景・水面反射に見える空自体は減光しない。二重に暗くなるのを"
                "避けるため)"))
        {
            m_Engine.m_SkyBakeDirty = true;
        }

        SliderFloatEx(
            "雲底の高度###CloudAltitude", &m_Engine.m_CloudAltitude, 200.0f, 5000.0f, Defaults::CloudAltitude,
            "%.0f m", 0, "雲底の高さ(カメラのワールドY基準)。視線とこの高さの平面との交点から雲のUVを"
            "作るレイヤーモデルのため、値を大きくすると地平線際の雲がより遠くに、小さくすると近くに見える");
        SliderFloatEx(
            "UVスケール###CloudUvScale", &m_Engine.m_CloudUvScale, 1.0f / 8000.0f, 1.0f / 500.0f,
            Defaults::CloudUvScale, "%.6f", ImGuiSliderFlags_Logarithmic,
            "ワールド1mあたりのノイズ空間の距離。大きいほど雲の塊(1個あたり)が小さく見える");
        SliderFloatEx(
            "密度###CloudDensity", &m_Engine.m_CloudDensity, 0.0f, 30.0f, Defaults::CloudDensity, "%.2f", 0,
            "消散係数。ビアの法則(exp(-density*経路長))で透過率を決める。大きいほど雲が不透明になり"
            "自己影も濃くなる");
        SliderFloatEx(
            "風速###CloudWindSpeed", &m_Engine.m_CloudWindSpeed, 0.0f, 30.0f, Defaults::CloudWindSpeed, "%.2f m/s",
            0, "雲のノイズを流す速度。実世界の速度[m/s]として扱える");
        SliderFloatEx(
            "風向###CloudWindDirection", &m_Engine.m_CloudWindDirectionDegrees, 0.0f, 360.0f,
            Defaults::CloudWindDirectionDegrees, "%.1f deg", 0,
            "風が吹いていく向き。太陽の方位角と同じ規約(X軸0度、Z軸(+方向)90度)。"
            "巻雲(下記)も同じ風向を共有する(速度・UVスケールだけ別に持つ)");
        SliderFloatEx(
            "前方散乱g###CloudForwardG", &m_Engine.m_CloudForwardG, 0.0f, 0.95f, Defaults::CloudForwardG, "%.2f", 0,
            "Henyey-Greensteinの非対称パラメータ。大きいほど太陽を直視する方向で雲の縁が強く光る"
            "(半逆光のシルバーライニング効果)");

        CheckboxEx(
            "ボリュームとして描く###CloudVolumetric", &m_Engine.m_CloudVolumetric, Defaults::CloudVolumetric,
            "積雲を雲底から雲頂までのスラブとしてレイマーチする(P13b)。切ると従来の厚みゼロの"
            "平面レイヤーへ戻るので、見た目と負荷をそのまま比べられる");
        if (m_Engine.m_CloudVolumetric)
        {
            SliderFloatEx(
                "厚み###CloudThickness", &m_Engine.m_CloudThickness, 100.0f, 3000.0f, Defaults::CloudThickness,
                "%.0f m", 0,
                "雲底から雲頂までの厚み。目安は扁平雲(humilis)が約400m、並雲(mediocris)が約1000m、"
                "雄大積雲(congestus)が約2500m。厚いほど縦に伸びた入道雲になる");
        }

        ImGui::SeparatorText("巻雲(高層)");
        ImGui::TextWrapped(
            "積雲より高い位置にある2層目。光学的に薄いため自己影は計算せず(常に太陽光がそのまま"
            "透過する扱い)、前方散乱の強さも積雲よりシェーダ内定数で弱めにしてある。"
            "風向・凍結トグルは積雲と共有する");

        // 巻雲の被覆率もIBLキューブの明るさ(平均透過率)に効くため、積雲のCloudCoverageと同じ判断基準で
        // 変わったときだけ再ベイクを要求する
        if (CheckboxEx(
                "巻雲を有効にする###CirrusEnabled", &m_Engine.m_CirrusEnabled, Defaults::CirrusEnabled,
                "無効にすると被覆率0と同じ扱いになり、Sky.hlsli側の巻雲の計算を一切行わない"))
        {
            m_Engine.m_SkyBakeDirty = true;
        }
        if (SliderFloatEx(
                "被覆率###CirrusCoverage", &m_Engine.m_CirrusCoverage, 0.0f, 1.0f, Defaults::CirrusCoverage, "%.2f",
                0,
                "0=巻雲なし、1=全天が巻雲。IBLキューブへ焼く天頂輝度にだけ、この値から求めた"
                "平均透過率(積雲との積)を掛けて全体を暗くする"))
        {
            m_Engine.m_SkyBakeDirty = true;
        }
        SliderFloatEx(
            "雲底の高度###CirrusAltitude", &m_Engine.m_CirrusAltitude, 3000.0f, 15000.0f, Defaults::CirrusAltitude,
            "%.0f m", 0,
            "雲底の高さ(カメラのワールドY基準)。巻雲の高度帯として一般に言われる目安"
            "(だいたい5,000〜13,000m)");
        SliderFloatEx(
            "UVスケール###CirrusUvScale", &m_Engine.m_CirrusUvScale, 1.0f / 12000.0f, 1.0f / 1000.0f,
            Defaults::CirrusUvScale, "%.6f", ImGuiSliderFlags_Logarithmic,
            "ワールド1mあたりのノイズ空間の距離。積雲より小さめが自然(巻雲は1つ1つの塊が大きく広がるため)");
        SliderFloatEx(
            "密度###CirrusDensity", &m_Engine.m_CirrusDensity, 0.0f, 5.0f, Defaults::CirrusDensity, "%.2f", 0,
            "消散係数。巻雲は光学的に薄いため積雲より1桁小さい値を想定している");
        SliderFloatEx(
            "風速###CirrusWindSpeed", &m_Engine.m_CirrusWindSpeed, 0.0f, 60.0f, Defaults::CirrusWindSpeed,
            "%.2f m/s", 0, "巻雲のノイズを流す速度。高層ほど風が速いという一般的な傾向に合わせ"
            "積雲より速めにしてある");
        SliderFloatEx(
            "異方性(筋状)###CirrusAnisotropy", &m_Engine.m_CirrusAnisotropy, 1.0f, 8.0f, Defaults::CirrusAnisotropy,
            "%.2f", 0, "fBmのUV(U方向)を伸ばして筋状にする倍率。1.0で積雲と同じ等方形状になる");

        EndParamGroup();
    }

    void RenderingPanel::DrawFogSection()
    {
        ImGui::TextWrapped(
            "大気遠近(height fog)。高度で指数減衰する消散係数を持つ均一媒質を光線に沿って解析的に"
            "積分した透過率で、遠方ほど背景の空色(Sky.hlsliのSkyColor)へ滲んでいく。"
            "反射パス(SSR/RT反射)の後、TAAの直前で1回だけ適用され、半透明メッシュにも及ぶ。"
            "手続き空が無効(.ksceneでスカイボックス画像を明示したシーン)なときは、"
            "このセクションの設定に関わらず常に無効になる(合成先の空の色を解析評価できないため)");

        BeginParamGroup();

        CheckboxEx(
            "大気遠近を有効にする###FogEnabled", &m_Engine.m_FogEnabled, Defaults::FogEnabled,
            "無効にするとパス自体が実行されず、反射パスの出力がそのままTAA(またはトーンマップ)へ渡る"
            "(密度を0にした場合とも数値上区別が付かないため、切り分け用にトグルを分けている)");
        SliderFloatEx(
            "消散係数###FogDensity", &m_Engine.m_FogDensity, 0.0f, 0.002f, Defaults::FogDensity, "%.5f",
            ImGuiSliderFlags_Logarithmic,
            "基準高度(下記)での消散係数[1/m]。大きいほど濃い霧になる。"
            "気象学的視程Vとは Koschmieder の V = 3.912 / 消散係数 で結び付くので、"
            "「どのくらいの視程を想定するか」で決めるのが分かりやすい"
            "(0.0004 で V ≒ 10km、0.0002 で V ≒ 20km、上限の0.002 で V ≒ 2km のもや)。"
            "上限は以前0.01(V ≒ 391m)だったが、600m先の島すら見えず屋外の風景として"
            "成立しない領域だったので下げた");
        SliderFloatEx(
            "スケールハイト###FogScaleHeight", &m_Engine.m_FogScaleHeight, 10.0f, 5000.0f, Defaults::FogScaleHeight,
            "%.0f m", 0, "霧の層の厚み。大きいほど高い高度まで霧が及ぶ");
        SliderFloatEx(
            "基準高度###FogRefHeight", &m_Engine.m_FogRefHeight, -500.0f, 500.0f, Defaults::FogRefHeight, "%.0f m",
            0, "消散係数(上記)を定義する高さ(ワールドY)。既定は水面の高さに合わせている");
        SliderFloatEx(
            "不透明度の上限###FogMaxOpacity", &m_Engine.m_FogMaxOpacity, 0.0f, 1.0f, Defaults::FogMaxOpacity, "%.2f",
            0, "1.0で遠方が完全に空の色まで行く。下げると最遠方でもうっすら元の色が透けて残る");

        EndParamGroup();
    }
}
