#pragma once

#include <DirectXMath.h>

#include "../EngineDefaults.h"

namespace Kurenai
{
    struct WaterSettings
    {
        // 水中項。Water.hlslのPSMainがメッシュ自身のBaseColorFactorの代わりにこの色を
        // 出力Albedoに使う(見下ろした水面がFresnel最小でほぼ真っ黒になる問題への対処。
        // 干潟の水の色はシーン側で調整したいパラメータであり、.kmodelを焼き直さずに変えられるようにするため)
        DirectX::XMFLOAT3 BodyColor{
            Defaults::WaterBodyColorR, Defaults::WaterBodyColorG, Defaults::WaterBodyColorB
        };

        // trueにすると波のスクロールが止まる(m_SkySettings.TimeAutoAdvanceの水面版に近いが、
        // 「動かす/止める」の2値なので速度ではなくフラグにしている)
        bool TimeFrozen = Defaults::WaterTimeFrozen;
        // シーン読み込み時にScene::WaterWaveScale等から初期化され、以降はUIで実行時上書きできる
        // (m_ReflectionSettings.ModeがScene.SSREnabledから初期化されるのと同じ設計、ApplyLoadedScene参照)。
        // m_WaterWaveSpeedはm_WaterScrollOffsetの進行速度に使われる。m_WaterWaveScale/
        // m_WaterWaveStrengthはFrameConstants.TimeParams.y/zとしてWater.hlslへ渡り、層のUV
        // スケール(kWaterLayerAUvScale等への倍率)・波の振幅(距離減衰のweightへの倍率)に効く
        float WaveScale = Defaults::WaterWaveScale;
        float WaveSpeed = Defaults::WaterWaveSpeed;
        float WaveStrength = Defaults::WaterWaveStrength;
        // 水面の反射に解析空フォールバックを使うか(SSRの水面分岐)。SSRレイが画面外へ抜けた・
        // 最大距離まで判定がつかなかった水面画素で、プリフィルタ済み鏡面IBL(128pxベースの
        // キューブマップをラフネス由来のミップで引くため広い水面ではにじむ)の代わりに
        // Sky.hlsliのSkyColorを画面解像度で直接評価する(SSR.hlslのPSMain参照)。
        // 効果が出るのはm_ReflectionSettings.Mode==ScreenSpaceのときだけで、かつ手続き空が無効な
        // シーンでは常に無効化される(SSRパスのExecute内、usingProceduralSkyとのAND判定)
        bool AnalyticSkyReflection = Defaults::WaterAnalyticSkyReflection;
    };
}
