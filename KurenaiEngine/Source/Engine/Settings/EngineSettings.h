#pragma once

#include "Settings/AmbientOcclusionSettings.h"
#include "Settings/CloudSettings.h"
#include "Settings/DDGISettings.h"
#include "Settings/DebugViewSettings.h"
#include "Settings/EmissiveLightSettings.h"
#include "Settings/FogSettings.h"
#include "Settings/GeometrySettings.h"
#include "Settings/IBLSettings.h"
#include "Settings/MegaLightsSettings.h"
#include "Settings/PostProcessSettings.h"
#include "Settings/QualitySettings.h"
#include "Settings/ReflectionProbeSettings.h"
#include "Settings/ReflectionSettings.h"
#include "Settings/ShadowSettings.h"
#include "Settings/SkySettings.h"
#include "Settings/StarsSettings.h"
#include "Settings/SystemSettings.h"
#include "Settings/WaterSettings.h"

// 機能ごとの設定構造体を1つにまとめた入れ物。
//
// 【なぜまとめるか】以前はエンジンが18個の設定を個別のメンバとして持ち、
// UIホストがそれぞれに getter を1本ずつ生やしていた。読む側から見ると
// 「設定を渡す」だけのことに18本の口が要る状態で、設定を1つ足すたびに
// エンジンの公開面も1本増えていた。
//
// 【Rendering::RenderSettingsSnapshot と混同しないこと】あちらはパス群へ配るための
// **写し**で、フレームの先頭で1回だけ作る。**実体をあちらと共有させてはいけない** ――
// UIが書いた瞬間にパス群の見る値が変わり、「写した時点の値で1フレームを通す」という
// 規約が壊れる。絵は変わらない公算が高く、**採取では捕まらない**種類の劣化になる。
// 写しを作る口は KurenaiEngine3D::FillFrameContextSnapshot 1本だけにしてある。
namespace Kurenai::Settings
{
    struct EngineSettings
    {
        AmbientOcclusionSettings AmbientOcclusion;
        CloudSettings Cloud;
        DDGISettings DDGI;
        DebugViewSettings DebugView;
        EmissiveLightSettings EmissiveLight;
        FogSettings Fog;
        GeometrySettings Geometry;
        IBLSettings IBL;
        MegaLightsSettings MegaLights;
        PostProcessSettings PostProcess;
        ReflectionProbeSettings ReflectionProbe;
        ReflectionSettings Reflection;
        ShadowSettings Shadow;
        SkySettings Sky;
        StarsSettings Stars;
        SystemSettings System;
        WaterSettings Water;
        QualitySettings Quality;
    };
}
