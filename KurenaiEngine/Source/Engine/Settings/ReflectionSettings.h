#pragma once

#include "../EngineDefaults.h"

namespace Kurenai
{
    // 鏡面反射の手法。どのモードでもLightingパスが適用した鏡面IBLを「差し替える」形で働き、
    // Offならその差し替えを一切行わない(プローブ/グローバルIBLがそのまま残る。20章)
    enum class ReflectionMode
    {
        Off,         // 反射パスを実行しない
        ScreenSpace, // SSR(SSR.hlsl)。画面に映っているものだけが反射に映る
        Raytraced,   // RT反射(RTReflection.hlsl)。画面外も映るが、DX12かつDXR Tier 1.1が要る
    };

    struct ReflectionSettings
    {
        // 「反射を出す」と決まったあとで、環境から**手法だけ**を選ぶ。出すかどうかはここでは決めない。
        // 画面外も反射に映るRTが使えるなら常にそちら
        static constexpr ReflectionMode ReflectionModeForCapability(bool raytracingAvailable)
        {
            return raytracingAvailable ? ReflectionMode::Raytraced : ReflectionMode::ScreenSpace;
        }
        // シーンが何も言っていないときの既定。「反射を出すか」をここで決める。
        //
        // 【この関数に「出すか」と「どの手法か」を兼ねさせてはいけない】兼ねさせると、
        // ApplyLoadedSceneが「シーンが反射を要求している。ではどの手法か」を聞くときにも
        // 同じ関数を使うことになる。Defaults::SSREnabledはfalse(SSRは画面端で反射が途切れる
        // 破綻が目立つため)なので、RTが使えない環境では**.ksceneがScreenSpaceReflection = true
        // と明示していてもReflectionMode::Offが返り、シーンの指定が握り潰される**。
        // DX11でモン・サン=ミシェルの水面に何も映らない、White Furnace TestのSSR回帰テストが
        // 実は動いていない、という形で現れていた(DX12はDXRが使えてRTが選ばれるため露見しなかった)
        static constexpr ReflectionMode DefaultReflectionMode(bool raytracingAvailable)
        {
            return Defaults::SSREnabled ? ReflectionModeForCapability(raytracingAvailable) : ReflectionMode::Off;
        }
        // 現在の手法。RaytracedはSupportsRaytracing()がtrueの環境でしか選べない
        // (UI側で選択不可にする)。ここの初期値はm_RaytracingAvailableが確定する前の値でしかなく、
        // 実際の既定はシーン読み込み時にDefaultReflectionModeで決め直される
        ReflectionMode Mode = DefaultReflectionMode(false);

        float SSRMaxDistance = Defaults::SSRMaxDistance;
        float SSRThickness = Defaults::SSRThickness;
        float SSRRoughnessCutoff = Defaults::SSRRoughnessCutoff;

        float RTReflectionMaxDistance = Defaults::RTReflectionMaxDistance;
        float RTReflectionRoughnessCutoff = Defaults::RTReflectionRoughnessCutoff;
        bool RTReflectionShadowRayEnabled = Defaults::RTReflectionShadowRayEnabled;

        bool PlanarEnabled = Defaults::PlanarReflectionEnabled;
        // 反射解像度の倍率(レンダー解像度に対する比)。EngineDefaults.hのコメント参照
        float PlanarResolutionScale = Defaults::PlanarReflectionResolutionScale;
        // 波の法線による画面UVのずらし量(SSR.hlslが読む)
        float PlanarDistortion = Defaults::PlanarReflectionDistortion;
    };
}
