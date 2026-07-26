// IBL(Image Based Lighting)のオフスクリーン畳み込みパス。実行はエンジン起動時に一度だけ
// (スカイボックスは静的なため)。出力は本物のTextureCube(CreateUAVTextureCube/
// CreateMippedUAVTextureCubeで作成、面ごとに個別のUAV(Texture2DArray、要素数1)を持つ)で、
// HLSLがリソースを動的にスライス選択できないため、C++側(KurenaiEngine3D::Render)が
// 面ごとに1回ずつこのシェーダーをディスパッチする(カスケードシャドウマップのテクスチャ分岐と
// 同種の制約)。どの面を処理するかはIBLFaceConstants.Faceで受け取る。
//
// CSIrradiance: 拡散イラディアンス。出力テクセルが表す法線方向Nに対し、接空間の半球を
// (phi, theta)の等間隔グリッドで離散化してcosθ*sinθ重み付き積分する(Lambertian BRDFの1/πと
// 積分のπが相殺されるよう正規化済み。DeferredLighting.hlsl側は追加のπ処理なしでそのまま
// albedoに乗算できる)。
//
// CSPrefilter: プリフィルタ済み鏡面。ミップレベルごとに異なるラフネスでGGXインポータンスサンプリングし、
// 出力テクセルが表す反射方向R周りのラディアンスをNdotL重み付き平均する。リアルタイムIBLの定番近似
// (Karis 2013, "Real Shading in Unreal Engine 4")に従いV=N=Rと仮定する(視線依存の歪みが出るが
// 実用上十分な近似として広く使われている)

static const float PI = 3.14159265359f;

TextureCube SourceSkybox : register(t0);
SamplerState SourceSampler : register(s0);

// IBLConvolve.hlsl側のこの宣言とC++側 KurenaiEngine3D.cpp の IBLFaceConstants を一致させる必要がある
cbuffer IBLFaceConstants : register(b0)
{
    uint Face;         // 処理対象の面(D3Dのキューブマップ標準順: +X=0,-X=1,+Y=2,-Y=3,+Z=4,-Z=5)
    float Roughness;   // CSPrefilterのみ使用。ミップが表すラフネス値
    float2 FacePadding;
};

// キューブマップの1面上のUV([0,1]^2)から、その面・そのテクセルが表す方向を求める
// (D3Dの標準的な面→方向マッピング。KurenaiEngine3D.cpp/generate_sky_cubemap.pyの
// face_direction_gridと同じ規約に揃えている)
float3 CubeFaceDirection(uint face, float2 uv)
{
    float2 ndc = uv * 2.0f - 1.0f;
    float u = ndc.x;
    float v = ndc.y;

    float3 dir;
    if (face == 0)      dir = float3(1.0f, -v, -u);   // +X
    else if (face == 1) dir = float3(-1.0f, -v, u);   // -X
    else if (face == 2) dir = float3(u, 1.0f, v);     // +Y
    else if (face == 3) dir = float3(u, -1.0f, -v);   // -Y
    else if (face == 4) dir = float3(u, -v, 1.0f);    // +Z
    else                dir = float3(-u, -v, -1.0f);  // -Z

    return normalize(dir);
}

RWTexture2DArray<float4> IrradianceOut : register(u0);

[numthreads(8, 8, 1)]
void CSIrradiance(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height, elements;
    IrradianceOut.GetDimensions(width, height, elements);
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
    {
        return;
    }

    const float2 uv = (float2(dispatchThreadID.xy) + 0.5f) / float2(width, height);
    const float3 N = CubeFaceDirection(Face, uv);
    const float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 tangentX = normalize(cross(up, N));
    const float3 tangentY = cross(N, tangentX);

    float3 irradiance = float3(0.0f, 0.0f, 0.0f);
    uint sampleCount = 0;

    const float kPhiStep = 0.025f;
    const float kThetaStep = 0.025f;

    [loop]
    for (float phi = 0.0f; phi < 2.0f * PI; phi += kPhiStep)
    {
        [loop]
        for (float theta = 0.0f; theta < 0.5f * PI; theta += kThetaStep)
        {
            const float3 tangentSample = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            const float3 sampleDir = tangentX * tangentSample.x + tangentY * tangentSample.y + N * tangentSample.z;

            irradiance += SourceSkybox.SampleLevel(SourceSampler, sampleDir, 0.0f).rgb * cos(theta) * sin(theta);
            sampleCount += 1;
        }
    }

    irradiance = PI * irradiance / max(float(sampleCount), 1.0f);
    IrradianceOut[uint3(dispatchThreadID.xy, 0)] = float4(irradiance, 1.0f);
}

RWTexture2DArray<float4> PrefilterOut : register(u0);

float RadicalInverseVdC_P(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float2 Hammersley_P(uint i, uint n)
{
    return float2(float(i) / float(n), RadicalInverseVdC_P(i));
}

float3 ImportanceSampleGGX_P(float2 xi, float3 N, float roughness)
{
    const float a = roughness * roughness;

    const float phi = 2.0f * PI * xi.x;
    const float cosTheta = sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    const float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

    float3 H;
    H.x = sinTheta * cos(phi);
    H.y = sinTheta * sin(phi);
    H.z = cosTheta;

    const float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 tangentX = normalize(cross(up, N));
    const float3 tangentY = cross(N, tangentX);

    return tangentX * H.x + tangentY * H.y + N * H.z;
}

static const uint kPrefilterSampleCount = 256;

[numthreads(8, 8, 1)]
void CSPrefilter(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height, elements;
    PrefilterOut.GetDimensions(width, height, elements);
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
    {
        return;
    }

    const float2 uv = (float2(dispatchThreadID.xy) + 0.5f) / float2(width, height);
    const float3 N = CubeFaceDirection(Face, uv);

    // リアルタイムIBLの定番近似(Karis 2013): V=R=Nと仮定し、視線依存の伸び(斜め視線での
    // 反射像の伸長)を無視する
    const float3 V = N;

    if (Roughness < 1e-3f)
    {
        // ラフネス0(ミップ0)は鏡面そのものなので畳み込み不要。そのままスカイボックスをコピーする
        PrefilterOut[uint3(dispatchThreadID.xy, 0)] = float4(SourceSkybox.SampleLevel(SourceSampler, N, 0.0f).rgb, 1.0f);
        return;
    }

    float3 prefilteredColor = float3(0.0f, 0.0f, 0.0f);
    float totalWeight = 0.0f;

    [loop]
    for (uint i = 0; i < kPrefilterSampleCount; ++i)
    {
        const float2 xi = Hammersley_P(i, kPrefilterSampleCount);
        const float3 H = ImportanceSampleGGX_P(xi, N, Roughness);
        const float3 L = normalize(2.0f * dot(V, H) * H - V);

        const float NdotL = saturate(dot(N, L));
        if (NdotL > 0.0f)
        {
            prefilteredColor += SourceSkybox.SampleLevel(SourceSampler, L, 0.0f).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    prefilteredColor = (totalWeight > 0.0f) ? (prefilteredColor / totalWeight) : SourceSkybox.SampleLevel(SourceSampler, N, 0.0f).rgb;
    PrefilterOut[uint3(dispatchThreadID.xy, 0)] = float4(prefilteredColor, 1.0f);
}
