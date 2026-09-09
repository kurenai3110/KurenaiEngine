#pragma once

#include <cstddef>

#include <DirectXMath.h>

// 反射プローブ1つぶんの、GPUへ送るレコード。
//
// 【共有ヘッダーにする理由】GPULight.h と同じ事情で、詰めるのはフレームの値を
// 組み立てる側、読むのは DeferredLighting.hlsl。型が KurenaiEngine3D.cpp の
// 無名名前空間にあると、組み立てを別の翻訳単位へ切り出せない。
//
// **「レイアウト」はここ、「組み立て」は使う側**という役割分担は GPULight.h と同じ。
namespace Kurenai
{
    // DeferredLighting.hlsl 側の struct GPUReflectionProbe と
    // 並び・ストライド(48バイト)を一致させる必要がある
    struct alignas(16) GPUReflectionProbe
    {
        DirectX::XMFLOAT4 PositionRadius; // xyz=ワールド座標(Box形状では箱の中心), w=Sphere形状の影響半径
        DirectX::XMFLOAT4 BoxExtents;     // xyz=Box形状の各軸の半径(ハーフエクステント), w=ブレンド距離
        DirectX::XMFLOAT4 ShapeParams;    // x=形状(0=Sphere,1=Box), y=sin(Yaw), z=cos(Yaw), w=未使用
    };

    // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
    // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
    // HLSL側を直さないかぎり黙って別の値を読むことになる。
    // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
    //
    // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
    // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
    static_assert(offsetof(GPUReflectionProbe, PositionRadius) == 0, "PositionRadius のレイアウトが変わっている");
    static_assert(offsetof(GPUReflectionProbe, BoxExtents) == 16, "BoxExtents のレイアウトが変わっている");
    static_assert(offsetof(GPUReflectionProbe, ShapeParams) == 32, "ShapeParams のレイアウトが変わっている");
    static_assert(sizeof(GPUReflectionProbe) == 48, "GPUReflectionProbe の総サイズが変わっている");
}
