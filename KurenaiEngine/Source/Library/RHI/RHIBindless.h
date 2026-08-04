#pragma once

#include <cstdint>

// bindless(HLSLのResourceDescriptorHeap、シェーダーモデル6.6)関連の共有定義。
//
// 【bindlessとは何を変えるのか】従来このエンジンは、シェーダーが読むリソースを
// 「t0〜t20の固定スロットへ、描画のたびにディスクリプタテーブルをコピーして」渡していた
// (DX12Device::AllocateSrvTableBlock)。この方式にはHLSL側の制約が付いて回る:
// リソースそのものを動的な添字で選べないため、
//   ・カスケードシャドウマップは4枚を個別スロットではなくTexture2DArrayにまとめる
//   ・反射プローブは複数キューブをTextureCubeArrayにまとめる
//   ・レイトレーシングはシーン全体の頂点/インデックスを1本の構造化バッファへ連結する
// といった「1つのリソースに詰め直す」対処が必要だった
// (それぞれIRHIDevice::CreateDepthTextureArray / CreateMippedUAVTextureCubeArray /
//  Assets::RaytracingScene冒頭のコメントに理由が書かれている)。
//
// bindlessはシェーダ可視ヒープ全体を1つの巨大な配列として直接添字できるようにするもので、
// 「実行時に決まる番号でリソースを選ぶ」が素直に書ける。このエンジンで最初にその恩恵を
// 受けるのは次の2つ:
//   1. レイトレーシングのヒット面がマテリアルのテクスチャをサンプルできるようになる
//      (従来は定数色のみ。Assets::RaytracingMaterialの旧「Phase 1の制約」)
//   2. メッシュシェーダーがメッシュレットのジオメトリバッファを引ける
//      (メッシュシェーダーには入力アセンブラが無く、頂点は自分でバッファから読むしかない)
//
// 【常に使えるとは限らない】必要なのはDX12かつシェーダーモデル6.6で、加えて
// シェーダーをコンパイルするdxcompiler.dll自体が6.6を知っている必要がある。
// DX11には存在しない。そのため上位層は必ずIRHIDevice::SupportsBindless()を確認し、
// falseなら従来経路へフォールバックすること(レイトレーシングの扱い方と同じ)。

namespace Kurenai::RHI
{
    // bindlessディスクリプタが割り当てられていないことを表す番号。
    // シェーダー側(Shaders/3D/Bindless.hlsli の kInvalidBindlessIndex)と同じ値にすること。
    //
    // 【0ではなく0xFFFFFFFFにする】0は「ヒープ先頭の正当なディスクリプタ」でもあるため、
    // 未登録と区別できない。シェーダー側はこの値との比較で「テクスチャ無し」を判定し、
    // 従来のプレースホルダー(白1x1など)と同じ既定値へ落とす
    inline constexpr uint32_t kInvalidBindlessIndex = 0xFFFFFFFFu;
}
