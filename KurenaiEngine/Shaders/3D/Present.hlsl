// Mode: 0=RGB(SceneColor/Albedo/Normal/Material/SSILの間接光など、そのまま表示できるバッファ用)
//       1=単チャンネルの深度をそのままpow()でコントラストを持ち上げて表示(シャドウマップ用。
//         正射影のため深度がライト視点距離に対して線形に分布し、これで十分見やすくなる)
//       2=透視投影のGBuffer深度用。NDC深度は遠方ほど値が1.0f付近に密集する非線形分布のため、
//         そのままpow()しても見分けがつかない。ワールド座標を再構成しカメラからの距離を
//         線形にグレースケール化する
//       3=AO/GIバッファのa(遮蔽率)チャンネルをグレースケール表示(SSAOのrgbは常に0のため専用)
//       4=直接光パスの結果(HDR、トーンマッピング前)をReinhardトーンマッピング+ガンマ補正して表示
//       5=深度の生値(0〜1)を加工せずそのままグレースケール表示(reverse-z等の生値確認用)
//       6=Hi-Zミップチェーンの指定ミップ(MipLevel)をSampleLevelで読み、生値のままグレースケール表示
//       7=G-Bufferのオクタヘドラルエンコード法線(R16G16_Float)をデコードし、[-1,1]を[0,1]へ
//         再マップして表示(法線マップのデバッグ表示で見慣れた配色にするため)
#include "NormalEncoding.hlsli"

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    // カスケードシャドウマップ用(このシェーダでは未使用。オフセット合わせのためだけに宣言する)
    float4x4 CascadeViewProj[4];
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
    float4x4 View;
    float4x4 Proj;
};

cbuffer PresentConstants : register(b1)
{
    int Mode;
    float MipLevel;
    float2 PresentPadding;
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
    if (Mode == 6)
    {
        // Hi-Zはミップごとに解像度が異なるため、画面スケールから決まる自動ミップ選択(Sample)ではなく
        // 明示的に指定したミップ(MipLevel)を必ず読む
        float depth = SourceTexture.SampleLevel(DefaultSampler, input.UV, MipLevel).r;
        return float4(depth, depth, depth, 1.0f);
    }

    float4 sourceColor = SourceTexture.Sample(DefaultSampler, input.UV);

    if (Mode == 7)
    {
        float3 n = OctDecode(sourceColor.xy);
        return float4(n * 0.5f + 0.5f, 1.0f);
    }

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

    if (Mode == 4)
    {
        float3 color = sourceColor.rgb / (sourceColor.rgb + 1.0f);
        color = pow(color, 1.0f / 2.2f);
        return float4(color, 1.0f);
    }

    if (Mode == 5)
    {
        float depth = sourceColor.r;
        return float4(depth, depth, depth, 1.0f);
    }

    return float4(sourceColor.rgb, 1.0f);
}
