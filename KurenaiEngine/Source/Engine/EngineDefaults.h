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
    inline constexpr float SSAORadius = 0.5f;
    inline constexpr float SSAOPower = 1.5f;
    inline constexpr float SSILRadius = 0.5f;
    inline constexpr float SSILThickness = 0.01f;
    inline constexpr float SSILIntensity = 2.0f;
    inline constexpr float SSILPower = 1.5f;
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
    inline constexpr bool SpecularEnergyCompensationEnabled = true;

    // --- SSR ---
    inline constexpr bool SSREnabled = true;
    inline constexpr float SSRMaxDistance = 5.0f;
    inline constexpr float SSRThickness = 0.1f;
    inline constexpr float SSRRoughnessCutoff = 0.6f;

    // --- シャドウ(スクリーンスペース) ---
    inline constexpr bool ScreenSpaceShadowEnabled = true;
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

    // --- トーンマップ / ディザ ---
    inline constexpr bool DitherEnabled = true;
    inline constexpr float MesopicStrength = 0.0f;

    // --- TAA(Temporal Anti-Aliasing) ---
    inline constexpr bool TAAEnabled = true;
    // 今フレームの色を履歴へ混ぜる割合。0.1なら毎フレーム1割ずつ入れ替わるので、
    // 静止していれば十数フレームで収束する。上げるとゴーストに強くなる代わりにちらつきが残る
    inline constexpr float TAABlendWeight = 0.1f;
    // ジッターの振れ幅の倍率。1.0でピクセル内いっぱい(±0.5px)に散らす
    inline constexpr float TAAJitterScale = 1.0f;
    // 蓄積によるボケを補う量。0で無効
    inline constexpr float TAASharpness = 0.35f;

    // --- ブルーム ---
    inline constexpr bool BloomEnabled = true;
    inline constexpr float BloomStrength = 0.06f;
    inline constexpr float BloomThreshold = 1.0f;
    inline constexpr float BloomSoftKnee = 0.5f;

    // --- 自動露出 ---
    inline constexpr bool AutoExposureEnabled = true;
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
