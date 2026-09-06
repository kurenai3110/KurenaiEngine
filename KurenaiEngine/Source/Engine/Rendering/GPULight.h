#pragma once

#include <cstddef>
#include <cstdint>

#include <DirectXMath.h>

// GPUへ送るライト1灯ぶんのレコード(段階6)。
//
// 【共有ヘッダーにする理由】ライトの配列を組み立てるのは Render()、読むのは
// 直接光・半透明・プローブのキャプチャと広く散っている。フレームのスナップショットが
// この配列を指すため、型が KurenaiEngine3D.cpp の無名名前空間にあると参照できない。
//
// 【static_assert が守るのはC++側だけ】HLSL の宣言と突き合わせているわけではない。
// **通すために期待値を書き換えないこと。**
namespace Kurenai
{
        // DirectLighting.hlsl側のstruct GPULightと並び・ストライド(64バイト)を一致させる必要がある
        struct alignas(16) GPULight
        {
            DirectX::XMFLOAT4 PositionType;   // xyz=ワールド座標, w=LightType
            DirectX::XMFLOAT4 ColorRange;     // rgb=露出済み放射輝度, w=Range
            DirectX::XMFLOAT4 DirectionAngle; // xyz=向き(正規化済み), w=spotAngleScale
            // x=spotAngleOffset
            // y=影のフラグ(bit0=画面空間シャドウ / bit1=レイトレース影レイ。kLightShadow* を使う)
            // z=SourceRadius(球光源の半径 / エミッシブ光源プロキシでは面積等価の円板半径)
            // w=指向性κ(エミッシブ光源プロキシのみ。それ以外は0)
            DirectX::XMFLOAT4 Params;
        };
        // 【HLSL側の宣言とレイアウトを揃えたまま保つための固定】cbuffer(と構造化バッファ)は
        // 宣言順でオフセットが決まるので、ここで並べ替え・挿入・型変更が起きると、
        // HLSL側を直さないかぎり黙って別の値を読むことになる。
        // **通すために期待値を書き換えないこと**(FrameConstants.h と同じ規約)。
        //
        // 【これが守るのはC++側だけ】HLSLの宣言と突き合わせているわけではない。
        // ここが落ちたら「HLSL側も同じだけ動かせ」という合図として使う
        static_assert(offsetof(GPULight, PositionType) == 0, "PositionType のレイアウトが変わっている");
        static_assert(offsetof(GPULight, ColorRange) == 16, "ColorRange のレイアウトが変わっている");
        static_assert(offsetof(GPULight, DirectionAngle) == 32, "DirectionAngle のレイアウトが変わっている");
        static_assert(offsetof(GPULight, Params) == 48, "Params のレイアウトが変わっている");
        static_assert(sizeof(GPULight) == 64, "GPULightはDirectLighting.hlsl側と64バイトで一致させる必要がある");
}
