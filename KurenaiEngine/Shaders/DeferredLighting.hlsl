// 最終合成パス。DirectLightingパスで計算済みの直接光(拡散+鏡面反射、シャドウ適用済み)を
// サンプルし、環境光(時刻に応じて変化、AO/SSILの遮蔽率を適用)・間接拡散光(SSIL使用時)を
// 加算する。PBRのライティング計算自体はDirectLighting.hlsl側で行うため、このパスはバッファの
// 合成のみを行う。出力はHDR(SceneColor、1.0を超える輝度を保持)のままで、トーンマッピングは
// 行わない。SSRパスがこのHDR値を反射元として参照するため、ここでLDRへ落とすとSSRの反射色が
// 1.0を超えられずエネルギー保存が破れる。トーンマッピングはPresent直前のTonemap.hlslで行う
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
    // 昼夜サイクル用。rgb=環境光の色、a=昼度(0=夜,1=昼)
    float4 AmbientColor;
};

Texture2D AlbedoTexture : register(t0);
Texture2D DirectLightTexture : register(t1);
Texture2D MaterialTexture : register(t2);
Texture2D DepthTexture : register(t3);
TextureCube SkyboxTexture : register(t4);
// SSAO/SSIL(Visibility Bitmask)共通のAO/GIバッファ。rgb=間接拡散光(加算)、a=遮蔽率(乗算)
Texture2D AOTexture : register(t5);
// G-Bufferのエミッシブ(自発光)バッファ。AO/シャドウの影響を受けず常に加算する
Texture2D EmissiveTexture : register(t6);
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
    float depth = DepthTexture.Sample(DefaultSampler, input.UV).r;
    if (depth <= 0.0f)
    {
        // 何も描かれなかった背景ピクセル: カメラからそのピクセル方向への視線ベクトルで
        // 空のキューブマップをサンプリングする
        // Reverse-Zのため遠平面(=背景)はNDC z=0.0付近になる
        float3 farPoint = ReconstructWorldPos(input.UV, 0.0f);
        float3 rayDir = normalize(farPoint - CameraPosition.xyz);
        float3 skyColor = SkyboxTexture.Sample(DefaultSampler, rayDir).rgb;
        // 夜は空を暗い紺色へ落とし込む(スカイボックス自体は昼のテクスチャ固定のため)
        const float3 kNightSkyColor = float3(0.01f, 0.012f, 0.02f);
        skyColor = lerp(kNightSkyColor, skyColor, AmbientColor.a);
        return float4(skyColor, 1.0f);
    }

    float3 albedo = AlbedoTexture.Sample(DefaultSampler, input.UV).rgb;
    float metallic = MaterialTexture.Sample(DefaultSampler, input.UV).r;
    float3 diffuseColor = albedo * (1.0f - metallic);

    float4 aoSample = AOTexture.Sample(DefaultSampler, input.UV);
    float ao = aoSample.a;
    float3 indirectLight = aoSample.rgb; // SSIL(Visibility Bitmask)使用時のみ非ゼロ。周囲のサーフェスからの間接拡散光
    float3 directLight = DirectLightTexture.Sample(DefaultSampler, input.UV).rgb; // DirectLighting.hlslで計算済み(シャドウ適用済み)
    float3 emissive = EmissiveTexture.Sample(DefaultSampler, input.UV).rgb;

    // エミッシブは自発光のためAO/シャドウの影響を受けず常に加算する
    float3 color = diffuseColor * (AmbientColor.rgb * ao + indirectLight) + directLight + emissive;

    return float4(color, 1.0f);
}
