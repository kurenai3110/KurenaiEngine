#pragma once

#include <cstdint>

#include "../EngineDefaults.h"

namespace Kurenai
{
    // 太陽(平行光)の影の手法。値はDirectLighting.hlslのLightingConstants.LightCount.zへ
    // そのまま渡すため、シェーダ側の分岐と番号を一致させること
    enum class ShadowMode
    {
        Off,                // 影を落とさない
        CascadedShadowMap,  // カスケードシャドウマップ+PCSS(ShadowSampling.hlsli)
        Raytraced,          // RTシャドウ(RTShadow.hlsl)。DX12かつDXR Tier 1.1が要る
    };

    struct ShadowSettings
    {
        // 現在の手法。RaytracedはSupportsRaytracing()がtrueの環境でしか選べない
        // (UI側で選択不可にし、シーン読み込み時にも非対応ならCascadedShadowMapへ落とす)。
        //
        // 【重要】Raytracedでもシャドウパス(CSMの描画)はスキップしない。半透明
        // (Transparent.hlsl)と反射プローブのキャプチャ(ProbeCapture.hlsl)は
        // カメラ視点の画面空間テクスチャを使えず、CSMのシャドウマップを必要とするため
        // (RTシャドウは不透明サーフェスの直接光パスだけを置き換える。26章)
        //
        // 既定の手法はDefaultShadowModeが決める(反射のDefaultReflectionModeと同じ理由で1か所に置く)。
        // ここの初期値はm_RaytracingAvailableが確定する前の値でしかなく、
        // 実際の既定はシーン読み込み時に決め直される
        // 【反射と違い「出すか」と「どの手法か」を分けていない】Defaults::ShadowEnabledがtrueで
        // あるため、シーンがShadow = trueと書いたときにこの関数へ問い合わせても
        // 手法の選択と同じ結果になるため問題にならない。ただし構造は反射と同じ危うさを持つ
        // (DefaultReflectionModeのコメント参照)ので、Defaults::ShadowEnabledを
        // falseにするなら反射と同じ形(ShadowModeForCapabilityへの分割)へ直すこと
        static constexpr ShadowMode DefaultShadowMode(bool raytracingAvailable)
        {
            if (!Defaults::ShadowEnabled)
            {
                return ShadowMode::Off;
            }
            return raytracingAvailable ? ShadowMode::Raytraced : ShadowMode::CascadedShadowMap;
        }

        ShadowMode Mode = DefaultShadowMode(false);
        // PCSS(Percentage Closer Soft Shadows)のライトサイズ。シャドウマップUV空間での
        // ブロッカーサーチ・半影の広さを決める係数(値が大きいほど半影が広く柔らかくなる)
        float LightSize = Defaults::ShadowLightSize;
        // デバッグ表示(Render Targets - Shadow Map)で確認するカスケード番号(0=カメラに近い方)
        int32_t DebugCascade = 0;

        // ポイント/スポットライトのスクリーンスペースシャドウ(接触影)の設定。
        // シャドウマップを増やさず、G-Bufferの深度バッファをライト方向へレイマーチして影を出す
        // (Shaders/3D/ScreenSpaceShadow.hlsli、docs/Architecture.html 18章)
        bool ScreenSpaceEnabled = Defaults::ScreenSpaceShadowEnabled;
        // レイマーチのステップ数。ScreenSpaceShadow.hlsliのkSSSMaxStepCount(64)が上限
        int ScreenSpaceStepCount = Defaults::ScreenSpaceShadowStepCount;
        // 1本のレイが伸びる最大のワールド距離。ライトまでの距離がこれより短ければそちらが優先される。
        // 短いほど「接触影」寄りになり、コストも下がる
        float ScreenSpaceMaxRayLength = Defaults::ScreenSpaceShadowMaxRayLength;
        // 遮蔽と判定する深度差の上限。深度バッファがサーフェスの厚みを持たないための近似で、
        // 大きすぎると遠景が無限に厚い遮蔽物として振る舞い、小さすぎると薄い物体を貫通する
        float ScreenSpaceThickness = Defaults::ScreenSpaceShadowThickness;
        // レイ始点を法線方向へ押し出す量(View空間深度に比例させる係数)。自己遮蔽(シャドウアクネ)対策
        float ScreenSpaceNormalBias = Defaults::ScreenSpaceShadowNormalBias;
        // ヒット位置が画面端に近いときに影を弱める幅(UV単位)。SSRのkSSREdgeFadeDistanceと同じ役割
        float ScreenSpaceEdgeFade = Defaults::ScreenSpaceShadowEdgeFade;
        // 1ピクセルが撃てるシャドウレイ数の上限。ライトを増やしてもレイマーチのコストが
        // 線形に伸び続けないようにするための予算
        int ScreenSpaceMaxLightsPerPixel = Defaults::ScreenSpaceShadowMaxLightsPerPixel;

        int32_t RTSampleCount = Defaults::RTShadowSampleCount;
        float RTSunAngularRadiusDegrees = Defaults::RTShadowSunAngularRadiusDegrees;
    };
}
