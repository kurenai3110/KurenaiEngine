// 雲(積雲+巻雲)だけを低解像度で評価するパス。Lightingパスの直前に走る。
//
// 【なぜ分離したか】雲の評価は背景1画素あたり値ノイズを数十回踏むため極端に重い。
// Intel UHD Graphics 620 / 1280x720 / DX11 / Release の実測では、Lightingパス19.4msのうち
//   ・積雲のボリュームレイマーチ 約10.4ms
//   ・積雲の平面レイヤー(基本fBm + 自己影5段) 約5.9ms
//   ・巻雲 約1.3ms
// で、雲だけでGPUフレーム時間31.7msの半分以上を占めていた。一方で雲は空間周波数が低く、
// 低解像度で評価してバイリニアで引き伸ばしても見た目の劣化が小さい。
//
// 【なぜ厳密に分離できるか】Sky.hlsliのSkyColorは
//     clearColor * transmittance + scatteredLight
// という事前乗算(premultiplied)のover合成になっている。つまり雲は
// 「透過率(スカラ)」と「散乱光(RGB)」の2つで完全に表現されており、この2つを低解像度で
// 求めてバイリニア補間しても、合成の形は変わらない。
// 太陽・星のような高周波成分はclearColor側(SkyColorWithoutClouds)に残るため、
// フル解像度のまま保たれる——ここが「空全体を低解像度化する」案との決定的な違いである。
//
// 【出力】rgb = 散乱光(事前乗算済み) / a = 透過率。
// 合成側(DeferredLighting.hlslの背景分岐)が
//     SkyColorWithoutClouds(rayDir) * a + rgb
// を行う。雲が無い画素では (rgb, a) = (0, 1) が入り、x*1.0+0.0 はIEEE754で厳密にxと
// 一致するため、雲が無いときの絵は分離前と1ビットも変わらない。
//
// 【深度を見ないでよい理由】雲は視線方向だけの関数で、シーンの深度に一切依存しない。
// 低解像度バッファには画面全体ぶんの雲が隙間なく入るので、ジオメトリの輪郭で
// 低解像度の値がにじみ出す(いわゆるbleeding)が起きない。したがって深度を考慮した
// バイラテラルアップサンプルは不要で、素直なバイリニアで正しい。
#include "Samplers.hlsli"
// 大気遠近の透過率(cbufferに依存しない純粋関数)。ApplyCloudFogParametersが使う
#include "HeightFog.hlsli"
// SkyView LUT。SkyCloudLayers自体はLUTを引かないが、Sky.hlsliはSkyColorUpperUnitを
// 常にコンパイルするため宣言が要る。**定義しないとコンパイルエラーになる**
// (Sky.hlsliのSkyViewセクションはフォールバックを意図的に持たない)
#define KURENAI_SKYVIEW_REGISTER t0
// ボリュメトリック積雲が引く3Dノイズ。**このパスが雲の本体を評価する側**なので、
// ここで定義しないとボリュームの経路がコンパイルされず平面の雲に化ける
#define KURENAI_CLOUD_SHAPE_REGISTER t1
#define KURENAI_CLOUD_DETAIL_REGISTER t2
// 焼いた雲のウェザーマップ(H3)。定義しない場合は手続きで評価する経路が残るので絵は出るが、
// レイマーチの1歩が約10倍高くつく。**雲の本体を評価するのはこのパス**なのでここで定義する
#define KURENAI_CLOUD_WEATHER_REGISTER t4
#include "Sky.hlsli"

#include "ShaderInterop/FrameConstants.hlsli"

// SkyIntegrate.hlslが書いた空パラメータ(ティント4本と正規化済みの天頂輝度)
StructuredBuffer<GPUSkyParameters> SkyParametersBuffer : register(t3);

#include "ShaderInterop/FullscreenTriangle.hlsli"

#include "ShaderInterop/Common.hlsli"

#define KURENAI_SKY_WITH_STARS
#define KURENAI_SKY_RAYMARCH_STEPS CloudQualityParams.x
#include "ShaderInterop/SkyFrameParameters.hlsli"

// 2枚出す。
//   SV_TARGET0 … rgb=事前乗算済みの散乱光 / a=透過率
//   SV_TARGET1 … fogInFront(雲に最初に当たった位置の霞の透過率)
//
// 【なぜ1枚に収まらないか】合成側は
//   clearColor * T + S + clearColor * (CloudSkyLight - 1) * (1 - fogInFront)
// を行う(P18b。Sky.hlsliのCloudAirlightCorrection参照)。CloudSkyLightはフレーム定数だが
// float3なので、この式に必要な画素ごとの量は (T, fogInFront) の2スカラ + S の3成分=5chになる。
// 補正項はclearColorに比例するため、こちら側で畳み込むと補正項の中の太陽・星だけが
// 低解像度化してぼける。だからfogInFrontは畳み込まずそのまま持ち出す
struct PSOutput
{
    float4 Cloud : SV_TARGET0;
    float  FogInFront : SV_TARGET1;
};

PSOutput PSMain(PSInput input)
{
    // 背景(遠平面)方向の視線ベクトル。Reverse-Zのため遠平面はNDC z=0.0
    const float3 farPoint = ReconstructWorldPos(input.UV, 0.0f);
    const float3 rayDir = normalize(farPoint - CameraPosition.xyz);

    float transmittance;
    float3 scatteredLight;
    float fogInFront;
    SkyCloudLayers(
        rayDir, MakeSkyParameters(input.Position.xy), transmittance, scatteredLight, fogInFront);

    PSOutput output;
    output.Cloud = float4(scatteredLight, transmittance);
    output.FogInFront = fogInFront;
    return output;
}
