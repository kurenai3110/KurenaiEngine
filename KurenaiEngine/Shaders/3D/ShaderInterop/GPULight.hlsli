#ifndef KURENAI_SHADERINTEROP_GPULIGHT_HLSLI
#define KURENAI_SHADERINTEROP_GPULIGHT_HLSLI

// ライト1灯ぶんのデータ。**HLSL側ではここが唯一の宣言。**
//
// 以前は PunctualLighting.hlsli / ProbeShading.hlsli / PlanarReflection.hlsl /
// Transparent.hlsl の4か所が同じ並びを別々に宣言しており、そのうえ
// PunctualLighting.hlsli と ProbeShading.hlsli は「同名の struct を二重に宣言するので
// 同時にインクルードできない」という制約を生んでいた。並びを1本へ寄せると、
// 手で4か所を揃える義務も、同時インクルードの制約も消える。
//
// 【C++側は Source/Engine/Rendering/GPULight.h が持つ】ストライド64バイトの一致は
// あちらの static_assert が見ている。**並びを変えるときは両方を直すこと。**
struct GPULight
{
    float4 PositionType;   // xyz=ワールド座標, w=LightType(0=Directional, 1=Point, 2=Spot)
    // rgb = Color * Intensity[cd] * exposure(EV100)。カンデラ→露出済みの最終放射輝度で、
    // CPU側(MakeGPULight)で計算してあるためシェーダ側はそのまま乗算するだけでよい
    float4 ColorRange;     // rgb=露出済み放射輝度, w=Range
    float4 DirectionAngle; // xyz=向き(正規化済み), w=spotAngleScale
    // x=spotAngleOffset
    // y=影のフラグ(bit0=画面空間シャドウ / bit1=レイトレース影レイ)
    // z=SourceRadius(球光源の半径 / エミッシブ光源プロキシでは面積等価の円板半径)
    // w=指向性κ(エミッシブ光源プロキシのみ。それ以外は0)
    float4 Params;
};

#endif // KURENAI_SHADERINTEROP_GPULIGHT_HLSLI
