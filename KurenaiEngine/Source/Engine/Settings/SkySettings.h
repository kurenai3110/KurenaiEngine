#pragma once

#include "../EngineDefaults.h"

namespace Kurenai
{
    struct SkySettings
    {
        // DebugView::AtmosphereLUT で表示するLUT
        // (0=Transmittance、1=MultiScattering、2=SkyView)
        int AtmosphereLUTDebugIndex = 0;

        // 太陽(平行光)そのものの有効/無効。.ksceneの[Sun]Enabledで設定される。
        // TimeOfDayを夜にすると昼度(AmbientColor.a)も一緒に落ちて環境光まで消えてしまうため、
        // 「昼のまま太陽だけ消す」にはこちらを使う(White Furnace Testが必要とする)。
        // 無効時はFrameConstants.LightColorをゼロにするだけでよく、シェーダー側の変更は不要
        bool SunEnabled = Defaults::SunEnabled;
        bool ProceduralEnabled = Defaults::ProceduralSkyEnabled;
        // 焼き直しの角度閾値(度)。Auto Advance既定(1h/s)では太陽は15度/秒動くので、
        // 1.0度なら毎秒15回の焼き直しになる。空の見た目は15Hz更新でも連続に見える
        float BakeAngleThresholdDegrees = 1.0f;

        // 背景(深度が書かれていない画素)をキューブマップのサンプルではなく、Sky.hlsliの
        // SkyColorを画面解像度で直接評価するか。キューブマップは256px/面しかなく
        // 3840px・水平画角68度のカメラでは約20倍に拡大表示されるため、既定で有効にしてある。
        // 手続き空が無効(.ksceneのDDSスカイボックス使用時)は、この設定に関わらずキューブマップを使う
        // (DeferredLighting.hlslへ渡すSkyParams.yはActiveSkyTexture()の結果とのANDで決める)
        bool AnalyticBackground = Defaults::SkyAnalyticBackground;

        // 昼夜サイクル: ImGuiで操作する時刻(0〜24時)。太陽の向き・色・環境光・空の明るさに反映される
        float TimeOfDay = Defaults::TimeOfDay;
        bool TimeAutoAdvance = Defaults::TimeAutoAdvance;
        float TimeAdvanceSpeed = Defaults::TimeAdvanceSpeed; // 自動進行時、1秒あたりに進む時間(時)

        // 太陽が昇ってくる方位角(度)。X軸を0度、Z軸(+方向)を90度とした水平面上の角度で、
        // ImGuiで調整する(ComputeSunLightingが太陽の日の出側水平方向として使用する)
        float SunAzimuthDegrees = Defaults::SunAzimuthDegrees;

        // 大気の濁り具合(Preetham xyYモデルのタービディティ)。値が大きいほど地平線が白く
        // 霞み、天頂の青が薄くなる。定義域はおおむね1.7〜10(EngineDefaults.h::SkyTurbidity参照)。
        // 変更すると空の焼き直しが必要(Render()のturbidityMoved判定参照)
        float Turbidity = Defaults::SkyTurbidity;
        // 空の彩度(アート指定)。既定1.0=Preethamそのまま。詳細はEngineDefaults::SkySaturation。
        // .ksceneの[Scene]SkySaturationで初期化され、以降はUIで上書きできる
        // (m_ReflectionModeがScene.SSREnabledから初期化されるのと同じ設計)
        float Saturation = Defaults::SkySaturation;

        // 月の位置。**時刻には連動せず、ここで指定した固定位置に居続ける**。
        // 実際の月は太陽とは独立した周期(朔望月)で動くので、反太陽方向に固定するのは
        // 「常に満月かつ常に真夜中に南中する」という二重の簡略化になってしまう。
        // 任意の月齢・任意の時刻の見え方を作れるよう、位置は手動指定にしている。
        // 方位角の規約は太陽と同じ(X軸が0度、Z軸(+方向)が90度)。
        // 仰角が0度以下なら月は地平線下にあり、月光は出ない。
        // シーンを切り替えても引き継がれる(.ksceneのキーは持たない)
        float MoonAzimuthDegrees = Defaults::MoonAzimuthDegrees;
        float MoonElevationDegrees = Defaults::MoonElevationDegrees;
    };
}
