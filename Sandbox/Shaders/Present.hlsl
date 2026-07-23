// Mode: 0=RGB(SceneColor/Albedo/Normal/Material/SSILの間接光など、そのまま表示できるバッファ用)
//       1=単チャンネルの深度をそのままpow()でコントラストを持ち上げて表示(シャドウマップ用。
//         正射影のため深度がライト視点距離に対して線形に分布し、これで十分見やすくなる)
//       2=透視投影のGBuffer深度用。NDC深度は遠方ほど値が1.0f付近に密集する非線形分布のため、
//         そのままpow()しても見分けがつかない。ワールド座標を再構成しカメラからの距離を
//         線形にグレースケール化する
//       3=AO/GIバッファのa(遮蔽率)チャンネルをグレースケール表示(SSAOのrgbは常に0のため専用)
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

cbuffer PresentConstants : register(b1)
{
    int Mode;
    float3 PresentPadding;
};

Texture2D SourceTexture : register(t0);
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

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 sourceColor = SourceTexture.Sample(DefaultSampler, input.UV);

    if (Mode == 1)
    {
        float depth = pow(saturate(sourceColor.r), 0.25f);
        return float4(depth, depth, depth, 1.0f);
    }

    if (Mode == 2)
    {
        float3 worldPos = ReconstructWorldPos(input.UV, sourceColor.r);
        float viewZ = mul(float4(worldPos, 1.0f), View).z;
        // カメラからの距離をz/(z+K)で0〜1に正規化する(Kはシーン規模を問わず見やすくなるよう
        // 経験的に選んだ値)。近いほど暗く、遠いほど明るいグレースケールになる
        float depth = saturate(viewZ / (viewZ + 20.0f));
        return float4(depth, depth, depth, 1.0f);
    }

    if (Mode == 3)
    {
        float ao = sourceColor.a;
        return float4(ao, ao, ao, 1.0f);
    }

    return float4(sourceColor.rgb, 1.0f);
}
