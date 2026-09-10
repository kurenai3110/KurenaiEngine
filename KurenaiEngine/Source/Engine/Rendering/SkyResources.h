#pragma once

#include <cstdint>
#include <memory>

#include "RHI/IRHIDevice.h"

// 空・大気・雲のリソース一式。
//
// 【なぜ1つにまとめるか】SkyViewLUTは5つ、空パラメータと雲のノイズは3〜4つの
// パス群が読む。エンジンのメンバのままだと、そのために friend が要る。
//
// 【生成の位置を動かさないこと】DX12はディスクリプタ枠を生成順に割り当てる。
// 元の並びの途中に他のリソースの生成が挟まるので、生成を1本の関数にまとめず
// 分けてある。呼ぶ側は元の行位置のまま呼ぶこと(Rendering/RenderTargets.h と同じ理由)。
//
// 【生成に失敗したときの記録は呼ぶ側に残す】どのリソースが欠けたら何が壊れるかは
// エンジンの文脈なので、判定とログは移していない。

namespace Kurenai::Rendering
{
    struct SkyResources
    {
        // SkyIntegrate.hlslが書き、SkyGenerate.hlsl/DeferredLighting.hlsl/SSR.hlslが読む
        // 要素数1のStructuredRWバッファ(Sky.hlsliのGPUSkyParametersと一致させること)。
        // 【なぜ毎フレーム作り直さないのか】ベイク時の値をそのまま使うことで、背景とキューブマップ
        // (IBL・反射)が常に同一の空パラメータを見る。毎フレーム作り直すと、太陽の角度閾値で
        // ベイクを間引いている間だけ背景とIBLの空がずれてしまう。加えて積分はθ64×φ256=16,384
        // サンプルなので、背景評価のためだけに毎フレーム走らせるのは無駄が大きい
        std::unique_ptr<RHI::IRHIBuffer> ParametersBuffer;

        // 手続き空(SkyGenerate.hlsl)を焼き込むキューブマップ。太陽が動くたびに焼き直す。
        // 【なぜここが持つか】焼くのはEnvironmentPassesだけだが、
        // KurenaiEngine3D が「今フレームどちらの空を使うか」(DDSのスカイボックスか手続き空か)を
        // これとの同一性で判定するため、エンジン側からも見える必要がある。
        // プリフィルタの入力にしかならないので解像度はオフラインDDS(512)より小さい256で足りる
        std::unique_ptr<RHI::IRHITexture> ProceduralSkyTexture;

        // --- ボリュメトリック雲の3Dノイズ ---
        //
        // 雲の形状ノイズ。カメラにも太陽にも空の状態にも一切依存しない純粋な手続き生成なので、
        // BRDF積分LUTとまったく同じ理由で起動後に一度だけ焼き、二度と焼き直さない
        // (EnvironmentPassesのm_CloudNoiseBaked)。生成の中身はShaders/3D/CloudNoiseGenerate.hlsl。
        //
        // 【なぜ2枚に分けるか】Shapeは雲の大まかな塊、Detailはその縁を削る高周波成分で、
        // 必要な解像度が2桁違う。1枚にまとめると細かい側に合わせた巨大なテクスチャが要る
        //
        // 【3枚目: ウェザーマップ(H3)】雲がどこに立つかを決める2Dの場。上の2枚と同じく
        // 純粋な手続き生成なので同じパスで一度だけ焼く。**これはレイマーチの高速化が目的**で、
        // 実測ではマーチの1歩あたりコストの91%がこの2Dのfbmだった(根拠と解像度の実測は
        // Shaders/3D/Sky.hlsli のウェザーマップの節)
        std::unique_ptr<RHI::IRHITexture> CloudShapeNoiseTexture;
        std::unique_ptr<RHI::IRHITexture> CloudDetailNoiseTexture;
        std::unique_ptr<RHI::IRHITexture> CloudWeatherNoiseTexture;

        // --- 大気散乱のLUT(Hillaire 2020) ---
        //
        // TransmittanceとMultiScatteringは大気パラメータ(AtmosphereLUT.hlsl冒頭の定数と、
        // 実行時に動かせる濁り)だけで決まり、カメラにも太陽にも時刻にも依存しない。
        // そのためBRDF積分LUT・雲の3Dノイズとほぼ同じ「一度だけ焼く」作法に乗せ、
        // 濁りが変わったときだけ焼き直す(EnvironmentPassesのm_AtmosphereLUTBakedTurbidity)。
        //
        // SkyViewは空そのもので太陽の位置に依存するため、太陽か濁りが動いたときに焼き直す
        // (同 m_SkyViewBakedSunPosition)。
        // 【テクセル数が小さいことを「負荷が無い」の根拠にしないこと】1テクセルあたり
        // 視線32段+天頂32段の計64段のレイマーチで、各段が2つのLUTのサンプルを伴う。
        // 毎フレーム焼いていた頃の実測は docs/ImplementationHistory.md 41.17。
        // **このLUTを読むパス(SkyIntegrate/SkyGenerate/Lighting/SSR/AerialPerspective/
        // PlanarReflection)より前に実行される必要がある**が、順序はレンダーグラフが
        // Reads/Writesの依存から自動で決めるので、パスの登録順に依存しない
        std::unique_ptr<RHI::IRHITexture> TransmittanceLUT;
        std::unique_ptr<RHI::IRHITexture> MultiScatteringLUT;
        std::unique_ptr<RHI::IRHITexture> SkyViewLUT;

        void CreateCloudNoise(
            RHI::IRHIDevice& device, uint32_t shapeSize, uint32_t detailSize, uint32_t weatherSize);
        void CreateAtmosphereLUTs(
            RHI::IRHIDevice& device, uint32_t transmittanceWidth, uint32_t transmittanceHeight,
            uint32_t multiScatteringSize, uint32_t skyViewWidth, uint32_t skyViewHeight);
        void CreateParametersBuffer(RHI::IRHIDevice& device, uint32_t sizeInBytes);
    };
}
