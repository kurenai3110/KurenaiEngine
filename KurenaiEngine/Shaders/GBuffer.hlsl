cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 LightViewProj;
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
};

cbuffer MaterialConstants : register(b1)
{
    float MetallicFactor;
    float RoughnessFactor;
    float2 MaterialPadding;
};

Texture2D BaseColorTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D MetallicRoughnessTexture : register(t2);
SamplerState DefaultSampler : register(s0);

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
    float3 WorldPos : TEXCOORD1;
    float2 UV : TEXCOORD0;
};

struct PSOutput
{
    float4 Albedo : SV_TARGET0;
    float4 Normal : SV_TARGET1;
    float4 Material : SV_TARGET2;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.Position = mul(float4(input.Position, 1.0f), ViewProj);
    output.Normal = input.Normal;
    output.WorldPos = input.Position;
    output.UV = input.UV;
    return output;
}

// 頂点の接線を持たないため、UV/位置の画面空間微分から接線フレームを近似する
// (Christian Schuler "Normal Mapping without Precomputed Tangents" の手法)
float3x3 ComputeTangentFrame(float3 N, float3 worldPos, float2 uv)
{
    float3 dp1 = ddx(worldPos);
    float3 dp2 = ddy(worldPos);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);

    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    float invMax = rsqrt(max(dot(T, T), dot(B, B)));
    return float3x3(T * invMax, B * invMax, N);
}

PSOutput PSMain(PSInput input)
{
    float4 baseColorSample = BaseColorTexture.Sample(DefaultSampler, input.UV);

    float3 geometricNormal = normalize(input.Normal);

    // BC5(2チャンネル、X/Yのみ)圧縮された法線マップはB/Aチャンネルにデータを持たず、
    // サンプリング時にハードウェアがB=0を返すため、Bをそのまま使うとタンジェント空間Zが
    // 常に-1(裏向き)になってしまう。単位ベクトルである前提でX/YからZを再構成する
    // (通常の3チャンネル法線マップに対しても正しく機能する)
    float2 normalXY = NormalTexture.Sample(DefaultSampler, input.UV).xy * 2.0f - 1.0f;
    float normalZ = sqrt(saturate(1.0f - dot(normalXY, normalXY)));
    float3 normalSample = float3(normalXY, normalZ);
    float3x3 tbn = ComputeTangentFrame(geometricNormal, input.WorldPos, input.UV);
    float3 N = normalize(mul(normalSample, tbn));

    float3 metallicRoughnessSample = MetallicRoughnessTexture.Sample(DefaultSampler, input.UV).rgb;
    float metallic = saturate(MetallicFactor * metallicRoughnessSample.b);
    float roughness = clamp(RoughnessFactor * metallicRoughnessSample.g, 0.045f, 1.0f);

    PSOutput output;
    output.Albedo = float4(baseColorSample.rgb, 1.0f);
    output.Normal = float4(N * 0.5f + 0.5f, 0.0f);
    output.Material = float4(metallic, roughness, 0.0f, 0.0f);
    return output;
}
