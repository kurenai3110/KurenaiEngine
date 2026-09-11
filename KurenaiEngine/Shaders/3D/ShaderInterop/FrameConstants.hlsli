// 描画パス全体が register(b0) で共有する定数バッファ。**このリポジトリで唯一の宣言。**
//
// 【各フィールドが何を意味するかはここに書かない】唯一の出所は
// KurenaiEngine/Source/Engine/ShaderInterop/FrameConstants.h で、
// x/y/z/w に何を詰めているかも含めてすべてあちらのコメントにある。
// 同じ説明を2か所へ書くと、片方だけ直された時点でどちらが正しいか分からなくなる。
//
// 【どのシェーダーも全フィールドを宣言すること】先頭からの前方一致で再宣言すると、
// C++側の途中へフィールドを挿したときに一斉にオフセットずれを起こし、
// **コンパイルは通って絵だけが静かに壊れる**(経緯は docs/ImplementationHistory.md 81章)。
// 使わないフィールドを宣言しても、生成されるコードは変わらない。
//
// 【C++との一致を守っているもの】FrameConstants.h の offsetof の static_assert。
// C++側で並べ替え・挿入・型変更が起きるとビルドが落ちる。
// このファイルを直したら、あちらへ同じ変更を入れること。

#ifndef KURENAI_SHADERINTEROP_FRAMECONSTANTS_HLSLI
#define KURENAI_SHADERINTEROP_FRAMECONSTANTS_HLSLI

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 CascadeViewProj[4];
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
    float4x4 View;
    float4x4 Proj;
    float4 AmbientColor;
    float4 CascadeSplits;
    float4 ShadowParams;
    float4 ActiveLightCount;
    float4 IBLParams;
    float4 ProbeParams;
    float4 ProbeParams2;
    float4x4 PrevViewProj;
    float4 TAAParams;
    float4 DDGIParams0;
    float4 DDGIParams1;
    float4 DDGIParams2;
    float4 DDGIParams3;
    float4 DDGIParams4;
    float4 DDGILODOrigin[4];
    float4 DDGILODBase[4];
    float4 OcclusionParams;
    float4 TimeParams;
    float4 SkySunDirection;
    float4 SkyParams;
    float4 CloudParams0;
    float4 CloudParams1;
    float4 CloudParams2;
    float4 CloudParams3;
    float4 PlanarReflectionPlane;
    float4 FogParams0;
    float4 FogParams1;
    float4 WaterBodyColor;
    float4 StarsParams;
    float4 CloudQualityParams;
    float4 OcclusionCullParams;
    float4 HiZScreenParams;
    float4 MeshletCullStatsParams;
};

#endif // KURENAI_SHADERINTEROP_FRAMECONSTANTS_HLSLI
