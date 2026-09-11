#pragma once

#include <cstdint>

#include "Assets/ModelPackage.h"

// スレッドグループのサイズ。**C++側ではここが唯一の定義。**
//
// 以前は KurenaiEngine3D.cpp の中に「GBufferMeshlet.hlslと一致させること」という
// コメント付きの constexpr が散らばっており、増幅シェーダーの32は2箇所にあった。
//
// 【HLSL側は Shaders/3D/ShaderInterop/GroupSizes.hlsli が持ち、一致は機械で確かめる】
// KurenaiShaderPacker がこのヘッダーを取り込み、ここの値を
// KURENAI_EXPECT_* として -D で HLSL へ渡す。GroupSizes.hlsli 側は受け取った値と
// 自分の #define を突き合わせ、食い違っていれば #error でビルドを落とす。
//
// 【なぜ HLSL 側にも実数値を残すのか】-D が来ない経路(shader-check スキルが
// fxc/dxc を直接叩く場合)でもコンパイルできる必要があるため。
// あちらの #if は KURENAI_EXPECT_* が未定義なら丸ごと飛ぶ
namespace Kurenai::ShaderInterop
{
    // 増幅シェーダー1グループが判定するメッシュレット数。
    // C++側は「モデル全体のメッシュレット数 ÷ これ」を起動グループ数にする。
    // GroupSizes.hlsli の KURENAI_AMPLIFICATION_GROUP_SIZE と一致させること
    constexpr uint32_t kAmplificationGroupSize = 32;

    // メッシュシェーダーの1グループのスレッド数。1スレッドが頂点1つと三角形1つを担当する。
    //
    // 【C++はこの値でディスパッチしない】numthreads の値であって、起動グループ数の
    // 割り算には使わない。それでもここに置くのは、下の static_assert で
    // 「メッシュレットの上限以上あること」を機械で守るため。
    // GroupSizes.hlsli の KURENAI_MESH_GROUP_SIZE と一致させること
    constexpr uint32_t kMeshGroupSize = 128;

    // 1スレッドが頂点1つと三角形1つを担当するので、メッシュレットの上限より小さいと
    // 頂点か三角形が出力されないまま欠ける。**絵には「一部の面が消える」形でしか出ない**
    static_assert(kMeshGroupSize >= Assets::kMeshletMaxVertices, "メッシュグループが頂点の上限に足りない");
    static_assert(kMeshGroupSize >= Assets::kMeshletMaxTriangles, "メッシュグループが三角形の上限に足りない");

    // ソフトウェアラスタライザ(SoftwareRaster.hlsl)の1グループのスレッド数。
    // C++側が「三角形数 ÷ これ」でディスパッチする(GeometryPasses.cpp)。
    // SoftwareRaster.hlsl の KURENAI_SWRASTER_GROUP_SIZE と一致させること
    constexpr uint32_t kSWRasterGroupSize = 64;

    // モデル単位のカリング(ModelCull.hlsl)の1グループのスレッド数。
    // GroupSizes.hlsli の KURENAI_MODEL_CULL_GROUP_SIZE と一致させること
    constexpr uint32_t kModelCullGroupSize = 64;

    // ソフトウェアラスタライザの解決パスのタイル1辺(2次元グループ)。
    // GroupSizes.hlsli の KURENAI_SWRASTER_RESOLVE_GROUP_SIZE と一致させること
    constexpr uint32_t kSWRasterResolveGroupSize = 8;

    // DispatchMeshIndirect の引数1件ぶんのバイト数。ModelCull.hlsl がこの刻みで書き込む。
    //
    // 【本体は RHI::IRHICommandList::kDispatchMeshIndirectArgStride】あちらは RHI の
    // インターフェースの一部で、このヘッダーは RHI に依存しない(パッカーが取り込むため)。
    // 二重に持つことになるが、KurenaiEngine3D.cpp の static_assert が両者の一致を止める。
    // GroupSizes.hlsli の KURENAI_INDIRECT_ARG_STRIDE とはパッカーの -D で突き合わせる
    constexpr uint32_t kDispatchMeshIndirectArgStride = 24;
}
