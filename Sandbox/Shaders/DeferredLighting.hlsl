static const float PI = 3.14159265359f;

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 LightViewProj;
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
};

Texture2D AlbedoTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D MaterialTexture : register(t2);
Texture2D DepthTexture : register(t3);
Texture2D ShadowMapTexture : register(t4);
TextureCube SkyboxTexture : register(t5);
SamplerState DefaultSampler : register(s0);

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

// 頂点バッファなしで画面全体を覆う三角形を1枚だけ生成する定番のテクニック
PSInput VSMain(uint vertexID : SV_VertexID)
{
    PSInput output;
    output.UV = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(output.UV.x * 2.0f - 1.0f, 1.0f - output.UV.y * 2.0f, 0.0f, 1.0f);
    return output;
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos = mul(clipPos, InvViewProj);
    return worldPos.xyz / worldPos.w;
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

// ワールド座標をライト視点のクリップ空間に変換し、シャドウマップと深度比較する。
// 戻り値は0(完全に影)〜1(完全に光が当たる)。シャドウマップの範囲外は影を落とさない。
float ComputeShadowFactor(float3 worldPos, float NdotL)
{
    float4 lightClipPos = mul(float4(worldPos, 1.0f), LightViewProj);
    float3 lightNdc = lightClipPos.xyz / lightClipPos.w;

    if (abs(lightNdc.x) > 1.0f || abs(lightNdc.y) > 1.0f || lightNdc.z < 0.0f || lightNdc.z > 1.0f)
    {
        return 1.0f;
    }

    float2 shadowUV = float2(lightNdc.x * 0.5f + 0.5f, 1.0f - (lightNdc.y * 0.5f + 0.5f));
    float shadowMapDepth = ShadowMapTexture.Sample(DefaultSampler, shadowUV).r;

    // シャドウアクネ対策のバイアス。斜入射(NdotLが小さい)ほどアクネが出やすいため傾斜に応じて大きくする
    const float kShadowBiasMin = 0.0005f;
    const float kShadowBiasMax = 0.0025f;
    float bias = lerp(kShadowBiasMax, kShadowBiasMin, NdotL);

    return (lightNdc.z - bias > shadowMapDepth) ? 0.0f : 1.0f;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float depth = DepthTexture.Sample(DefaultSampler, input.UV).r;
    if (depth >= 1.0f)
    {
        // 何も描かれなかった背景ピクセル: カメラからそのピクセル方向への視線ベクトルで
        // 空のキューブマップをサンプリングする
        float3 farPoint = ReconstructWorldPos(input.UV, 1.0f);
        float3 rayDir = normalize(farPoint - CameraPosition.xyz);
        float3 skyColor = SkyboxTexture.Sample(DefaultSampler, rayDir).rgb;
        skyColor = skyColor / (skyColor + 1.0f);
        skyColor = pow(skyColor, 1.0f / 2.2f);
        return float4(skyColor, 1.0f);
    }

    float3 worldPos = ReconstructWorldPos(input.UV, depth);
    float3 albedo = AlbedoTexture.Sample(DefaultSampler, input.UV).rgb;
    float3 N = normalize(NormalTexture.Sample(DefaultSampler, input.UV).xyz * 2.0f - 1.0f);
    float2 material = MaterialTexture.Sample(DefaultSampler, input.UV).rg;
    float metallic = material.r;
    float roughness = material.g;

    float3 V = normalize(CameraPosition.xyz - worldPos);
    float3 L = normalize(-LightDirection.xyz);
    float3 H = normalize(V + L);

    float NdotV = saturate(dot(N, V)) + 1e-5f;
    float NdotL = saturate(dot(N, L));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float3 diffuseColor = albedo * (1.0f - metallic);

    float3 color = diffuseColor * 0.03f; // 環境光の簡易近似

    if (NdotL > 0.0f)
    {
        float shadow = ComputeShadowFactor(worldPos, NdotL);

        float D = DistributionGGX(NdotH, roughness);
        float G = GeometrySmith(NdotV, NdotL, roughness);
        float3 F = FresnelSchlick(VdotH, F0);

        float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, 1e-4f);
        float3 kd = (1.0f - F) * (1.0f - metallic);
        float3 diffuse = kd * albedo / PI;

        color += (diffuse + specular) * LightColor.rgb * NdotL * shadow;
    }

    // トーンマッピング(Reinhard)とガンマ補正
    color = color / (color + 1.0f);
    color = pow(color, 1.0f / 2.2f);

    return float4(color, 1.0f);
}
