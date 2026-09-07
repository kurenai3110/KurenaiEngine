#pragma once

#include <memory>
#include <vector>

#include "Assets/Scene.h"
#include "RHI/IRHIDevice.h"

// 間接光(DDGIと反射プローブ)のリソース一式。
//
// 【なぜ1つにまとめるか】DDGIのアトラスは5つ、プローブの配列は4つのパス群が
// 読む。エンジンのメンバのままだと、そのために friend が要る。
//
// 【生成の位置を動かさないこと】DX12はディスクリプタ枠を生成順に割り当てる。
// このヘッダは所有権をまとめるだけで、生成は元の場所(GI/DDGISystem.cpp・
// KurenaiEngine3D.cpp の反射プローブの節・CreateRenderTargets)のままにしてある。

namespace Kurenai::Rendering
{
    struct GIResources
    {
        // オクタヘドラル2Dアトラス。RGBがイラディアンス、距離側はR=平均距離・G=平均二乗距離。
        // どちらもR32系で確保する。更新CSがヒステリシスのために「前の値を読んでから書く」ため、
        // 型付きUAV読み出しがR32系しか保証されていないという制約に従う必要がある
        // (AutoExposure.hlslの同じ判断を参照)
        std::unique_ptr<RHI::IRHITexture> DDGIIrradianceAtlas;
        std::unique_ptr<RHI::IRHITexture> DDGIDistanceAtlas;

        std::unique_ptr<RHI::IRHITexture> DDGIResolveTexture;
        // 上のパスが2枚目のレンダーターゲットへ書く低解像度の深度(41.24節)。
        // 合成側のバイラテラルアップサンプルがGatherRed 1回で4テクセルぶんを取るために使う
        std::unique_ptr<RHI::IRHITexture> DDGIResolveDepthTexture;

        // シーンから読み込んだボリューム(先頭の1つだけを使う)。HasGIVolumeがfalseの間は
        // アトラスは1プローブぶんのダミーとして確保され、シェーダー側もDDGIParams0.w=0で無効になる
        Assets::GIVolume GIVolume;
        bool HasGIVolume = false;

        // 畳み込み結果(プローブごと)。DeferredLighting.hlslがTextureCubeArrayとして読む。
        // 反射プローブは鏡面専任なので拡散イラディアンス側の配列は持たない(拡散はDDGIへ一本化)
        std::unique_ptr<RHI::IRHITexture> ProbePrefilteredArray;
        // 距離キューブ(プローブごと、19.12節)。プローブ位置から各方向の被写体までのワールド距離。
        // 放射輝度と違い畳み込まないため、キャプチャからキューブ配列へ直接書き込む
        // (スクラッチのキューブマップを経由しない)。用途は2つ:
        //   1. 視差補正を「箱との交差」から「実際に記録された形状との交差」へ精密化する
        //   2. プローブから見えない位置(壁の向こう)のピクセルで重みを落とし、光漏れを抑える
        std::unique_ptr<RHI::IRHITexture> ProbeDistanceArray;
        // プローブの影響範囲(位置・半径)をシェーダーへ渡すStructuredBuffer(t13)
        std::unique_ptr<RHI::IRHIBuffer> ProbeBuffer;

        // キューブ面のキャプチャに使うPSOと定数バッファ、および6面をキューブへ写す
        // コンピュートのPSO。
        // 【なぜここが持つか】反射プローブとDDGIがまったく同じ経路でキャプチャする
        // (ProbeCapture.hlslを共有する)。どちらかの群に持たせると、もう片方が
        // その群を経由して取りに行くことになる
        std::unique_ptr<RHI::IRHIPipelineState> ProbeCapturePipelineState;
        // キャプチャの面ごとに値を更新して使い回すFrameConstants(共有のものとは別。
        // ViewProj/CameraPositionだけをプローブのものへ差し替える。詳細はProbeCapture.hlsl冒頭)
        std::unique_ptr<RHI::IRHIBuffer> ProbeCaptureConstantBuffer;
        std::unique_ptr<RHI::IRHIPipelineState> ProbeCubeCopyPipelineState;

        // シーンが持つ反射プローブの一覧。ApplyLoadedSceneがm_Scene.ReflectionProbesから
        // コピーし、以降ImGuiが編集する。Renderスレッド専有のためロックは不要。
        // 焼くのはReflectionProbePassesだけだが、PresentPassのデバッグ表示が
        // 表示対象の番号を範囲内へ丸めるために個数を読む
        std::vector<Assets::ReflectionProbe> ReflectionProbes;
    };
}
