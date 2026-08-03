// 平面反射パス(P6)。水面に不透明ジオメトリ(城など)の鏡像を映すためのフォワードパス。
//
// 【カメラを鏡映して描くことの意味】水面の平面についてXMMatrixReflectで鏡映行列Rを作り、
//   反射用ViewProj = R * View * jitteredProj
// でこのパスを描く(KurenaiEngine3D::Render、水面平面の鏡映カメラ構築箇所を参照)。
// こうして描いた絵は「完全な鏡が水面にあったときにその画素に映るはずの像」が、メインカメラで
// 描いたときと**まったく同じ画面UVの位置**に来る(平面鏡の反射は、鏡映したカメラで景色を
// 撮り直すことと数学的に等価であるため)。これが平面反射を採る最大の理由で、
// SSR(Shaders/3D/SSR.hlsl)のように反射ベクトルを再投影する必要が無く、SSR側は
// このパスの結果を「自分の画面UVでそのままサンプルする」だけでよい(SSR.hlsl PSMain末尾参照)。
// メインカメラと同じジッター済みProjを使うのはこの対応関係を保つためで、
// 別のジッターを使うとサブピクセル単位で画面UVの対応が崩れる。
//
// 【ProbeCapture.hlslとの関係】このシェーダーの土台はProbeCapture.hlsl(反射プローブの
// キャプチャパス)で、cbufferの宣言・ライティングの式は意図的にそちらと同一にしてある
// (式を変えると反射に映る色が本編と食い違うため)。ProbeCaptureと同じく、映るのは
// 「直接光 + カスケードシャドウ + スカイボックス由来のグローバルIBL + DDGI(多重バウンス)」までで、
// SSAO/SSIL/SSRのスクリーンスペース手法・他の反射プローブ・半透明メッシュは含まない
// (反射プローブで既に文書化済みの「反射の中の反射は1バウンスで打ち切る」という割り切りに
// そのまま乗る。ここで新しい判断を増やさない)。ProbeCaptureとの違いは以下の3点のみ:
//   1. レンダーターゲットは1枚(放射輝度のみ。プローブと違い視差補正用の距離は要らない)
//   2. Viewにカメラのビュー行列をそのまま渡す理由がProbeCaptureとは異なる(下記VSMain手前参照)
//   3. 水面より下のジオメトリをSV_ClipDistance0で落とす(下記参照)
//
// 【Viewをカメラのままにする理由】ProbeCaptureは「カスケード選択の深度がカメラ視錐台基準
// だから仕方なく」カメラのViewを使っていた(プローブ自身の視点とは無関係にカメラ視点で
// カスケードを選ぶ、という妥協)。このパスでは事情が違う: 描いているのは鏡像ではなく
// **実在のジオメトリそのもの**である(カメラを鏡映しただけで世界は動かしていない)。
// したがってカメラのViewでカスケードを選ぶことは妥協ではなく、そもそも正しい
// (影を落とす太陽とカスケードの分割は、鏡映されていない現実のカメラ視錐台に対して
// 決まっているべきものだから)。
//
// 【水面より下のジオメトリを落とす】反射に映ってはいけない「水面より下にあるもの」を
// 頂点シェーダーのSV_ClipDistance0で落とす。値は水面平面の方程式にworldPosを代入したもの
// (FrameConstants.PlanarReflectionPlane、水面より上が正)。
// Lengyelの斜め近平面(Oblique Near-Plane Clipping)は採らない――Reverse-Zの深度精度は
// 近平面・遠平面の配置にかなり敏感で、任意の傾いた平面を近平面に差し替える手法は
// この設計に手を入れることになる。SV_ClipDistanceによるクリップはラスタライズ前に
// プリミティブを切り捨てるだけで深度の投影自体は変えないため、Reverse-Zの設計へ触れずに済む。
// SV_ClipDistanceはfxc(SM5.0)・dxc(SM6.0)の両方でコンパイルが通ることをClaude/Tools/check_shaders.ps1で
// 確認済み(このエンジンはDX11がfxc/SM5.0固定、DX12がdxcのため両方で通る必要がある)。
#include "SpecularEnergy.hlsli"
#include "Samplers.hlsli"
// 大気遠近(P8)。フォグの透過率を求める純粋関数(cbufferに依存しない)。AerialPerspective.hlslと共有する
#include "HeightFog.hlsli"
// 空モデル(Perez分布)の共有ヘッダー。大気遠近のin-scatter項に、背景と同じSkyColorを使うため
// (Sky.hlsli冒頭のコメント参照)
// SkyView LUT(P14b)。日中の空はこのLUTを引く。**定義しないと日中の空が黒くなる**ので、
// SkyColorUpperUnitを呼ぶシェーダーは全員定義すること(Sky.hlsliのSkyViewセクション参照)
#define KURENAI_SKYVIEW_REGISTER t15
#include "Sky.hlsli"

static const float PI = 3.14159265359f;

cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProj;    // 反射用ViewProj(R * View * jitteredProj)。ラスタライズに使う
    float4x4 InvViewProj; // このシェーダーでは未使用(オフセット合わせのためだけに宣言する)
    float4x4 CascadeViewProj[4];
    float4 CameraPosition; // 鏡映したカメラ位置(視線ベクトルVの起点。ファイル冒頭参照)
    float4 LightDirection;
    float4 LightColor;
    float4x4 View; // カメラのビュー行列をそのまま渡す(理由はファイル冒頭を参照。ProbeCaptureとは異なる)
    float4x4 Proj; // このシェーダーでは未使用(オフセット合わせのためだけに宣言する)
    float4 AmbientColor;
    float4 CascadeSplits;
    float4 ShadowParams;
    // x=t8のライトリストの有効数(ProbeCapture.hlslと同じく末尾で受け取る)
    float4 ActiveLightCount;
    float4 IBLParams;
    // 反射プローブ用。このシェーダーではプローブを一切参照しない(ファイル冒頭参照)ため未使用だが、
    // 後続のDDGIParamsのオフセットを合わせる目的で宣言する(ProbeCapture.hlslと同じ理由)
    float4 ProbeParams;
    float4 ProbeParams2;
    // TAA(23章)用。このシェーダーでは未使用だが、C++側でDDGIParamsより手前に置かれているため
    // オフセット合わせのためだけに宣言する。ProbeCaptureと同じくPrevViewProj/TAAParamsの実際の値は
    // C++側(KurenaiEngine3D::Render)で「今フレーム=前フレーム、ジッター無し」に明示的に潰される
    float4x4 PrevViewProj;
    float4 TAAParams;
    // DDGI(22章)。多重バウンスのために前フレームのイラディアンスを引くのに使う
    float4 DDGIParams0;
    float4 DDGIParams1;
    float4 DDGIParams2;
    float4 DDGIParams3;
    // x=このフレームの実効プリ露出(アトラスは露出非依存で持つため読み出し時に掛け戻す)
    float4 DDGIParams4;
    // bent normalによる遮蔽(34章)。このシェーダーでは読まないが、C++側のFrameConstantsでは
    // DDGIParams4の直後にあるため、**宣言しないと以降のフィールドが16バイトずれる**。
    // 実際このマージで一度宣言し忘れ、PlanarReflectionPlaneが16バイトずれた結果
    // SV_ClipDistance0が全ジオメトリを切り落とし、水面の鏡像が丸ごと消えた
    float4 OcclusionParams;
    // 水面(P2)用。このシェーダーでは未使用だが、cbufferのレイアウトは宣言順で決まり
    // 途中のフィールドを飛ばせないため、末尾のPlanarReflectionPlaneのオフセットを
    // C++側 KurenaiEngine3D.cpp の FrameConstants と合わせる目的だけで宣言する
    // (DeferredLighting.hlsl/SSR.hlslの同名フィールドと同じ扱い。P9でSkyZenithTint/
    // SkyHorizonTint/SkyGroundTint/SkySunGlowTintの4本がSkyParametersBufferへ移り
    // FrameConstantsから消えたため、ここも同時に削ってフィールド数を合わせてある)
    float4 TimeParams;
    // 空の解析評価用(P3)・雲(P5)。大気遠近(P8)のin-scatter項(下記MakeSkyParameters/SkyColor)が
    // 読むため、このシェーダーでも実際に使う。xyz=太陽が「ある」向き(未正規化のまま渡ってくる。
    // MakeSkyParametersでnormalizeする)、w=未使用
    float4 SkySunDirection;
    // x=未使用、y=このシェーダーでは未使用(背景の解析評価トグルはDeferredLighting.hlsl専用)、
    // z=太陽照度/空照度比(SunToSkyIlluminanceRatio。MakeSkyParametersが読み、
    // Sky.hlsliのEvaluateCloudLayerが雲の明るさを太陽照度基準にするために使う)、w=未使用
    float4 SkyParams;
    // CloudParams0: x=被覆率(0で雲なし)、y=雲底の高度[m](カメラ基準)、
    //               z=UVスケール[ノイズ空間/m]、w=消散係数。MakeSkyParametersが読む
    float4 CloudParams0;
    // CloudParams1: xy=風によるノイズ空間の移動量(kCloudNoisePeriodでwrap済み)、
    //               z=Henyey-Greensteinの非対称パラメータ、w=未使用。MakeSkyParametersが読む
    float4 CloudParams1;
    // 巻雲(P11、さらに末尾に追加)。このシェーダーでは未使用だが、C++側 KurenaiEngine3D.cpp の
    // FrameConstants::CloudParams2/3 と揃える目的だけで宣言する
    // (DeferredLighting.hlsl/SSR.hlslの同名フィールドと同じ扱い)
    float4 CloudParams2;
    float4 CloudParams3;
    // 平面反射(P6)。xyz=水面平面の法線(現状は常に(0,1,0))、w=平面の距離項
    // (SV_ClipDistance0 = dot(worldPos, xyz) + w が水面より上で正になるように詰める)
    float4 PlanarReflectionPlane;
    // 大気遠近(P8、末尾に追加)。鏡像にも同じフォグを掛けるため、AerialPerspective.hlslと同じ値を読む。
    // x=基準高度での消散係数[1/m]、y=スケールハイト[m]、z=基準高度[m](ワールドY)、
    // w=有効フラグ(0で無効。C++側の判断はAerialPerspective.hlslのFogParams0.wコメント参照)
    float4 FogParams0;
    // x=不透明度の上限、yzw=未使用
    float4 FogParams1;
    // 水中項(P8)。このシェーダーでは未使用(オフセット合わせのためだけに宣言する)。Water.hlslが読む
    float4 WaterBodyColor;
};

// GBuffer.hlsl/Transparent.hlsl/ProbeCapture.hlslのObjectConstantsと同じレイアウト
cbuffer ObjectConstants : register(b1)
{
    float4x4 World;
    float4x4 NormalMatrix;
    float MetallicFactor;
    float RoughnessFactor;
    float TangentSignFlip;
    float AlphaCutoff;
    float3 EmissiveFactor;
    // glTFのocclusionTexture.strength(既定1.0)。GBuffer.hlslと同じ枠
    float OcclusionStrength;
    float4 BaseColorFactor;
};

// DirectLighting.hlsl側のstruct GPULightと並び・ストライド(64バイト)を一致させる必要がある
struct GPULight
{
    float4 PositionType;
    float4 ColorRange;
    float4 DirectionAngle;
    float4 Params;
};
StructuredBuffer<GPULight> Lights : register(t8);

Texture2D BaseColorTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D MetallicRoughnessTexture : register(t2);
Texture2D EmissiveTexture : register(t3);
// ベイク済みアンビエントオクルージョン(遮蔽マップ)。t4はカスケードシャドウマップ配列が
// 使っているためt5を使う(GBuffer.hlsl/Transparent.hlsl/ProbeCapture.hlslと共通)
Texture2D OcclusionTexture : register(t5);
// カスケードシャドウマップ(t4のTexture2DArray)とそのPCSSサンプリング。
// DirectLighting.hlsl/Transparent.hlsl/ProbeCapture.hlslと同じ実装を共有しているため、
// このパスに焼かれる影と本編の影が食い違うことはない。FrameConstants(CascadeViewProj/
// CascadeSplits/ShadowParams)とDataSamplerを参照するため、それらの宣言より後でインクルードする
#include "ShadowSampling.hlsli"
// スカイボックス由来のグローバルIBL。ProbeCaptureと同じく反射に映る面の環境光として使う
TextureCube IrradianceTexture : register(t9);
TextureCube PrefilteredEnvTexture : register(t10);
Texture2D BRDFLUTTexture : register(t11);
// DDGI(22章)の多重バウンス用。前フレームのイラディアンスを拡散の環境光として使う
#define KURENAI_DDGI_IRRADIANCE_REGISTER t12
#define KURENAI_DDGI_DISTANCE_REGISTER t13
#include "DDGI.hlsli"

// SkyIntegrate.hlslが書いた空パラメータ(P9)。大気遠近(P8)のin-scatter項(MakeSkyParameters/
// SkyColor)が読む。t0〜t13が既に使用済みのためt14を使う
StructuredBuffer<GPUSkyParameters> SkyParametersBuffer : register(t14);

// 多重バウンスの減衰。ProbeCapture.hlslと同じ値・同じ理由(1バウンスあたり5%のエネルギーを
// 捨てることで、反射率1の白い部屋でも等比級数が必ず収束するようにする)
static const float kDDGIBounceAttenuation = 0.95f;

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
    float4 Tangent : TANGENT;
    // ライトマップUV(Assets::Vertex::UV1)。遮蔽マップ専用(GBuffer.hlsl/ProbeCapture.hlslと同じ)
    float2 LightmapUV : TEXCOORD1;
};

struct PSInput
{
    float4 Position : SV_POSITION;
    float3 Normal : NORMAL;
    float3 WorldPos : TEXCOORD1;
    float2 UV : TEXCOORD0;
    float4 Tangent : TANGENT;
    float2 LightmapUV : TEXCOORD2;
    // 水面より下のジオメトリをラスタライズ前に落とす(ファイル冒頭参照)。正なら残す、負なら破棄される
    float ClipDistance : SV_ClipDistance0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float3 worldPos = mul(float4(input.Position, 1.0f), World).xyz;
    output.Position = mul(float4(worldPos, 1.0f), ViewProj);
    output.Normal = mul(input.Normal, (float3x3)NormalMatrix);
    output.WorldPos = worldPos;
    output.UV = input.UV;
    output.LightmapUV = input.LightmapUV;
    output.Tangent = float4(mul(input.Tangent.xyz, (float3x3)World), input.Tangent.w * TangentSignFlip);
    output.ClipDistance = dot(worldPos, PlanarReflectionPlane.xyz) + PlanarReflectionPlane.w;
    return output;
}

// FrameConstantsのSky*フィールドからSky.hlsliのSkyParametersを組み立てる。大気遠近(P8)の
// in-scatter項にだけ使う(このパス自体のライティングは従来どおりIBLキューブマップを使う。
// ファイル冒頭のEvaluateGlobalIBL参照)。
// SSR.hlsl/DeferredLighting.hlsl/AerialPerspective.hlslのMakeSkyParametersと完全に同一の内容で
// あること(正規化の扱いを含む)。4つのシェーダーはcbufferをそれぞれ別に宣言しているため
// 関数そのものは共有できず複製しているが、中身がずれると「背景の空」「水面に映る空」
// 「フォグの合成先の色」が互いに食い違ってしまうため、中身を変える場合は必ず4つとも同時に直すこと
SkyParameters MakeSkyParameters()
{
    SkyParameters params;
    params.SunDirection = normalize(SkySunDirection.xyz);
    params = ApplySkyParametersFromBuffer(params, SkyParametersBuffer[0]);
    // 太陽照度/空照度比(SkyParams.zに詰めてある。KurenaiEngine3D.cppのSkyParams.zコメント参照)。
    // EvaluateCloudLayerが雲の明るさを太陽照度基準にするために使う
    params.SunToSkyIlluminanceRatio = SkyParams.z;
    params.CloudCoverage = CloudParams0.x;
    params.CloudAltitude = CloudParams0.y;
    params.CloudUvScale = CloudParams0.z;
    params.CloudDensity = CloudParams0.w;
    params.CloudScrollOffset = CloudParams1.xy;
    params.CloudForwardG = CloudParams1.z;
    // 積雲の厚み[m](P13b)。CloudParams1.wは従来ずっと0で未使用だった枠なので、
    // FrameConstantsは1バイトも増えていない。0ならレイマーチせず従来の平面になる
    params.CloudThickness = CloudParams1.w;
    params.CirrusCoverage = CloudParams2.x;
    params.CirrusAltitude = CloudParams2.y;
    params.CirrusUvScale = CloudParams2.z;
    params.CirrusDensity = CloudParams2.w;
    params.CirrusScrollOffset = CloudParams3.xy;
    params.CirrusAnisotropy = CloudParams3.z;
    // 雲層へ掛ける大気遠近(P12。Sky.hlsliのEvaluateCloudLayer (f)節)。
    // 雲はAerialPerspective.hlslの早期脱出でフォグを受けないため、雲側で自前に掛ける
    params = ApplyCloudFogParameters(params, FogParams0, CameraPosition.y);
    return params;
}

// GBuffer.hlsl/ProbeCapture.hlslのComputeTangentFrameと同じ(ピクセル単位でGram-Schmidt再直交化する)
float3x3 ComputeTangentFrame(float3 N, float4 tangent)
{
    float3 T = normalize(tangent.xyz - N * dot(N, tangent.xyz));
    float3 B = cross(N, T) * tangent.w;
    return float3x3(T, B, N);
}

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * d * d, 1e-6f);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// DirectLighting.hlsl/ProbeCapture.hlslのEvaluateDirectBRDFと同じ(拡散+鏡面を足した1つの値を返す)
float3 EvaluateDirectBRDF(
    float3 N, float3 V, float3 L, float NdotV, float3 albedo, float metallic, float roughness,
    SpecularEnergyContext energy)
{
    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float D = DistributionGGX(NdotH, roughness);
    float G = GeometrySmith(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(VdotH, F0);

    float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, 1e-4f) * energy.Compensation;

    if (energy.Mode == KURENAI_SPEC_COMP_KULLACONTY)
    {
        // 加算ローブはE(NdotL)を要る(DirectLighting.hlslの同じ箇所と同一の処理)
        const float2 brdfL = BRDFLUTTexture.SampleLevel(ColorSampler, float2(NdotL, energy.Roughness), 0).rg;
        specular += SpecularMultiScatterLobe(F0, energy.EssV, brdfL.x + brdfL.y, energy.Eavg, energy.Mode);
    }

    float3 kd = (1.0f - F) * (1.0f - metallic);
    float3 diffuse = kd * albedo / PI;

    return (diffuse + specular) * NdotL;
}

float DistanceAttenuation(float distSq, float range)
{
    float factor = distSq / max(range * range, 1e-4f);
    float window = saturate(1.0f - factor * factor);
    return (window * window) / max(distSq, 0.0001f);
}

float SpotAttenuation(float3 spotDirection, float3 L, float angleScale, float angleOffset)
{
    float t = saturate(dot(spotDirection, -L) * angleScale + angleOffset);
    return t * t;
}

// DirectLighting.hlsl/ProbeCapture.hlslのEvaluateLightと同じ(影なし)
float3 EvaluateLight(
    GPULight light, float3 worldPos, float3 N, float3 V, float NdotV, float3 albedo, float metallic, float roughness,
    SpecularEnergyContext energy)
{
    uint lightType = (uint)light.PositionType.w;
    float range = light.ColorRange.w;

    float3 L;
    float atten = 1.0f;

    if (lightType == 0u)
    {
        L = normalize(-light.DirectionAngle.xyz);
    }
    else
    {
        float3 toLight = light.PositionType.xyz - worldPos;
        float distSq = dot(toLight, toLight);
        if (distSq > range * range)
        {
            return float3(0.0f, 0.0f, 0.0f);
        }

        atten = DistanceAttenuation(distSq, range);
        if (atten <= 0.0f)
        {
            return float3(0.0f, 0.0f, 0.0f);
        }

        L = toLight * rsqrt(max(distSq, 1e-8f));

        if (lightType == 2u)
        {
            float spotAtten = SpotAttenuation(light.DirectionAngle.xyz, L, light.DirectionAngle.w, light.Params.x);
            if (spotAtten <= 0.0f)
            {
                return float3(0.0f, 0.0f, 0.0f);
            }
            atten *= spotAtten;
        }
    }

    if (dot(N, L) <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    return EvaluateDirectBRDF(N, V, L, NdotV, albedo, metallic, roughness, energy) * light.ColorRange.rgb * atten;
}

// スカイボックス由来のグローバルIBL(ProbeCapture.hlslのEvaluateGlobalIBLと同一の式)。
// 焼いた絵とメインパスの絵が食い違わないよう、aoにはマテリアルの遮蔽マップを渡す
float3 EvaluateGlobalIBL(float3 N, float3 V, float3 worldPos, float3 albedo, float metallic, float roughness, float ao, float3 brdf, int compensationMode)
{
    const float NdotV = saturate(dot(N, V));
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    // 【多重バウンス(22章)】ProbeCapture.hlslのEvaluateGlobalIBLと同じ理由・同じ式。
    // DDGIが有効なら拡散の環境光を前フレームのDDGIイラディアンスにすることで、
    // このパスに映る反射にも多重バウンスの間接光が反映される。
    // 【M11 Stage 1】鏡映カメラが映す点はDDGIボリュームの外にあることがある(このシーンは
    // 干潟が6,000m四方でボリュームより遥かに広い)。insideWeightが0の点ではグローバルIBLの
    // ままにする——ProbeCapture.hlsl・DeferredLighting.hlslとまったく同じ規則
    float3 irradiance = IrradianceTexture.Sample(MaterialSampler, N).rgb;
    if (DDGIParams0.w > 0.5f)
    {
        float ddgiInsideWeight;
        const float3 ddgiIrradiance =
            SampleDDGIIrradiance(worldPos, N, V, ddgiInsideWeight) * kDDGIBounceAttenuation;
        irradiance = lerp(irradiance, ddgiIrradiance, ddgiInsideWeight);
    }
    const float3 fresnelRoughness =
        F0 + (max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0) - F0) * pow(saturate(1.0f - NdotV), 5.0f);
    const float3 kd = (1.0f - fresnelRoughness) * (1.0f - metallic);
    const float3 diffuseIBL = kd * albedo * irradiance * ao;

    const float3 R = reflect(-V, N);
    const float mipLevel = roughness * ShadowParams.y;
    const float3 prefiltered = PrefilteredEnvTexture.SampleLevel(MaterialSampler, R, mipLevel).rgb;
    const float3 FssEss = F0 * brdf.x + brdf.y;
    const float Ess = brdf.x + brdf.y;
    const float3 specularIBL =
        (prefiltered * FssEss * SpecularEnergyCompensation(F0, brdf, compensationMode)
         + SpecularMultiScatterIBL(F0, FssEss, Ess, compensationMode) * irradiance)
        * SpecularOcclusion(NdotV, roughness, ao);

    return diffuseIBL + specularIBL;
}

// このパスのレンダーターゲットは1枚だけ(放射輝度)。ProbeCaptureと違い視差補正用の距離は要らない
// (平面反射は「同じ画面UVで引ける」ため、SSR側は視差補正を必要としない。ファイル冒頭参照)
float4 PSMain(PSInput input) : SV_TARGET
{
    float4 baseColorSample = BaseColorTexture.Sample(MaterialSampler, input.UV) * BaseColorFactor;

    // 不透明パスと同じアルファカットアウト(葉・フェンス等を反射でも正しく抜く)
    clip(baseColorSample.a - AlphaCutoff);

    float3 geometricNormal = normalize(input.Normal);
    float2 normalXY = NormalTexture.Sample(MaterialSampler, input.UV).xy * 2.0f - 1.0f;
    float normalZ = sqrt(saturate(1.0f - dot(normalXY, normalXY)));
    float3 normalSample = float3(normalXY, normalZ);
    float3x3 tbn = ComputeTangentFrame(geometricNormal, input.Tangent);
    float3 N = normalize(mul(normalSample, tbn));

    float3 metallicRoughnessSample = MetallicRoughnessTexture.Sample(MaterialSampler, input.UV).rgb;
    float metallic = saturate(MetallicFactor * metallicRoughnessSample.b);
    // RoughnessFactorが負の場合はソースデータにラフネス係数が無かったことを表す
    // (Assets::kInvalidMaterialFactor)。GBuffer.hlsl/ProbeCapture.hlslと同じく係数1.0として扱う
    float roughnessFactor = (RoughnessFactor < 0.0f) ? 1.0f : RoughnessFactor;
    float roughness = clamp(roughnessFactor * metallicRoughnessSample.g, 0.045f, 1.0f);

    float3 emissive = EmissiveTexture.Sample(MaterialSampler, input.UV).rgb * EmissiveFactor;

    // マテリアルの遮蔽マップ(ベイク済みAO)。GBuffer.hlsl/ProbeCapture.hlslと同じ解釈・同じstrength適用
    float occlusionSample = OcclusionTexture.Sample(MaterialSampler, input.LightmapUV).r;
    float materialAO = lerp(1.0f, occlusionSample, OcclusionStrength);

    float3 albedo = baseColorSample.rgb;
    // CameraPositionには鏡映したカメラ位置が入っている(ファイル冒頭参照)
    float3 V = normalize(CameraPosition.xyz - input.WorldPos);
    float NdotV = saturate(dot(N, V)) + 1e-5f;

    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    // LUTの第3成分(Eavg)はKulla-Conty方式だけが使う(14.9.2.1節)
    const float3 brdf = BRDFLUTTexture.Sample(ColorSampler, float2(NdotV, roughness)).rgb;
    const SpecularEnergyContext energy = MakeSpecularEnergyContext(F0, brdf, roughness, ShadowParams.w);

    float3 color = float3(0.0f, 0.0f, 0.0f);

    // --- 太陽(b0、カスケードシャドウ付き) ---
    float3 sunL = normalize(-LightDirection.xyz);
    float sunNdotL = saturate(dot(N, sunL));
    if (sunNdotL > 0.0f)
    {
        // カスケード選択はカメラ視錐台基準(FrameConstants.Viewはカメラのビュー行列。ファイル冒頭参照)
        float viewDepth = mul(float4(input.WorldPos, 1.0f), View).z;
        float shadow = ComputeCascadedShadowFactor(input.WorldPos, viewDepth, sunNdotL);
        color += EvaluateDirectBRDF(N, V, sunL, NdotV, albedo, metallic, roughness, energy) * LightColor.rgb * shadow;
    }

    // --- t8のライトリスト(影なし) ---
    uint lightCount = (uint)ActiveLightCount.x;
    [loop]
    for (uint i = 0; i < lightCount; ++i)
    {
        color += EvaluateLight(Lights[i], input.WorldPos, N, V, NdotV, albedo, metallic, roughness, energy);
    }

    // --- スカイボックス由来のグローバルIBL(環境光) ---
    // ShadowParams.z = IBL強度倍率(Enable IBL無効なら0)。無効時はDeferredLighting.hlsl/
    // ProbeCapture.hlslと同じ定数色アンビエントへフォールバックし、反射が真っ黒になるのを防ぐ
    if (ShadowParams.z > 0.0f)
    {
        color += EvaluateGlobalIBL(N, V, input.WorldPos, albedo, metallic, roughness, materialAO, brdf, energy.Mode) * ShadowParams.z;
    }
    else
    {
        // AmbientColor.rgbは一様な環境のイラディアンスE相当なので、その環境の放射輝度はL = E / PI
        const float3 ambientRadiance = AmbientColor.rgb / PI;

        // 鏡面項を落とすと金属(拡散項が0)が真っ黒に映り込むため必ず計算する
        // (ProbeCapture.hlslの同じ分岐と同じ理由)
        const float3 fallbackFssEss = F0 * brdf.x + brdf.y;
        const float fallbackEss = brdf.x + brdf.y;

        color += albedo * (1.0f - metallic) * ambientRadiance * materialAO
            + ambientRadiance
                * (fallbackFssEss * energy.Compensation
                   + SpecularMultiScatterIBL(F0, fallbackFssEss, fallbackEss, energy.Mode))
                * SpecularOcclusion(NdotV, roughness, materialAO);
    }

    color += emissive;

    // 大気遠近(P8)を鏡像にも適用する。掛けないと「霞んだ本体 vs くっきりした鏡像」になってしまう
    // (AerialPerspective.hlslは本編のSceneColorにしかフォグを掛けないため、この平面反射パスの
    // 出力は素通しだと霞まないまま合成されてしまう)。
    // 【経路長にCameraPositionをそのまま使ってよい理由】光は「物体→水面→目」の順に進むが、
    // その経路長は鏡映カメラ(CameraPosition、ファイル冒頭参照)から物体までの距離と
    // 幾何学的に厳密に等しい(鏡映は「鏡映カメラで景色を撮り直す」ことと数学的に等価なため。
    // ファイル冒頭の【カメラを鏡映して描くことの意味】と同じ理屈)
    if (FogParams0.w > 0.5f)
    {
        const float transmittance =
            HeightFogTransmittance(CameraPosition.xyz, input.WorldPos, FogParams0.x, FogParams0.y, FogParams0.z);
        const float alpha = saturate(1.0f - transmittance) * saturate(FogParams1.x);

        // in-scatterの方向はAerialPerspective.hlslと同じく視線方向(カメラ→着目点)のyを
        // 0以上へクランプしたものを使う。
        // 【鏡映カメラからの方向をそのまま使ってよい理由】水面をy=0の平面、実カメラをC、
        // 鏡映カメラをC'、着目点をO、実際の反射点をPとすると、
        //   O - C' = (Ox-Cx, Oy+Cy, Oz-Cz)
        //   O - P  = Oy/(Cy+Oy) * (Ox-Cx, Cy+Oy, Oz-Cz)
        // となり、2つは正のスカラー倍の関係で向きが厳密に一致する。つまりfogDirは
        // 「水面から物体へ向かう実際の光路の向き」そのものであって近似ではない
        // (光路のうち「目→水面」の短い区間だけは無視している。経路長は上のコメントのとおり
        // 鏡映カメラからの距離が全区間ぶんを正しく含む)
        const float3 viewDir = normalize(input.WorldPos - CameraPosition.xyz);
        const float3 clampedDir = float3(viewDir.x, max(viewDir.y, 0.0f), viewDir.z);
        const float clampedLength = length(clampedDir);
        // 零ベクトルのnormalizeによるNaNを避ける(理由と退避先の選び方はAerialPerspective.hlsl参照)
        const float3 fogDir =
            (clampedLength > 1e-5f) ? (clampedDir / clampedLength) : float3(0.0f, 0.0f, 1.0f);

        // Mie位相関数を掛けない理由、および雲を含むSkyColorではなく晴天のSkyColorUpperを使う
        // 理由(雲は高度1,000m以上にあり、カメラと着目点の間の空気には存在しないため、
        // in-scatterに乗せると雲の模様が地物へ透けて焼き付く)はAerialPerspective.hlslに
        // 詳しく書いてある。2つのパスで霞の色が食い違わないよう、必ず同時に直すこと
        const float3 inScatter = SkyColorUpper(fogDir, MakeSkyParameters());

        color = color * (1.0f - alpha) + inScatter * alpha;
    }

    // 事前乗算済みアルファの規約(a=1)は崩さない。フォグを掛けた色をrgbに、aは1のまま返す
    return float4(color, 1.0f);
}
