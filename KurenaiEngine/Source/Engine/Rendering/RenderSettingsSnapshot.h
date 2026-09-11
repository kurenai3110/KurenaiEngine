#pragma once

#include "Settings/AmbientOcclusionSettings.h"
#include "Settings/CloudSettings.h"
#include "Settings/DDGISettings.h"
#include "Settings/DebugViewSettings.h"
#include "Settings/EmissiveLightSettings.h"
#include "Settings/GeometrySettings.h"
#include "Settings/IBLSettings.h"
#include "Settings/MegaLightsSettings.h"
#include "Settings/PostProcessSettings.h"
#include "Settings/ReflectionProbeSettings.h"
#include "Settings/ReflectionSettings.h"
#include "Settings/ShadowSettings.h"
#include "Settings/SkySettings.h"
#include "Settings/WaterSettings.h"

// パス群が読む設定を、フレームの先頭で1回だけ写したもの。
//
// 【なぜ写すか】設定はエンジンが持ち、ImGuiのパネルが書き換える。パス群がエンジンの
// 設定を直接引くと、そのために friend が要る。ここへ写しておけば、パス群は
// RenderFrameContext だけを見ればよくなる。
//
// 【写す時点で値は変わらない】UIパネルの描画は Render() の冒頭(m_UIManager->Draw)で
// 終わっており、パス群が読むのはその後なので、いま読んでいる値と同じものが写る。
// **写す位置を Render() の先頭へ上げてはいけない** ―― UIが書く前の値を配ることになる。
//
// 【どの設定を載せるか】パス群が読むものだけ。エンジンとUIしか読まないもの
// (System / Quality / Fog / Stars)は載せない。
namespace Kurenai::Rendering
{
    struct RenderSettingsSnapshot
    {
        AmbientOcclusionSettings AmbientOcclusion;
        CloudSettings Cloud;
        DDGISettings DDGI;
        DebugViewSettings DebugView;
        EmissiveLightSettings EmissiveLight;
        GeometrySettings Geometry;
        IBLSettings IBL;
        MegaLightsSettings MegaLights;
        PostProcessSettings PostProcess;
        ReflectionProbeSettings ReflectionProbe;
        ReflectionSettings Reflection;
        ShadowSettings Shadow;
        SkySettings Sky;
        WaterSettings Water;
    };
}
