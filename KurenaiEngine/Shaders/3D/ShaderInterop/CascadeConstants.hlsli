// シャドウパスの各カスケード描画専用の定数バッファ。**唯一の宣言。**
// 以前は Shadow.hlsl と ShadowMeshlet.hlsl が別々に宣言していた。
// 各フィールドの意味は Source/Engine/ShaderInterop/CascadeConstants.h にある

#ifndef KURENAI_SHADERINTEROP_CASCADECONSTANTS_HLSLI
#define KURENAI_SHADERINTEROP_CASCADECONSTANTS_HLSLI

cbuffer CascadeConstants : register(b0)
{
    float4x4 ViewProj;
};

#endif // KURENAI_SHADERINTEROP_CASCADECONSTANTS_HLSLI
