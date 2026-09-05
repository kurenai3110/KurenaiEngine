#pragma once

#include <cstdint>

#include "KurenaiTypes.h"

namespace Kurenai::Assets
{
    // 段階2のメッシュライトが積分する三角形1枚。**ワールド空間**。
    // HLSL側の GPUEmissiveTriangle(Shaders/3D/MeshLighting.hlsli)と64バイトで一致させること。
    //
    // 【辺で持つ理由】p = P0 + b1*E1 + b2*E2 で重心座標からそのまま点が出て、
    // 幾何法線も cross(E1,E2) で出る。面積は毎回 sqrt するのが惜しいので焼いておく。
    //
    // 【ワールド空間で持つ理由】容量ではなく**評価回数**の問題。三角形の位置は
    // 候補プールの重要度・Initial の目標関数を M 回・Spatial の判定と Z を最大9回ずつ・
    // Temporal・Shade・参照実装 から読まれる。そこへ毎回
    // 「インスタンス表 → bindless頂点バッファ → インデックス → 3頂点 → 行列積」の
    // 依存読み出しを挟むのは割に合わないうえ、bindless非対応環境という2本目の分岐が生える。
    // シーンは読み込み後に変形しない(TLASすら更新していない)ので、1回焼けば済む。
    //
    // 【RTVertexAttribute では代用できない】あちらは位置を持たない(16バイトに抑えるため
    // 法線とUVだけ)。この専用バッファは避けられない
    struct GPUEmissiveTriangle
    {
        // xyz = 頂点0(ワールド), w = 影響半径 R[m]
        float P0AndRadius[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        // xyz = P1 - P0, w = 面積 A[m^2]
        float E1AndArea[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        // xyz = P2 - P0, w = asfloat(Flags)
        float E2AndFlags[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        // rgb = 放射輝度 L_e(**露出前**), a = Luminance(L_e) * A(光束の輝度換算)
        //
        // 【GPULight とは露出の扱いが逆】GPULight.ColorRange は CPU 側で露出済み
        // (PunctualLighting.hlsli)。こちらのテーブルは不変なので同じことができず、
        // 露出前で持ってフレーム定数から1回掛ける。**GPULight の慣習をそのまま写すと
        // 露出が二重/欠落し、しかも自動露出が部分的に補償して「それらしく」見える**
        float RadianceAndFlux[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    };
    static_assert(sizeof(GPUEmissiveTriangle) == 64, "HLSL側のGPUEmissiveTriangleと一致させるため64バイト固定");

    // GPUEmissiveTriangle::E2AndFlags.w のビット定義
    // (Shaders/3D/MeshLighting.hlsli の kMeshLightFlag* と一致させること)
    inline constexpr uint32_t kMeshLightFlagDoubleSided = 1u << 0;
}
