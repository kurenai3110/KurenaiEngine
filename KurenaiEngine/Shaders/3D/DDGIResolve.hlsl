// DDGIの拡散間接光だけを低解像度で評価するパス。Lightingパスの直前に走る。
//
// 【なぜ分離したか】Lightingパスの内訳を測るとDDGIのサンプリングが単独最大だった
// (実測は docs/ImplementationHistory.md 41.15)。SampleDDGIIrradianceは1画素あたり
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
//   SV_TARGET1: このテクセルが代表している全解像度の深度(41.24節)。
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

#include "ShaderInterop/FrameConstants.hlsli"

#include "DDGI.hlsli"

Texture2D DepthTexture : register(t2);
Texture2D NormalTexture : register(t3);

#include "ShaderInterop/FullscreenTriangle.hlsli"

#include "ShaderInterop/Common.hlsli"

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
