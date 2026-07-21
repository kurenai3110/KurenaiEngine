static const float PI = 3.14159265359f;

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
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

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * d * d, 1e-6f);
}

float GeometrySchlickGGX(float NdotX, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdotX / (NdotX * (1.0f - k) + k);
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 baseColorSample = BaseColorTexture.Sample(DefaultSampler, input.UV);

    float3 geometricNormal = normalize(input.Normal);
    float3 normalSample = NormalTexture.Sample(DefaultSampler, input.UV).xyz * 2.0f - 1.0f;
    float3x3 tbn = ComputeTangentFrame(geometricNormal, input.WorldPos, input.UV);
    float3 N = normalize(mul(normalSample, tbn));

    float3 metallicRoughnessSample = MetallicRoughnessTexture.Sample(DefaultSampler, input.UV).rgb;
    float metallic = saturate(MetallicFactor * metallicRoughnessSample.b);
    float roughness = clamp(RoughnessFactor * metallicRoughnessSample.g, 0.045f, 1.0f);

    float3 V = normalize(CameraPosition.xyz - input.WorldPos);
    float3 L = normalize(-LightDirection.xyz);
    float3 H = normalize(V + L);

    float NdotV = saturate(dot(N, V)) + 1e-5f;
    float NdotL = saturate(dot(N, L));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float3 albedo = baseColorSample.rgb;
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float3 diffuseColor = albedo * (1.0f - metallic);

    float3 color = diffuseColor * 0.03f; // 環境光の簡易近似

    if (NdotL > 0.0f)
    {
        float D = DistributionGGX(NdotH, roughness);
        float G = GeometrySmith(NdotV, NdotL, roughness);
        float3 F = FresnelSchlick(VdotH, F0);

        float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, 1e-4f);
        float3 kd = (1.0f - F) * (1.0f - metallic);
        float3 diffuse = kd * albedo / PI;

        color += (diffuse + specular) * LightColor.rgb * NdotL;
    }

    // トーンマッピング(Reinhard)とガンマ補正
    color = color / (color + 1.0f);
    color = pow(color, 1.0f / 2.2f);

    return float4(color, baseColorSample.a);
}
