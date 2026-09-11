#pragma once

#include <cstdint>
#include <memory>

#include "RHI/IRHIDevice.h"

// ドローンショー(発光点の描画)が使うGPUリソース一式。
//
// 【なぜ1つにまとめるか】機体は本描画(PostProcessPasses)と平面反射
// (ReflectionPasses)の2群から、まったく同じPSO・同じ定数バッファ・同じ機体データで
// 描かれる。エンジンのメンバのままだと、そのために friend が要る。
//
// 【機体の状態そのものはここに無い】1フレームぶんの機体データ(GPUDroneの配列)と
// 再生器(DroneShow)はエンジンが持ったままで、ここにあるのはGPUへ渡す器だけ。
// パスが必要とする「何機描くか」「どれだけ明るいか」は毎フレーム変わる値なので
// RenderFrameContext が運ぶ。
//
// 【生成の位置を動かさないこと】DX12はディスクリプタ枠を生成順に割り当てる。
// このヘッダは所有権をまとめるだけで、生成は元の場所(KurenaiEngine3D::CreateSceneResources
// のドローンショーの節)のままにしてある。

namespace Kurenai::Rendering
{
    // ドローンショーの機体数の上限。下の Buffer をこの容量で固定確保する
    // (32バイト×4096 = 128KB。DEFAULTヒープ本体とステージングリングを足しても
    //  1.3MB程度で、機体数を増減しても作り直さずに済む)
    inline constexpr uint32_t kMaxDrones = 4096;

    struct DroneShowResources
    {
        // 頂点バッファを持たず、Draw(6 * 機体数, 0)とSV_VertexIDでクアッドを展開する
        // (理由はShaders/3D/DroneShow.hlsl冒頭)。
        // 【PSOは1本でよい】平面反射(鏡映カメラ)でもこれをそのまま使う。ビルボードの四隅を
        // ビュー空間で足しており鏡映行列を通らないため、巻きが反転しない
        std::unique_ptr<RHI::IRHIPipelineState> PipelineState;
        std::unique_ptr<RHI::IRHIBuffer> ConstantBuffer;
        // 機体データ(StructuredReadOnly)。kMaxDrones分を固定で確保し、実際に描くのは
        // RenderFrameContext::DroneCount 機ぶんだけ。
        // 頂点シェーダーが直接読む(SetVertexShaderResourceBuffer) ―― 通常の
        // SetShaderResourceBufferが使うSRVテーブルはピクセルシェーダーからしか見えないため
        std::unique_ptr<RHI::IRHIBuffer> Buffer;
    };
}
