// IBL(split-sum近似, Karis 2013 "Real Shading in Unreal Engine 4")の第2項、
// BRDF積分ルックアップテーブルの生成。スカイボックスに依存しないため、エンジン起動時に一度だけ
// (NdotV, ラフネス)の128x128グリッドをコンピュートシェーダーで焼く。実行時はDeferredLighting.hlsl側で
// このテーブルの(x=スケール, y=バイアス)を F0*x + y として鏡面フレネル項に適用する
static const float PI = 3.14159265359f;
static const uint kSampleCount = 1024;

RWTexture2D<float2> BRDFLUT : register(u0);

// Hammersley点列(低不一致列)。GGXインポータンスサンプリングの2次元サンプル座標に使う
float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float2 Hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), RadicalInverseVdC(i));
}

float3 ImportanceSampleGGX(float2 xi, float3 N, float roughness)
{
    float a = roughness * roughness;

    float phi = 2.0f * PI * xi.x;
    float cosTheta = sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

    float3 H;
    H.x = sinTheta * cos(phi);
    H.y = sinTheta * sin(phi);
    H.z = cosTheta;

    float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangentX = normalize(cross(up, N));
    float3 tangentY = cross(N, tangentX);

    return tangentX * H.x + tangentY * H.y + N * H.z;
}

// IBL用のSchlick-GGX可視性項はダイレクトライト用(k=(r+1)^2/8)とは異なりk=roughness^2/2を使う(Karis 2013)
float GeometrySchlickGGX_IBL(float NdotX, float roughness)
{
    float k = (roughness * roughness) / 2.0f;
    return NdotX / (NdotX * (1.0f - k) + k);
}

float GeometrySmith_IBL(float NdotV, float NdotL, float roughness)
{
    return GeometrySchlickGGX_IBL(NdotV, roughness) * GeometrySchlickGGX_IBL(NdotL, roughness);
}

float2 IntegrateBRDF(float NdotV, float roughness)
{
    float3 V;
    V.x = sqrt(1.0f - NdotV * NdotV);
    V.y = 0.0f;
    V.z = NdotV;

    float A = 0.0f;
    float B = 0.0f;
    const float3 N = float3(0.0f, 0.0f, 1.0f);

    [loop]
    for (uint i = 0; i < kSampleCount; ++i)
    {
        float2 xi = Hammersley(i, kSampleCount);
        float3 H = ImportanceSampleGGX(xi, N, roughness);
        float3 L = normalize(2.0f * dot(V, H) * H - V);

        float NdotL = saturate(L.z);
        float NdotH = saturate(H.z);
        float VdotH = saturate(dot(V, H));

        if (NdotL > 0.0f)
        {
            float G = GeometrySmith_IBL(NdotV, NdotL, roughness);
            float Gvis = (G * VdotH) / max(NdotH * NdotV, 1e-5f);
            float Fc = pow(1.0f - VdotH, 5.0f);

            A += (1.0f - Fc) * Gvis;
            B += Fc * Gvis;
        }
    }

    return float2(A, B) / float(kSampleCount);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height;
    BRDFLUT.GetDimensions(width, height);
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
    {
        return;
    }

    const float NdotV = (float(dispatchThreadID.x) + 0.5f) / float(width);
    const float roughness = (float(dispatchThreadID.y) + 0.5f) / float(height);

    BRDFLUT[dispatchThreadID.xy] = IntegrateBRDF(max(NdotV, 1e-3f), roughness);
}
