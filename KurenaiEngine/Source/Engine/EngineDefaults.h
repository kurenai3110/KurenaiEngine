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
    // 月は時刻に連動しない独立した向き。ここを変えると夜空の目標照度が変わるため空の焼き直しが要る
    inline constexpr float MoonAzimuthDegrees = 306.87f;
    inline constexpr float MoonElevationDegrees = 45.0f;
    inline constexpr bool ProceduralSkyEnabled = true;
    inline constexpr float SceneExposureEV100 = 15.0f;
    inline constexpr float EmissiveIntensity = 1.0f;
}
