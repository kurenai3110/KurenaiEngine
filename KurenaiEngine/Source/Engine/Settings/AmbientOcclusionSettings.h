#pragma once

#include <cstdint>

#include "../EngineDefaults.h"

namespace Kurenai
{
    // AO/GI手法の選択。SSAOは遮蔽率のみ、SSIL(Visibility Bitmask)は遮蔽率に加えて
    // 近傍サーフェスからの間接拡散光(バウンス光)も計算する。Raytracedは同じものを
    // 深度バッファではなく高速化構造への交差判定で求める(画面外の遮蔽物も効く)。
    // いずれも出力フォーマットは共通(rgb=間接拡散光, a=遮蔽率)で、
    // ライティングパスは選択中のテクスチャを1枚読むだけでよい
    enum class AOTechnique
    {
        SSAO,
        SSILVisibilityBitmask,
        Raytraced,
    };

    // スペキュラ遮蔽の方式。FrameConstants.OcclusionParams.yへ数値として渡し、
    // SpecularEnergy.hlsliのComposeSpecularOcclusionが切り替える。
    // 値はComposeSpecularOcclusionのsoModeと一致させること
    enum class SpecularOcclusionMode
    {
        Legacy = 0,  // Frostbite近似(方向を見ない従来近似)
        Cone = 1,    // 球冠交差(SpecularOcclusionBand。d >= av+as で厳密に0になる)
        SG = 2,      // 球面ガウス(SpecularOcclusionSG、34.11節。常に正なので凹部が純黒へ潰れない)
    };

    struct AmbientOcclusionSettings
    {
        bool Enabled = Defaults::AOEnabled;
        AOTechnique Technique = AOTechnique::SSAO;
        float SSAORadius = Defaults::SSAORadius;
        float SSAOPower = Defaults::SSAOPower;
        // 1画素あたりのカーネルサンプル数。SSAOのコストはほぼこれに比例する
        // (実測でAOパスはジオメトリが画面を占めるシーンで4.8〜11.0msあり、雲を分離した後の
        //  最大の残りだった)。定数バッファの配列はkSSAOKernelSizeMax(16)で固定のまま、
        // 実際に回す段数だけをSSAOConstants.Params.wでシェーダへ渡す。
        //
        // 【減らすときはカーネルを作り直す】GenerateSSAOKernelはi/kernelSizeで各サンプルの
        // 長さを決めているため、16本用のカーネルの先頭N本を使うと原点付近の短いサンプルばかりが
        // 残り、遠距離の遮蔽を拾わなくなる。必ずこの数で生成し直すこと(EnsureSSAOKernel)
        uint32_t SSAOKernelSize = Defaults::SSAOKernelSize;

        float SSILRadius = Defaults::SSILRadius;
        float SSILThickness = Defaults::SSILThickness;
        float SSILIntensity = Defaults::SSILIntensity;
        float SSILPower = Defaults::SSILPower;
        uint32_t SSILSliceCount = Defaults::SSILSliceCount;
        uint32_t SSILStepCount = Defaults::SSILStepCount;

        int32_t RTAOSampleCount = Defaults::RTAOSampleCount;
        float RTAOMaxDistance = Defaults::RTAOMaxDistance;
        float RTAOPower = Defaults::RTAOPower;
        float RTAOIntensity = Defaults::RTAOIntensity;
        bool RTAOBounceShadowRayEnabled = Defaults::RTAOBounceShadowRayEnabled;

        SpecularOcclusionMode SpecularOcclusion =
            static_cast<SpecularOcclusionMode>(Defaults::SpecularOcclusionMode);

        // bent normalによる遮蔽(34章)。FrameConstants::OcclusionParamsへ載る
        bool BentNormalAOSource = Defaults::BentNormalAOSource;
        bool MultiBounceAOEnabled = Defaults::MultiBounceAOEnabled;

        // マテリアルの遮蔽マップ(glTFのocclusionTexture。22章)を使うか。
        // 上のEnabled(スクリーンスペースAO/GI)とは完全に別系統で、無効にしても遮蔽マップは
        // 効き続けるためこのトグルを別に持つ。無効時はObjectConstants.OcclusionStrengthへ0を渡し、
        // 各パスのlerp(1, occlusionSample, 0) = 1(遮蔽なし)にする方式なのでシェーダー側の変更は不要。
        // 反射プローブはキャプチャ時の値が焼き込まれるため、切り替えても焼き直すまで反映されない
        bool OcclusionMapEnabled = Defaults::OcclusionMapEnabled;
    };
}
