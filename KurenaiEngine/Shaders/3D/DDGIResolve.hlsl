// DDGIの拡散間接光だけを低解像度で評価するパス。Lightingパスの直前に走る。
//
// 【なぜ分離したか】ProbeTest / 1280x720 / DX11 / Release の実測(A/B)では、
// Lightingパス23.9msの内訳は
//   ・DDGIのサンプリング                                  約10.2ms (43%)
//   ・その他のEvaluateIBL(反射プローブ + 鏡面IBL + BRDF)  約 7.6ms (32%)
//   ・残り(背景の空合成 + G-Buffer読み + 直接光合成)      約 6.1ms (26%)
// で、DDGIのサンプリングが単独最大だった。SampleDDGIIrradianceは1画素あたり
// 周囲8プローブを走査し、各プローブでチェビシェフ可視性(距離アトラス)とイラディアンスの
// 2回サンプルを行う ―― つまり1画素16サンプル + 相応の演算になる。
//
// 【なぜ低解像度でよいか】拡散間接光は空間周波数が低い。DDGIはそもそも数十cm〜数m間隔の
// プローブ格子を補間して作る量なので、画面解像度の細部を持っていない。
//
// 【雲(SkyCloud.hlsl)との決定的な違い】雲は視線方向だけの関数で深度に依存しないため、
// 素直なバイリニアで数学的に正しかった。**こちらは深度と法線に依存する**ため、
// ジオメトリの輪郭をまたいでバイリニア補間すると、手前の面の間接光が奥へ滲む。
// したがって合成側(DeferredLighting.hlsl)は深度を見たバイラテラルアップサンプルを行う。
// つまりこの分離は雲と違って**厳密ではなく近似**であり、既定では無効にしてある
// (品質プリセットの低/中から有効になる)。
//
// 【出力】
//   SV_TARGET0: rgb = DDGIのイラディアンス / a = insideWeight(ボリューム内外のフェード)。
//               合成側は SampleDDGIIrradiance と同じく irradiance = lerp(irradiance, rgb, a) を行う。
//   SV_TARGET1: このテクセルが代表している全解像度の深度(41.23節)。
//
// 【なぜ深度を書き出すのか】合成側のバイラテラルアップサンプルは、周囲4テクセルそれぞれの
// 「代表している深度」を要る。以前はそれを**全解像度の深度テクスチャから4回サンプル**して
// 引き直していたが、その値はこのパスが下でDataSampler(ポイント)・同じUVで引いたものと
// ビット単位で同一である。ここへ書き出しておけば、合成側は低解像度テクスチャへの
// GatherRed 1回で4つとも取れる ―― 1画素8サンプルが5サンプルになる。
// このパスにとっては手元にある値をもう1枚へ書くだけで、追加のサンプルはゼロ。
#include "NormalEncoding.hlsli"
#include "Samplers.hlsli"

// DDGI(22章)のオクタヘドラルアトラス。DDGI.hlsliがこの2枚を読む
#define KURENAI_DDGI_IRRADIANCE_REGISTER t0
#define KURENAI_DDGI_DISTANCE_REGISTER t1

// C++側 KurenaiEngine3D.cpp の FrameConstants と並びを一致させること。
// このシェーダーが実際に読むのは InvViewProj / CameraPosition / DDGIParams0-4 だけだが、
// cbufferのレイアウトは宣言順で決まり途中のフィールドを飛ばせないため、
// 手前のフィールドはオフセット合わせのためだけに宣言する
cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 CascadeViewProj[4];
    float4 CameraPosition;
    float4 LightDirection;
    float4 LightColor;
    float4x4 View;
    float4x4 Proj;
    float4 AmbientColor;
    float4 CascadeSplits;
    float4 ShadowParams;
    float4 ActiveLightCount;
    float4 IBLParams;
    float4 ProbeParams;
    float4 ProbeParams2;
    float4x4 PrevViewProj;
    float4 TAAParams;
    float4 DDGIParams0;
    float4 DDGIParams1;
    float4 DDGIParams2;
    float4 DDGIParams3;
    float4 DDGIParams4;
    // 【宣言はここで止めている】このシェーダーが読むのはDDGIParams4までで、
    // それより後ろ(OcclusionParams以降)は使わない。DDGI.hlsliもこの範囲しか参照しない
};

#include "DDGI.hlsli"

Texture2D DepthTexture : register(t2);
Texture2D NormalTexture : register(t3);

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

// 頂点バッファ無しのフルスクリーン三角形(DeferredLighting.hlslのVSMainと同一)
PSInput VSMain(uint vertexID : SV_VertexID)
{
    PSInput output;
    output.UV = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(output.UV.x * 2.0f - 1.0f, 1.0f - output.UV.y * 2.0f, 0.0f, 1.0f);
    return output;
}

// DeferredLighting.hlslのReconstructWorldPosと同一の内容
float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos = mul(clipPos, InvViewProj);
    return worldPos.xyz / worldPos.w;
}

struct PSOutput
{
    // 並びはKurenaiEngine3D側のDDGIResolveパスのRenderTargetsおよび
    // PSOのRenderTargetFormatsと一致させること
    float4 Irradiance : SV_TARGET0;
    float Depth : SV_TARGET1;
};

PSOutput PSMain(PSInput input)
{
    PSOutput output;
    // 【深度と法線はDataSampler(ポイント)で引く】このパスは低解像度で走るので、
    // リニア補間で引くとG-Bufferの隣り合う画素の深度が混ざり、どのサーフェスにも
    // 属さない位置の間接光を計算することになる。
    // ここで使うUV(このテクセルの中心)は合成側のアップサンプルも同じ式で再現するため、
    // 「このテクセルが代表している全解像度の位置」が両者で一致する
    const float depth = DepthTexture.SampleLevel(DataSampler, input.UV, 0.0f).r;
    // 早期脱出する経路でも必ず書くこと(書き残すとレンダーターゲットの内容が未定義になる)。
    // 背景では深度0がそのまま合成側の「このテクセルは落とす」判定になる
    output.Depth = depth;

    // 背景(スカイ)。Reverse-Zのため遠平面はNDC z=0.0付近になる。
    // 間接光は不要なので、insideWeight=0(=合成側でDDGIを一切混ぜない)を返す
    if (depth <= 0.0f)
    {
        output.Irradiance = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return output;
    }

    // ボリュームが無効なら計算しない。合成側もDDGIParams0.wで弾くが、
    // このパス自体が登録されない条件と揃えておく
    if (DDGIParams0.w <= 0.5f)
    {
        output.Irradiance = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return output;
    }

    const float3 worldPos = ReconstructWorldPos(input.UV, depth);
    const float3 N = OctDecode(NormalTexture.SampleLevel(DataSampler, input.UV, 0.0f).xy);
    const float3 V = normalize(CameraPosition.xyz - worldPos);

    float insideWeight;
    const float3 irradiance = SampleDDGIIrradiance(worldPos, N, V, insideWeight);
    output.Irradiance = float4(irradiance, insideWeight);
    return output;
}
