// スクリーンスペースリフレクション(SSR)パス。
// Lightingパスで完成したSceneColor(HDR、トーンマップ前)を「反射先の環境色」として
// 簡易的に再利用し、G-Buffer(Normal/Material/Depth)を使ってワールド空間でレイマーチングする。
// HDRのまま反射色を加算するため、1.0を超える輝度(明るい光源の反射など)も正しく合成できる。
// トーンマッピングはこのパスより後段のTonemap.hlsl(Present直前)でまとめて行う。
// スカイボックスへのフォールバックは、レイが画面内で実際に背景(深度なし)ピクセルへ到達したことを
// 確認できた場合のみ行う。画面外に外れた場合や最大距離まで判定がつかなかった場合は、その先に
// 何があるか(スカイなのか、単に画面外の別ジオメトリなのか)分からないため反射を追加しない。
// そうしないと、洞窟のように周囲が完全に遮蔽された空間でも、レイが画面外に外れただけで
// 誤って空が映り込んでしまう。
// このエンジンにはレンダーグラフ/コンピュートシェーダー/Hi-Zミップチェーン/PSOのブレンドステートが
// 未実装のため、既存のSSAO/SSILと同じフルスクリーン三角形+ピクセルシェーダーのパターンで実装し、
// 反射色の合成もブレンドステートではなくこのシェーダー内で直接加算する。
#include "NormalEncoding.hlsli"

static const int kSSRStepCount = 32;
static const int kSSRBinaryStepCount = 6;
static const float kSSREdgeFadeDistance = 0.1f;

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 LightViewProj;
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
    float4x4 View;
    float4x4 Proj;
    float4 AmbientColor;
};

cbuffer SSRConstants : register(b1)
{
    float4 Params0; // x: 最大レイ距離(ワールド単位), y: ヒット判定の厚み, z: ラフネスカットオフ, w: 未使用
};

Texture2D SceneColorTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D MaterialTexture : register(t2);
Texture2D DepthTexture : register(t3);
TextureCube SkyboxTexture : register(t4);
Texture2D AlbedoTexture : register(t5);
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

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// ワールド座標を画面UVとView空間Z(カメラからの距離。値が大きいほど遠い)へ投影する。
// カメラ背後、または画面外に出た場合はfalseを返す
bool ProjectToScreen(float3 worldPos, out float2 uv, out float viewZ)
{
    float4 clipPos = mul(float4(worldPos, 1.0f), ViewProj);
    if (clipPos.w <= 0.0f)
    {
        uv = float2(0.0f, 0.0f);
        viewZ = 0.0f;
        return false;
    }

    float3 ndc = clipPos.xyz / clipPos.w;
    uv = float2(ndc.x * 0.5f + 0.5f, 1.0f - (ndc.y * 0.5f + 0.5f));
    viewZ = mul(float4(worldPos, 1.0f), View).z;
    return (uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f);
}

// UV位置の実際のジオメトリのView空間Zを取得する。背景(深度なし)ならfalseを返す
bool SampleSceneViewZ(float2 uv, out float viewZ)
{
    float sceneDepth = DepthTexture.Sample(DefaultSampler, uv).r;
    if (sceneDepth <= 0.0f)
    {
        viewZ = 0.0f;
        return false;
    }
    float3 sceneWorldPos = ReconstructWorldPos(uv, sceneDepth);
    viewZ = mul(float4(sceneWorldPos, 1.0f), View).z;
    return true;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 baseColor = SceneColorTexture.Sample(DefaultSampler, input.UV).rgb;

    float depth = DepthTexture.Sample(DefaultSampler, input.UV).r;
    if (depth <= 0.0f)
    {
        // 背景(スカイ)には反射元のサーフェスがない
        return float4(baseColor, 1.0f);
    }

    float3 albedo = AlbedoTexture.Sample(DefaultSampler, input.UV).rgb;
    float2 material = MaterialTexture.Sample(DefaultSampler, input.UV).rg;
    float metallic = material.r;
    float roughness = material.g;

    const float maxDistance = Params0.x;
    const float thickness = Params0.y;
    const float roughnessCutoff = Params0.z;

    // ミップ/ブラーによる粗さ表現がないため、粗い面ほど反射を弱めてノイズ化を防ぐ
    float roughnessFade = 1.0f - smoothstep(0.0f, roughnessCutoff, roughness);
    if (roughnessFade <= 0.0f)
    {
        return float4(baseColor, 1.0f);
    }

    float3 worldPos = ReconstructWorldPos(input.UV, depth);
    float3 N = OctDecode(NormalTexture.Sample(DefaultSampler, input.UV).xy);
    float3 V = normalize(CameraPosition.xyz - worldPos);
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float NdotV = saturate(dot(N, V));
    float3 fresnel = FresnelSchlick(NdotV, F0);
    float reflectivity = max(fresnel.r, max(fresnel.g, fresnel.b));

    float weight = reflectivity * roughnessFade;
    if (weight <= 0.001f)
    {
        return float4(baseColor, 1.0f);
    }

    float3 reflectDir = normalize(reflect(-V, N));

    // 線形マーチ: レイに沿って一定間隔でサンプルし、G-Buffer深度より奥に入った地点(ヒット)を探す
    const float stepSize = maxDistance / float(kSSRStepCount);
    bool hit = false;
    bool skyHit = false;
    float2 hitUV = float2(0.0f, 0.0f);
    float tPrev = 0.0f;
    float tCurr = 0.0f;

    [loop]
    for (int i = 1; i <= kSSRStepCount; ++i)
    {
        tPrev = tCurr;
        tCurr = stepSize * float(i);

        float3 samplePos = worldPos + reflectDir * tCurr;
        float2 sampleUV;
        float rayViewZ;
        if (!ProjectToScreen(samplePos, sampleUV, rayViewZ))
        {
            // 画面外に外れた: この先に何があるか(スカイか別のジオメトリか)分からないため打ち切る
            break;
        }

        float sceneViewZ;
        if (!SampleSceneViewZ(sampleUV, sceneViewZ))
        {
            // 画面内で背景(スカイ)ピクセルに到達したことが確定したので、以降はスカイボックスへ
            // フォールバックしてよい
            skyHit = true;
            break;
        }

        if (rayViewZ >= sceneViewZ && rayViewZ - sceneViewZ < thickness)
        {
            hit = true;
            hitUV = sampleUV;
            break;
        }
    }

    if (hit)
    {
        // 2分探索でヒット区間[tPrev, tCurr]を精密化し、貫通による誤差を減らす
        float tLo = tPrev;
        float tHi = tCurr;
        [unroll]
        for (int j = 0; j < kSSRBinaryStepCount; ++j)
        {
            float tMid = (tLo + tHi) * 0.5f;
            float3 samplePos = worldPos + reflectDir * tMid;
            float2 sampleUV;
            float rayViewZ;
            float sceneViewZ;
            if (ProjectToScreen(samplePos, sampleUV, rayViewZ) && SampleSceneViewZ(sampleUV, sceneViewZ) && rayViewZ >= sceneViewZ)
            {
                hitUV = sampleUV;
                tHi = tMid;
            }
            else
            {
                tLo = tMid;
            }
        }

        float3 reflectionColor = SceneColorTexture.Sample(DefaultSampler, hitUV).rgb;

        // 反射先が画面の縁に近いほど弱める(画面外へレイが抜ける際の急な打ち切りを緩和する)
        float2 edgeDist = min(hitUV, float2(1.0f, 1.0f) - hitUV);
        float edgeFade = saturate(min(edgeDist.x, edgeDist.y) / kSSREdgeFadeDistance);

        return float4(baseColor + reflectionColor * weight * edgeFade, 1.0f);
    }

    if (skyHit)
    {
        // 画面内で実際にスカイへ到達したことが確定した場合のみ、reflectDir方向の正しい
        // スカイ色をスカイボックスから直接サンプルする
        float3 skyColor = SkyboxTexture.Sample(DefaultSampler, reflectDir).rgb;
        return float4(baseColor + skyColor * weight, 1.0f);
    }

    // 画面外に外れた、または最大距離まで判定がつかなかった場合は、その先に何があるか
    // 不明なため反射を追加しない(洞窟内などで誤って空を映り込ませないため)
    return float4(baseColor, 1.0f);
}
