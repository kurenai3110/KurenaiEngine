// KurenaiEngine2D(2D公開API)が内部で使うスプライト描画シェーダ。
// 定数バッファはb0(フレーム共通)とb1(スプライト単位)、テクスチャ/サンプラーはt0/s0のみを使用する。

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
};

cbuffer ObjectConstants : register(b1)
{
    float4x4 World;
    float4 Color;
};

Texture2D SpriteTexture : register(t0);
SamplerState DefaultSampler : register(s0);

struct VSInput
{
    float3 Position : POSITION;
    float2 UV : TEXCOORD0;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 worldPos = mul(float4(input.Position, 1.0f), World);
    output.Position = mul(worldPos, ViewProj);
    output.UV = input.UV;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 texColor = SpriteTexture.Sample(DefaultSampler, input.UV);
    return texColor * Color;
}
