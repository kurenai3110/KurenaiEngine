cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4 LightDirection;
    float4 LightColor;
};

Texture2D BaseColorTexture : register(t0);
SamplerState BaseColorSampler : register(s0);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.Position = mul(float4(input.Position, 1.0f), ViewProj);
    output.Normal = input.Normal;
    output.UV = input.UV;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 normal = normalize(input.Normal);
    float ndotl = saturate(dot(normal, -LightDirection.xyz));
    float3 lighting = float3(0.25f, 0.25f, 0.25f) + LightColor.rgb * ndotl;

    float4 baseColor = BaseColorTexture.Sample(BaseColorSampler, input.UV);
    return float4(baseColor.rgb * lighting, baseColor.a);
}
