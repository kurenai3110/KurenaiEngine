// MegaLights の確率的サンプリング経路が共有する定数バッファ。**唯一の宣言。**
//
// 【各フィールドの意味はここに書かない】唯一の出所は
// KurenaiEngine/Source/Engine/ShaderInterop/MegaLightsStochasticConstants.h。
//
// 以前は Initial / Temporal / Spatial / Shade / Resolve の 5 本が、この宣言を
// 「先頭からの前方一致」として手で再宣言していた。深さは 3〜7 フィールドとばらついており、
// C++側の途中へフィールドを挿すと後ろを宣言している側が静かにずれる。
// いまはどれも全フィールドを宣言するので、この壊れ方は起きない。
// C++との一致は向こうの offsetof の static_assert が守っている

#ifndef KURENAI_SHADERINTEROP_MEGALIGHTSSTOCHASTICCONSTANTS_HLSLI
#define KURENAI_SHADERINTEROP_MEGALIGHTSSTOCHASTICCONSTANTS_HLSLI

cbuffer MegaLightsStochasticConstants : register(b1)
{
    uint4 Params0;
    uint4 Params1;
    uint4 Params2;
    float4 Params3;
    uint4 Params4;
    uint4 Params5;
    uint4 Params6;
};

#endif // KURENAI_SHADERINTEROP_MEGALIGHTSSTOCHASTICCONSTANTS_HLSLI
