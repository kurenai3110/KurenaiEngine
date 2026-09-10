// FrameConstants を読む小さな共有ヘルパ。**それぞれこのリポジトリで唯一の定義。**
//
// 式が短いぶん、複製の一部だけを直しても目で気付きにくい
// (散っていた頃の本数と食い違いは docs/ImplementationHistory.md 82章)。

#ifndef KURENAI_SHADERINTEROP_COMMON_HLSLI
#define KURENAI_SHADERINTEROP_COMMON_HLSLI

// 【パスは .hlsl から見た形で書く】このヘッダーと同じフォルダにあっても
// "FrameConstants.hlsli" とは書けない。fxc の D3D_COMPILE_STANDARD_FILE_INCLUDE は
// 入れ子のインクルードを**一番外側の .hlsl のフォルダ**基準で解決するため、
// 相対パスにすると DX11 側だけが error X1507 で落ちる(dxc は通ってしまう)
#include "ShaderInterop/FrameConstants.hlsli"

// 画面UVと深度バッファの生値からワールド座標を復元する。
//
// 【depth は深度バッファの生値をそのまま渡す】この関数は Reverse-Z かどうかを知らない。
// InvViewProj が投影行列の逆になっているので、近平面が z=1.0 の Reverse-Z でも
// 式は変わらない(近平面と遠平面の意味が入れ替わるだけ)。
//
// 【uv は左上原点】NDC の y は上が +1 なので符号を反転させている。
// ジオメトリの無い背景画素は深度 0(= 遠平面)で、復元される位置は遠平面上になる
float3 ReconstructWorldPos(float2 uv, float depth)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 worldPos = mul(float4(ndc, depth, 1.0f), InvViewProj);
    return worldPos.xyz / worldPos.w;
}

#endif // KURENAI_SHADERINTEROP_COMMON_HLSLI
