#pragma once

#include <cstdint>

namespace Kurenai::Defaults
{
    // KurenaiEngine3Dの各パラメータの既定値。
    //
    // 「KurenaiEngine3D.hのメンバ初期化子」と「UIの『既定値に戻す』」の2箇所に同じ数値を
    // 書くと必ずずれるため、ここを唯一の出所とし、両方がここを参照する。
    //
    // 注意: SSAO/SSILの半径・厚み・SSRの距離/厚みは、シーン読み込みのたびに
    // KurenaiEngine3D::ResetSceneDependentParams()がシーンの対角長から上書きするため、
    // ここの値は「シーンを読む前の初期値」でしかない。UI側はそれらだけ
    // 「既定値に戻す」ではなく「シーンから再計算」を提供する

    // --- AO / 間接光 ---
    inline constexpr bool AOEnabled = true;
    // マテリアルの遮蔽マップ(glTFのocclusionTexture。22章)を間接光へ掛けるか。
    // 上のAOEnabled(スクリーンスペースAO/GI)とは独立した別系統で、
    // 無効にするとObjectConstants.OcclusionStrengthへ0が渡り遮蔽マップの寄与が消える
    inline constexpr bool OcclusionMapEnabled = true;
    inline constexpr float SSAORadius = 0.5f;
    // 遮蔽率にかける指数。1.0は「求めた遮蔽率をそのまま使う」で、上げるほど遮蔽が濃くなる。
    // 素の遮蔽率を基準にしたいため既定は1.0(かつては1.5で、既定のまま濃く付いていた)
    inline constexpr float SSAOPower = 1.0f;
    inline constexpr float SSILRadius = 0.5f;
    inline constexpr float SSILThickness = 0.01f;
    inline constexpr float SSILIntensity = 2.0f;
    // SSAOPowerと同じ意味・同じ理由で1.0
    inline constexpr float SSILPower = 1.0f;
    inline constexpr uint32_t SSILSliceCount = 4;
    inline constexpr uint32_t SSILStepCount = 6;

    // --- シャドウ ---
    inline constexpr bool ShadowEnabled = true;
    inline constexpr float ShadowLightSize = 0.02f;

    // --- IBL / 環境光 / スペキュラ ---
    inline constexpr bool IBLEnabled = true;
    inline constexpr float IBLIntensity = 0.5f;
    inline constexpr bool IBLUseDedicatedIrradiance = false;
    inline constexpr float AmbientScale = 0.2f;
    // スペキュラのマルチスキャッタリング・エネルギー補正の方式。
    // KurenaiEngine3D::SpecularCompensationMode と HLSL の KURENAI_SPEC_COMP_* に対応する
    // (0=Off / 1=Linear / 2=Series / 3=Kulla-Conty)。ここを型付きにするには enum を
    // このヘッダーへ持ち込む必要があるが、EngineDefaults.hは値だけを置く方針なのでintで持つ。
    // 既定のLinearは、実使用域で3方式のうち最も真値に近いことを実測で確認した結果(14.9.8節)
    inline constexpr int SpecularCompensationMode = 1;

    // --- SSR ---
    // レイトレーシング反射に非対応な環境で、SSRを反射の既定の手法にするか。
    // SSRは画面に映っているものしか反射に映せず、画面端で反射が途切れる破綻が目立つため既定は無効
    // (=「反射なし」で起動する)。DXR Tier 1.1対応環境では従来どおりRT反射が既定になる
    // (手法の決定はKurenaiEngine3D::ApplyLoadedScene。EngineDefaults.h冒頭の注意も参照)
    inline constexpr bool SSREnabled = false;
    inline constexpr float SSRMaxDistance = 5.0f;
    inline constexpr float SSRThickness = 0.1f;
    inline constexpr float SSRRoughnessCutoff = 0.6f;

    // --- 水面(P2: 水面マテリアル基盤) ---
    // .ksceneの[Water]セクションが無い場合の既定値。[Water]がある場合はScene::WaterWaveScale等が
    // これらの値をリテラル複製した既定値で初期化され、そちらがKurenaiEngine3D::m_WaterWaveScale等を
    // 上書きする(Scene.hのコメント参照)。ここの値自体は.kscene側のコメントに残した暫定値の流用
    inline constexpr float WaterWaveScale = 12.0f;
    inline constexpr float WaterWaveSpeed = 0.03f;
    inline constexpr float WaterWaveStrength = 0.25f;
    // シーンに依存しないUIつまみ(Scene::WaterWaveScale等と違い.ksceneのキーを持たない)
    inline constexpr bool WaterTimeFrozen = false;
    // 水面のSSR反射で、レイが画面外へ抜けた・最大距離まで判定がつかなかったときに解析空
    // (Sky.hlsli)へフォールバックするか(P4: SSRの水面分岐)。既定ON。
    // 【現状の効果は小さい】空が滑らかな勾配しか持たない間は、プリフィルタ済み鏡面を
    // 低ミップで引いた値と解析評価した値がほぼ一致する(実測: 8bitで最大1、平均0.67)。
    // 差が大きくなるのは空に高周波の要素(雲)が入ってからで、そのとき128pxベースの
    // プリフィルタでは雲の輪郭が色斑に潰れる。既定でONにしてあるのは、
    // 解析評価のほうが情報を落とさない上位互換だから。
    // 手続き空が無効なシーンでは実際には効かない
    // (KurenaiEngine3D::m_WaterAnalyticSkyReflectionのコメント参照)
    inline constexpr bool WaterAnalyticSkyReflection = true;

    // --- 平面反射(P6: 水面への鏡像描画) ---
    inline constexpr bool PlanarReflectionEnabled = true;
    // 反射解像度の倍率(1/2)。水面はラフネスが低いとはいえ波の法線で画面UVを歪ませて引くため
    // 等倍の解像度は要らず、フォワードパス(不透明メッシュ全体)をもう1回走らせるコストの方が
    // 支配的なため、半分に落として負荷を抑える
    inline constexpr float PlanarReflectionResolutionScale = 0.5f;
    // 波の法線による画面UVのずらし量(SSR.hlsl参照)。UV空間の小さな値から始め、実測で調整可能
    inline constexpr float PlanarReflectionDistortion = 0.02f;

    // --- 雲(P5: 積雲1層のレイヤーモデル) ---
    inline constexpr bool CloudEnabled = true;
    // 被覆率。0.45は「晴れ間と雲がおよそ半々」という写真の見た目に寄せた値であり、
    // 物理的な導出ではない(実測で調整可能)
    inline constexpr float CloudCoverage = 0.45f;
    // 雲底の高度[m]。積雲の雲底高度として一般に言われる目安(だいたい1,000〜2,000m)の
    // 中間を採った値(精密な気象観測値ではなく目安からの採用)
    inline constexpr float CloudAltitude = 1500.0f;
    // ノイズ空間のUVスケール[ノイズ空間の距離/m]。積雲1個(ノイズの1セル)がおよそ2kmになるよう
    // 逆算した値(1/2000)。実測ではなく見た目からの調整値
    inline constexpr float CloudUvScale = 1.0f / 2000.0f;
    // 消散係数。既定の光路長(地平線際でクランプ後の最大約20倍)でも雲の芯が十分不透明に見え、
    // かつ薄い箇所では下の空が透けるバランスを見た目で探った調整値(実測で調整可能)
    inline constexpr float CloudDensity = 8.0f;
    // 風速[m/s]。そよ風〜軟風程度(ビューフォート風力階級2〜3相当)の値を感覚的に採用した
    // 調整値であり、実測値ではない
    inline constexpr float CloudWindSpeed = 5.0f;
    // 風向き(度)。太陽方位角の既定値と特に関係を持たせる理由が無いため、東(0度)を既定にした
    inline constexpr float CloudWindDirectionDegrees = 0.0f;
    // Henyey-Greensteinの非対称パラメータ。前方散乱が強すぎると太陽周辺だけが不自然に
    // 明るい点になるため、縁が仄かに光る程度に留めた調整値(実測で調整可能)
    inline constexpr float CloudForwardG = 0.6f;
    // シーンに依存しないUIつまみ(m_WaterTimeFrozenと同じ位置づけ)。
    // 積雲・巻雲の両方に効く(片方だけ凍結できるとA/B比較の対照が取れなくなるため)
    inline constexpr bool CloudTimeFrozen = false;

    // --- 巻雲(P11: 積雲の上に重ねる2層目) ---
    //
    // 【この5つの値の決め方】いずれも見た目からの調整値で物理的な導出ではないが、
    // 最初に置いた値(被覆率0.3・UVスケール1/6000・消散係数0.8)は実測すると
    // 「有効/無効を切り替えても空領域の画素の4.5%しか動かない(平均差0.25/255)」という
    // ほぼ見えない状態だった。原因は主にUVスケールで、下のCirrusUvScaleのコメント参照。
    // 現在の値では同じ比較で23.8%(平均差2.20)になり、高層に筋雲が読み取れる
    inline constexpr bool CirrusEnabled = true;
    // 被覆率。remap(fBm, 1-被覆率, 1)で塊に整形するため、この値が低いとfBmの上位だけが
    // 残ってまばらな筋になる。積雲(0.45)と同程度にして空の広い範囲へ薄く掛かるようにした
    inline constexpr float CirrusCoverage = 0.5f;
    // 雲底の高度[m]。巻雲の高度帯として一般に言われる目安は5,000〜13,000mで、その中ほどを
    // 採った値(精密な気象観測値ではなく目安からの採用。積雲のCloudAltitudeと同じ性格)
    inline constexpr float CirrusAltitude = 8000.0f;
    // ノイズ空間のUVスケール[ノイズ空間の距離/m]。
    // 【積雲と同じ値でよい理由】視線と雲底平面の交点までの距離は高度に比例するため、同じUV
    // スケールでも高度が高い層ほど画角内に入るノイズのセル数が増え、画面上では細かく見える
    // (仰角70度なら積雲は約0.8セル、巻雲は約4.3セル)。巻雲の細い筋・積雲の大きな塊という
    // 見え方の違いは、この高度差だけで自然に出る。当初これを1/6000にしていたときは画角内に
    // 1〜2セルしか入らず、模様が大きすぎて筋として読めなかった
    inline constexpr float CirrusUvScale = 1.0f / 2000.0f;
    // 消散係数。巻雲は光学的に薄く下の青空が透けるのが特徴なので積雲(8.0)より大幅に小さい。
    // 0.8まで下げると上の被覆率・UVスケールと相まってほぼ見えなくなったため、透けは保ちつつ
    // 存在は分かる値まで戻してある
    inline constexpr float CirrusDensity = 2.0f;
    // 風速[m/s]。高層ほど風が速いため積雲(5.0)より大きくしてある。感覚的に採用した
    // 調整値であり実測値ではない。風向きは積雲と共有する(CloudWindDirectionDegrees)
    inline constexpr float CirrusWindSpeed = 15.0f;
    // fBmのUV(U方向)を伸ばして筋状にする倍率。巻雲の「刷毛で掃いたような筋」を作るための
    // 見た目からの調整値。1.0で積雲と同じ等方的な塊になる
    inline constexpr float CirrusAnisotropy = 3.0f;

    // --- レイトレーシング反射(DX12かつDXR Tier 1.1対応時のみ選択できる) ---
    // 最大レイ距離はシーン読み込み時に対角長から決め直す(SSRのMaxDistanceと同じ扱い)。
    // SSRより長いのは、画面外まで追えるRTでは短く切ると反射が途中で空へ抜けてしまうため
    inline constexpr float RTReflectionMaxDistance = 50.0f;
    // SSRと同じく1本の鏡面レイしか撃たないため、粗い面ではプローブ/グローバルIBLへ戻す。
    // SSRより高めなのは、RTには「画面外に外れて打ち切り」という破綻要因が無く、
    // 中程度の粗さでも結果が安定しているため
    inline constexpr float RTReflectionRoughnessCutoff = 0.8f;
    // ヒット面から太陽へ影レイを撃つか。切ると反射に映る面の影が消えるが、その分速い
    inline constexpr bool RTReflectionShadowRayEnabled = true;

    // --- レイトレーシングシャドウ(DX12かつDXR Tier 1.1対応時のみ選択できる) ---
    // 1ピクセルあたりに撃つ影レイの本数。デノイザ(時間方向の蓄積)を持たないため、
    // 太陽を大きくする(角半径を上げる)ほどここを増やさないと半影にノイズが出る
    inline constexpr int RTShadowSampleCount = 4;
    // 太陽の見かけの半径(度)。実際の太陽は視直径約0.53度なので既定値はその半分。
    // 大きくすると半影が広く柔らかくなる(が、同じサンプル数ならノイズも増える)
    inline constexpr float RTShadowSunAngularRadiusDegrees = 0.27f;

    // --- レイトレーシングAO/GI(DX12かつDXR Tier 1.1対応時のみ選択できる) ---
    // 半球へ余弦重みで撃つレイの本数。デノイザを持たずAOBlurのボックスブラーだけで均すため、
    // 少なすぎるとブラー後もノイズが残る
    inline constexpr int RTAOSampleCount = 8;
    // レイの最大距離はシーン読み込み時に対角長から決め直す(SSAO/SSILの半径と同じ扱い)。
    // スクリーンスペース手法より長く取れる(画面外の遮蔽物も追えるため)
    inline constexpr float RTAOMaxDistance = 2.0f;
    // 遮蔽率にかける指数。SSAO/SSILと同じ意味・同じ既定値
    inline constexpr float RTAOPower = 1.5f;
    // 間接拡散光の強さ。物理的に正しい値が1.0になるためSSILの2.0より小さい
    // (SSILの重み付けはヒューリスティックで、1.0では暗すぎた)
    inline constexpr float RTAOIntensity = 1.0f;
    // バウンス面から太陽へ影レイを撃つか。切ると間接光に日陰が反映されなくなるが、その分速い
    inline constexpr bool RTAOBounceShadowRayEnabled = true;

    // --- シャドウ(スクリーンスペース) ---
    // ポイント/スポットライトの影。深度バッファに写っている面しか遮蔽物にできず、
    // 得られるのは接触影・中距離の遮蔽に限られる(画面外の物は影を落とさない)。
    // 効果の範囲が限定的な割に全ライトぶんのレイマーチを毎フレーム走らせるため既定は無効
    inline constexpr bool ScreenSpaceShadowEnabled = false;
    inline constexpr int ScreenSpaceShadowStepCount = 16;
    inline constexpr float ScreenSpaceShadowMaxRayLength = 1.5f;
    inline constexpr float ScreenSpaceShadowThickness = 0.5f;
    inline constexpr float ScreenSpaceShadowNormalBias = 0.002f;
    inline constexpr float ScreenSpaceShadowEdgeFade = 0.1f;
    inline constexpr int ScreenSpaceShadowMaxLightsPerPixel = 4;

    // --- タイルドライトカリング ---
    inline constexpr bool LightCullingEnabled = true;
    inline constexpr int LightTileHeatmapMax = 8;

    // --- 反射プローブ ---
    inline constexpr bool ReflectionProbeEnabled = true;
    inline constexpr bool ProbeParallaxCorrectionEnabled = true;
    inline constexpr bool ProbeBlendingEnabled = true;
    // 距離キューブを使う2つの機能(19.12節)。どちらも実装・検証は済んでいるが、プローブが疎な
    // 現状では副作用のほうが大きいため既定は無効。理由はKurenaiEngine3D.hの各メンバのコメント参照
    inline constexpr bool ProbeDepthParallaxEnabled = false;
    inline constexpr bool ProbeOcclusionEnabled = false;
    // 距離キューブのデバッグ表示で白飽和する距離。ProbeTestのホール(24×12)が収まる程度
    inline constexpr float ProbeDistanceDebugRange = 20.0f;

    // --- DDGI(22章) ---
    // .ksceneに[GIVolume]が無いシーンでは、このフラグに関わらず何も起きない
    inline constexpr bool DDGIEnabled = true;
    inline constexpr float DDGIIntensity = 1.0f;
    // 1フレームに焼き直すプローブ数。DDGIはヒステリシスで時間収束させる手法なので、
    // 全プローブを毎フレーム焼く必要はない(455個ならこの値で約29フレームで一巡する)
    inline constexpr int DDGIProbesPerFrame = 16;

    // --- トーンマップ / ディザ ---
    // 8bit出力時のバンディングを散らすディザ。最終出力へノイズを載せる処理であり、
    // スクリーンショットの画素差を取るA/B比較では差分の下限を押し上げてしまうため既定は無効
    inline constexpr bool DitherEnabled = false;
    inline constexpr float MesopicStrength = 0.0f;

    // --- TAA(Temporal Anti-Aliasing) ---
    // 時間方向に蓄積するため、フレームレートの揺れがそのまま画素差になりA/B比較の妨げになる。
    // 残像・半透明メッシュのゴーストといった副作用もあるため既定は無効
    inline constexpr bool TAAEnabled = false;
    // 今フレームの色を履歴へ混ぜる割合。0.1なら毎フレーム1割ずつ入れ替わるので、
    // 静止していれば十数フレームで収束する。上げるとゴーストに強くなる代わりにちらつきが残る
    inline constexpr float TAABlendWeight = 0.1f;
    // ジッターの振れ幅の倍率。1.0でピクセル内いっぱい(±0.5px)に散らす
    inline constexpr float TAAJitterScale = 1.0f;
    // 蓄積によるボケを補う量。0で無効。TAAの中ではなくTonemapパスで最終出力にのみ掛ける
    inline constexpr float TAASharpness = 0.35f;
    // 近傍クリップのボックス幅(近傍の標準偏差の何倍まで履歴を許容するか)。
    // 小さいほどゴーストに強いがちらつきが増える
    inline constexpr float TAAClipGamma = 1.25f;
    // 静止している画素のちらつきを抑える量。0で無効。動いている画素の挙動は変わらない
    inline constexpr float TAAAntiFlicker = 1.0f;

    // --- ブルーム ---
    // 明部の滲みを足す画作りの処理で、素の輝度分布を確認したいときには邪魔になるため既定は無効
    inline constexpr bool BloomEnabled = false;
    inline constexpr float BloomStrength = 0.06f;
    inline constexpr float BloomThreshold = 1.0f;
    inline constexpr float BloomSoftKnee = 0.5f;

    // --- 自動露出 ---
    // 画面の内容に応じて露出が動くため、カメラを動かすだけで明るさが変わりA/B比較の基準にならない。
    // 無効時はSceneExposureEV100(シーン全体の露出)がそのまま効く固定露出になるため既定は無効
    inline constexpr bool AutoExposureEnabled = false;
    inline constexpr float AutoExposureMinEV100 = -6.0f;
    inline constexpr float AutoExposureMaxEV100 = 18.0f;
    inline constexpr float AutoExposureSpeedUp = 3.0f;
    inline constexpr float AutoExposureSpeedDown = 1.0f;
    inline constexpr float AutoExposureLowPercentile = 0.5f;
    inline constexpr float AutoExposureHighPercentile = 0.95f;
    inline constexpr float AutoExposureCompensation = 0.0f;
    inline constexpr float AutoExposureNightRolloffEV = 4.5f;
    inline constexpr float AutoExposureNightRolloffDarkEV100 = -2.0f;
    inline constexpr float AutoExposureNightRolloffBrightEV100 = 10.0f;
    inline constexpr float AutoExposureKeyCeilingEV = 2.0f;

    // --- 内部レンダー解像度 ---
    // G-Buffer以降すべての中間バッファの解像度。ウィンドウサイズとは独立しており、
    // Presentパスでアスペクト比を保ったままウィンドウへ拡大縮小する(レターボックス/ピラーボックス)。
    // 実行時に「システム」パネルから変更できる
    inline constexpr uint32_t RenderWidth = 1280;
    inline constexpr uint32_t RenderHeight = 720;

    // --- 同期 ---
    inline constexpr bool VSyncEnabled = false;
    inline constexpr bool FixedFPSEnabled = true;
    inline constexpr float TargetFPS = 60.0f;

    // --- デバッグ表示 ---
    inline constexpr float DebugViewGain = 1.0f;

    // --- 太陽 / シーン全体の露出 ---
    inline constexpr bool SunEnabled = true;
    inline constexpr float TimeOfDay = 12.0f;
    inline constexpr bool TimeAutoAdvance = false;
    inline constexpr float TimeAdvanceSpeed = 1.0f;
    inline constexpr float SunAzimuthDegrees = 126.87f;
    // 大気の濁り具合(P7: Preetham xyYモデルのタービディティ)。Preethamの定義域はおおむね
    // 1.7〜10で、2.5は「澄んだ晴天」に相当する見た目からの選択であり、実測値ではない
    inline constexpr float SkyTurbidity = 2.5f;
    // 月は時刻に連動しない独立した向き。ここを変えると夜空の目標照度が変わるため空の焼き直しが要る
    inline constexpr float MoonAzimuthDegrees = 306.87f;
    inline constexpr float MoonElevationDegrees = 45.0f;
    inline constexpr bool ProceduralSkyEnabled = true;
    // 背景(深度が無い画素)をキューブマップのサンプルではなく、Sky.hlsliのSkyColorを画面解像度で
    // 直接評価するか(P3)。キューブマップは256px/面しかなく背景としては拡大表示されるため、
    // 既定でON。手続き空が無効なときはこの設定に関わらずキューブマップが使われる
    inline constexpr bool SkyAnalyticBackground = true;
    inline constexpr float SceneExposureEV100 = 15.0f;
    inline constexpr float EmissiveIntensity = 1.0f;
}
