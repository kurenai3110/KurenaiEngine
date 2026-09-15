// MegaLights の確率的サンプリング経路が共有する定数バッファ。**唯一の宣言。**
//
// 【各フィールドの意味はここに書かない】唯一の出所は
// KurenaiEngine/Source/Engine/ShaderInterop/MegaLightsStochasticConstants.h。
//
// **どのシェーダーも全フィールドを宣言すること。** 先頭からの前方一致で再宣言すると、
// C++側の途中へフィールドを挿したときに後ろを宣言している側が静かにずれる
// (実際にそうなっていた頃の経緯は docs/ImplementationHistory.md 83章)。
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
    // x=1画素あたりの標本数、y=ブースト標本数B、z=対象モード、
    // w=述語フラグ(bit0=デノイズ履歴有効、bit1=幾何ガイド有効、bit2=4タップ判定)
    uint4 Params5;
    uint4 Params6;
};

#endif // KURENAI_SHADERINTEROP_MEGALIGHTSSTOCHASTICCONSTANTS_HLSLI
