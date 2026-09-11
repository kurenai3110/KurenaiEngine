#pragma once

#include <cstdint>
#include <memory>

#include "RHI/IRHIDevice.h"

// IBL(Image Based Lighting)の畳み込み結果と、その生成に使う定数バッファ。
//
// 【なぜ1つにまとめるか】BRDF積分LUTは7つのパス群が、イラディアンスと
// プリフィルタ済み鏡面は6つのパス群が読む。エンジンのメンバのままだと、
// そのために friend が要る。
//
// 【生成の位置を動かさないこと】DX12はディスクリプタ枠を生成順に割り当てる。
// 元の並びの途中に他のテクスチャの生成が挟まるので、生成を1本の関数にまとめず
// 分けてある。呼ぶ側は元の行位置のまま呼ぶこと(Rendering/RenderTargets.h と同じ理由)。
namespace Kurenai::Rendering
{
    struct IBLResources
    {
        std::unique_ptr<RHI::IRHITexture> IrradianceTexture;
        std::unique_ptr<RHI::IRHITexture> PrefilteredEnvTexture;
        // BRDF積分LUT。float4(A, B, Eavg, 0)。第3成分Eavgはスペキュラのエネルギー補正のうち
        // Kulla-Conty(加算ローブ)方式だけが使う半球平均で、行(ラフネス)内では同じ値が入る
        std::unique_ptr<RHI::IRHITexture> BRDFLUTTexture;
        // プリフィルタ済み鏡面のミップごとの畳み込みで使うラフネス値を渡す専用の定数バッファ
        std::unique_ptr<RHI::IRHIBuffer> PrefilterConstantBuffer;
        // ミップごとの畳み込みを行うコンピュートのPSO。
        // 【なぜここが持つか】グローバルIBL(EnvironmentPasses)と反射プローブ
        // (ReflectionProbePasses)がまったく同じシェーダーで畳み込む。上の定数バッファと対で使う
        std::unique_ptr<RHI::IRHIPipelineState> PrefilterPipelineState;

        // 畳み込み結果の2枚。BRDF積分LUTの前に作る
        void CreateEnvironmentMaps(
            RHI::IRHIDevice& device, uint32_t irradianceSize, uint32_t prefilterBaseSize,
            uint32_t prefilterMipLevels);
        // BRDF積分LUT。**中間バッファ(EnvironmentPassesのm_BRDFLUTScratchTexture)の生成を挟んでから呼ぶこと**
        void CreateBRDFLUT(RHI::IRHIDevice& device, uint32_t size);
        void CreatePrefilterConstantBuffer(RHI::IRHIDevice& device, uint32_t sizeInBytes);
    };
}
