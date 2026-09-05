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

// C++側 KurenaiEngine3D.cpp の FrameConstants と並びを一致させること。
// このシェーダーが実際に読むのは InvViewProj / CameraPosition / TimeParams /
// SkySunDirection / SkyParams / CloudParams0-3 / FogParams0 / StarsParams だけだが、
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
    // DDGIのクリップマップLOD(31.4.2節)。**要素数はC++側のkDDGIMaxLODCountと一致させること。**
    // 読むのはDDGI.hlsliだけだが、cbufferは宣言順でオフセットが決まるため、
    // DDGIParams4の後ろのフィールドを読むシェーダーはすべてここへ同じ宣言が要る
    // (飛ばすと以降のフィールドが64バイトずれ、コンパイルは通るのに別の値を読む)
    float4 DDGILODOrigin[4];
    float4 DDGILODBase[4];
    float4 OcclusionParams;
    // x=星のまたたきに使う時刻
    float4 TimeParams;
    // xyz=太陽が「ある」向き(未正規化。MakeSkyParametersでnormalizeする)
    float4 SkySunDirection;
    // z=太陽照度/空照度比(EvaluateCloudLayerが雲の明るさの基準に使う)
    float4 SkyParams;
    float4 CloudParams0;
    float4 CloudParams1;
    float4 CloudParams2;
    float4 CloudParams3;
    float4 PlanarReflectionPlane;
    float4 FogParams0;
    float4 FogParams1;
    float4 WaterBodyColor;
    float4 StarsParams;
    // x=積雲のボリュームレイマーチの段数(0以下ならSky.hlsliのコンパイル時の既定)。yzwは予備。
    // **FrameConstantsの末尾にあること**。ここより手前へ入れると、途中までしか宣言していない
    // 他のシェーダー(AerialPerspective/PlanarReflection等)のオフセットが全部ずれる
    float4 CloudQualityParams;
};

// SkyIntegrate.hlslが書いた空パラメータ(ティント4本と正規化済みの天頂輝度)
StructuredBuffer<GPUSkyParameters> SkyParametersBuffer : register(t3);

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

// DeferredLighting.hlslのReconstructWorldPosと同一の内容。
// UVは0..1の画面座標なので、このパスが低解像度で走っていても
// フル解像度と同じ視線方向の場を張る(だからバイリニア補間が意味を持つ)
float3 ReconstructWorldPos(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos = mul(clipPos, InvViewProj);
    return worldPos.xyz / worldPos.w;
}

// FrameConstantsのSky*フィールドからSky.hlsliのSkyParametersを組み立てる。
// DeferredLighting.hlsl/SSR.hlsl/AerialPerspective.hlsl/PlanarReflection.hlslの
// MakeSkyParametersと完全に同一の内容であること。5つのシェーダーはcbufferをそれぞれ別に
// 宣言しているため関数そのものは共有できず複製しているが、中身がずれると
// 「背景に見える雲」「水面に映る雲」が食い違ってしまうため、変える場合は必ず5つとも同時に直すこと
SkyParameters MakeSkyParameters(float2 pixelPosition)
{
    SkyParameters params;
    params.SunDirection = normalize(SkySunDirection.xyz);
    params = ApplySkyParametersFromBuffer(params, SkyParametersBuffer[0]);
    params.SunToSkyIlluminanceRatio = SkyParams.z;
    params.CloudCoverage = CloudParams0.x;
    params.CloudAltitude = CloudParams0.y;
    params.CloudUvScale = CloudParams0.z;
    params.CloudDensity = CloudParams0.w;
    params.CloudScrollOffset = CloudParams1.xy;
    params.CloudForwardG = CloudParams1.z;
    params.CloudThickness = CloudParams1.w;
    params.CirrusCoverage = CloudParams2.x;
    params.CirrusAltitude = CloudParams2.y;
    params.CirrusUvScale = CloudParams2.z;
    params.CirrusDensity = CloudParams2.w;
    params.CirrusScrollOffset = CloudParams3.xy;
    params.CirrusAnisotropy = CloudParams3.z;
    // 雲の種類の偏り(C4)。CloudParams3.wはこれまで未使用だった枠なので、FrameConstantsは1バイトも増えない
    params.CloudTypeBias = CloudParams3.w;
    // ボリューム経路のレイマーチ段数。**このシェーダーだけが上書きする**
    // (他のシェーダーはApplySkyParametersFromBufferが入れた0のままで、
    //  Sky.hlsliのコンパイル時の既定へ落ちる。SkyParameters::CloudRaymarchSteps参照)
    params.CloudRaymarchSteps = (int)CloudQualityParams.x;
    // 【P17でfloat3を渡す】雲層はワールド座標に固定されており、レイの起点(視点)の
    // XZまで要る。ここをCameraPosition.yに戻すと雲が視点に追従して破綻する
    params = ApplyCloudFogParameters(params, FogParams0, CameraPosition.xyz);
    // レイマーチの開始位置を画素ごとにずらす量(C2)。スライスの縞をディザへ変える。
    // このパスは低解像度なので、ずらしの粒度も低解像度の画素になる
    params.RaymarchJitter = CloudRaymarchDither(pixelPosition);
    params.StarsIntensity = StarsParams.x;
    params.StarsDensity = StarsParams.y;
    params.StarsTwinkle = StarsParams.z;
    params.StarsPixelAngle = StarsParams.w;
    params.StarsTime = TimeParams.x;
    return params;
}

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
