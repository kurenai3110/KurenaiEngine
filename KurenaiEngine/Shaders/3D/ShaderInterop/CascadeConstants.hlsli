// シャドウパスの各カスケード描画専用の定数バッファ。**唯一の宣言。**
// 別々に宣言していた頃の経緯は docs/ImplementationHistory.md 83章。
// 各フィールドの意味は Source/Engine/ShaderInterop/CascadeConstants.h にある

#ifndef KURENAI_SHADERINTEROP_CASCADECONSTANTS_HLSLI
#define KURENAI_SHADERINTEROP_CASCADECONSTANTS_HLSLI

cbuffer CascadeConstants : register(b0)
{
    float4x4 ViewProj;
};

#endif // KURENAI_SHADERINTEROP_CASCADECONSTANTS_HLSLI
