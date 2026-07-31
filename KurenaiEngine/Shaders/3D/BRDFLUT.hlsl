// IBL(split-sum近似, Karis 2013 "Real Shading in Unreal Engine 4")の第2項、
// BRDF積分ルックアップテーブルの生成。スカイボックスに依存しないため、エンジン起動時に一度だけ
// (NdotV, ラフネス)の128x128グリッドをコンピュートシェーダーで焼く。実行時はDeferredLighting.hlsl側で
// このテーブルの(x=スケール, y=バイアス)を F0*x + y として鏡面フレネル項に適用する。
//
// 最終LUTは float4(A, B, Eavg, 0)。第3成分Eavgはスペキュラのエネルギー補正のうち
// Kulla-Conty(加算ローブ)方式だけが必要とする半球平均で、下のCSCombineEavgが2パス目で足す。
// 2パスに分けている理由と、追加のSRVスロットを使わずに済ませている理由はCSCombineEavgのコメント参照。
// 可視性項は実行時の直接光BRDF(DirectLighting.hlsl / Transparent.hlsl)と必ず同じものを
// 使う必要があるため、SpecularEnergy.hlsliの共有定義を用いる(そこにkの選定理由を記載)
#include "SpecularEnergy.hlsli"

static const float PI = 3.14159265359f;
static const uint kSampleCount = 1024;

// パス1(CSMain)の出力先。A(スケール)とB(バイアス)だけを持つ中間テクスチャ
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
            float G = GeometrySmith(NdotV, NdotL, roughness);
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

// ------------------------------------------------------------------------------------
// パス2: パス1が焼いた(A, B)へ、Kulla-Contyの加算ローブが必要とする
//        Eavg(ラフネスだけの関数、方向アルベドの半球平均)を足して最終LUTを作る。
//
//   Eavg(α) = 2∫E(µ)µdµ ≒ Σ 2·(A+B)·µ_k·(1/width),  µ_k = (k+0.5)/width
//
// 同一リソースをSRVとUAVへ同時にバインドできないため、パス1の出力を別テクスチャ(SRV)
// として読み、最終LUT(RGBA、UAV)へ書き出す2パス構成にしている。RGBA16Fのtyped UAV load
// はハードウェア機能(Typed UAV Load Additional Formats)依存なので、UAVの読み戻しは使わない。
//
// Eavgは行(ラフネス)ごとに1つの値だが、実行時に追加のテクスチャフェッチを増やしたくないので
// 行内の全テクセルへ同じ値を複製し、既存の1回のサンプルから.bとして読めるようにする。
// これにより実行時のSRVスロットは1つも増えない。
// ------------------------------------------------------------------------------------

Texture2D<float2> ScratchLUT : register(t0);
RWTexture2D<float4> BRDFLUTCombined : register(u0);

[numthreads(8, 8, 1)]
void CSCombineEavg(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height;
    BRDFLUTCombined.GetDimensions(width, height);
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
    {
        return;
    }

    // 自分の行(同じラフネス)をµ方向に走査して半球平均を取る。
    // パス1と同じµの刻みで積分するため、LUTが持つEとEavgが必ず整合する
    float eavg = 0.0f;
    const float invWidth = 1.0f / float(width);
    [loop]
    for (uint k = 0; k < width; ++k)
    {
        const float2 ab = ScratchLUT.Load(int3(int(k), int(dispatchThreadID.y), 0));
        const float mu = (float(k) + 0.5f) * invWidth;
        eavg += 2.0f * (ab.x + ab.y) * mu * invWidth;
    }

    const float2 ab = ScratchLUT.Load(int3(dispatchThreadID.xy, 0));
    BRDFLUTCombined[dispatchThreadID.xy] = float4(ab.x, ab.y, eavg, 0.0f);
}
