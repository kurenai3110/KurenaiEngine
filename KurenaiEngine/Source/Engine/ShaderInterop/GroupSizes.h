#pragma once

#include <cstdint>

// スレッドグループのサイズ。**C++側ではここが唯一の定義。**
//
// 以前は KurenaiEngine3D.cpp の中に「GBufferMeshlet.hlslと一致させること」という
// コメント付きの constexpr が散らばっており、増幅シェーダーの32は2箇所にあった。
//
// 【HLSL側は Shaders/3D/ShaderInterop/GroupSizes.hlsli が持つ】値が一致していることを
// **機械で確かめる仕掛けは無い**(HLSLのマクロをC++から読めないため。FrameConstants の
// offsetof のようには守れない)。片方を直したらもう片方も直すこと。
//
// 間接引数の刻み(24)だけはここに置かない。RHIのインターフェースの一部なので
// RHI::IRHICommandList::kDispatchMeshIndirectArgStride が持つ
namespace Kurenai::ShaderInterop
{
    // 増幅シェーダー1グループが判定するメッシュレット数。
    // C++側は「モデル全体のメッシュレット数 ÷ これ」を起動グループ数にする。
    // GroupSizes.hlsli の KURENAI_AMPLIFICATION_GROUP_SIZE と一致させること
    constexpr uint32_t kAmplificationGroupSize = 32;

    // モデル単位のカリング(ModelCull.hlsl)の1グループのスレッド数。
    // GroupSizes.hlsli の KURENAI_MODEL_CULL_GROUP_SIZE と一致させること
    constexpr uint32_t kModelCullGroupSize = 64;

    // ソフトウェアラスタライザの解決パスのタイル1辺(2次元グループ)。
    // GroupSizes.hlsli の KURENAI_SWRASTER_RESOLVE_GROUP_SIZE と一致させること
    constexpr uint32_t kSWRasterResolveGroupSize = 8;
}
