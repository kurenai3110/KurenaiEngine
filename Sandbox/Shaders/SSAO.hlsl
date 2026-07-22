// SSAO(Screen Space Ambient Occlusion)パス。
// PSMain: G-BufferのNormal/Depthからサンプリングカーネルを使って遮蔽率を計算する(Texture0=World Normal, Texture1=Depth)
// PSMainBlur: PSMainの出力(タイル状ノイズを含む)を均すための4x4ボックスブラー(Texture0=SSAO Raw)
static const float PI = 3.14159265359f;
static const int kSSAOKernelSize = 16;

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
};

cbuffer SSAOConstants : register(b1)
{
    float4 Samples[kSSAOKernelSize]; // タンジェント空間の半球カーネル(xyz)。原点付近に偏らせてある
    float4 Params;                   // x: 半径, y: バイアス, z: 強さ(べき乗), w: 未使用
};

// PSMainではNormal(t0)/Depth(t1)、PSMainBlurではSSAO Raw(t0)をバインドして使い回す
Texture2D Texture0 : register(t0);
Texture2D Texture1 : register(t1);
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

// ピクセル座標から[0,1)の疑似乱数を得るハッシュ関数(Dave Hoskinsのhash12)。
// タイル状のノイズテクスチャを用意する代わりに、画面全体で高周波なランダム回転を安価に生成する
float Hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float depth = Texture1.Sample(DefaultSampler, input.UV).r;
    if (depth >= 1.0f)
    {
        // 背景(スカイ)は遮蔽なし
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    float3 worldPos = ReconstructWorldPos(input.UV, depth);
    float3 normalWorld = normalize(Texture0.Sample(DefaultSampler, input.UV).xyz * 2.0f - 1.0f);

    float3 viewPos = mul(float4(worldPos, 1.0f), View).xyz;
    float3 viewNormal = normalize(mul(normalWorld, (float3x3)View));

    // ピクセル座標のハッシュから毎ピクセル異なる回転を作り、カーネルサンプルの向きをランダム化する
    // (バンディングを高周波ノイズに変換し、後段のブラーパスで均す)
    float randomAngle = Hash12(input.Position.xy) * 2.0f * PI;
    float2 randomVec = float2(cos(randomAngle), sin(randomAngle));

    float3 tangent = normalize(float3(randomVec, 0.0f) - viewNormal * dot(float3(randomVec, 0.0f), viewNormal));
    float3 bitangent = cross(viewNormal, tangent);
    float3x3 tbn = float3x3(tangent, bitangent, viewNormal);

    const float radius = Params.x;
    const float bias = Params.y;
    const float power = Params.z;

    float occlusion = 0.0f;
    [unroll]
    for (int i = 0; i < kSSAOKernelSize; ++i)
    {
        float3 sampleVec = mul(Samples[i].xyz, tbn);
        float3 samplePos = viewPos + sampleVec * radius;

        float4 offset = mul(float4(samplePos, 1.0f), Proj);
        offset.xyz /= offset.w;
        float2 sampleUV = float2(offset.x * 0.5f + 0.5f, 1.0f - (offset.y * 0.5f + 0.5f));

        if (sampleUV.x < 0.0f || sampleUV.x > 1.0f || sampleUV.y < 0.0f || sampleUV.y > 1.0f)
        {
            continue;
        }

        float sampleDepth = Texture1.Sample(DefaultSampler, sampleUV).r;
        if (sampleDepth >= 1.0f)
        {
            continue;
        }

        // サンプル位置のワールド座標をView空間へ変換し、Z(カメラからの距離)だけを比較に使う
        float3 sampleWorldPos = ReconstructWorldPos(sampleUV, sampleDepth);
        float sampleViewZ = mul(float4(sampleWorldPos, 1.0f), View).z;

        // 遮蔽物がカーネルサンプル位置より手前(視距離が近い)にあれば遮蔽としてカウントする。
        // ただし遠く離れた無関係なジオメトリまで遮蔽扱いしないよう半径ベースで減衰させる(range check)
        float rangeCheck = smoothstep(0.0f, 1.0f, radius / max(abs(viewPos.z - sampleViewZ), 1e-4f));
        occlusion += (sampleViewZ <= samplePos.z - bias ? 1.0f : 0.0f) * rangeCheck;
    }

    float ao = saturate(1.0f - occlusion / float(kSSAOKernelSize));
    ao = pow(ao, power);
    return float4(ao, ao, ao, 1.0f);
}

float4 PSMainBlur(PSInput input) : SV_TARGET
{
    uint width, height;
    Texture0.GetDimensions(width, height);
    float2 texelSize = 1.0f / float2(width, height);

    float sum = 0.0f;
    [unroll]
    for (int x = -2; x <= 1; ++x)
    {
        [unroll]
        for (int y = -2; y <= 1; ++y)
        {
            float2 offsetUV = input.UV + float2(x, y) * texelSize;
            sum += Texture0.Sample(DefaultSampler, offsetUV).r;
        }
    }

    float ao = sum / 16.0f;
    return float4(ao, ao, ao, 1.0f);
}
