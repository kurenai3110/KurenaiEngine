// トーンマップパス。HDRのSceneColor(SSR有効時はSSR適用後のSceneColor)を読み、
// Reinhardトーンマッピング+ガンマ補正でLDR(0〜1)へ変換してPresentパスへ渡す。
// 以前はDeferredLighting.hlsl(ライティングパス自体)がこの変換まで行っていたが、
// それだとSSRがトーンマップ済みLDRを反射元として読むことになり、反射色が1.0を超えられず
// エネルギー保存が破れていた。SceneColorをHDRのまま保持しトーンマップをPresent直前の
// この独立パスへ切り出すことで、SSR・将来のブルーム/露出制御(M7)が物理的に正しいHDR値の
// 上に成立できるようにする
#include "Samplers.hlsli"

Texture2D SceneColorTexture : register(t0);

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

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 color = SceneColorTexture.Sample(ColorSampler, input.UV).rgb;

    // トーンマッピング(Reinhard)とガンマ補正
    color = color / (color + 1.0f);
    color = pow(color, 1.0f / 2.2f);

    return float4(color, 1.0f);
}
