// 半透明フォワードパス。glTFのalphaMode=BLENDのメッシュだけを、G-Bufferには一切書き込まず
// 直接SceneColorへフォワードシェーディングする。Deferredパス(GBuffer.hlsl)と違い、この1枚の
// シェーダーの中でPBR直接光(太陽+ポイント/スポットライト、シャドウ適用済み)と簡易環境光までを
// 完結させて出力する(SSAO/SSIL/SSRのようなスクリーンスペース手法は非対応。既知の制約は
// docs/Architecture.htmlの「半透明描画(フォワードパス)」章を参照)。
//
// ライティングのPBR計算はDirectLighting.hlslと同じ式を使う(このシェーダーは1メッシュぶんずつ
// 描画するフォワードパスのため、フルスクリーンパスのDirectLighting.hlslとは呼び出し形態が異なり、
// PSMainの構造に依存する部分は#includeで共有できず複製している)。
// ただし以下はリソースにも呼び出し形態にも依存しないため、共有ヘッダーへ切り出している:
//   - Smith可視性項とスペキュラのエネルギー補正(SpecularEnergy.hlsli)。BRDF積分LUTの生成と
//     必ず一致していなければならないため
//   - PCSS(Percentage Closer Soft Shadows)によるカスケードシャドウのサンプリング
//     (ShadowSampling.hlsli)。以前はここに複製していたが、片方だけ直すと半透明と不透明で
//     影が食い違う事故が起きやすかったため統合した
//
// DX12のルートシグネチャがCBVをb0/b1の2枠しか持たないため、GBuffer.hlslと同じくb1に
// ObjectConstants(モデル行列)を置く。そのためDirectLighting.hlsl側のb1(LightingConstants、
// 有効ライト数)をここでは使えず、有効ライト数はFrameConstants末尾のActiveLightCountで受け取る

// Smith可視性項とスペキュラのエネルギー補正。DirectLighting.hlslのPBR計算を複製している
// このシェーダーでも、BRDF積分LUT(BRDFLUT.hlsl)と同じ可視性項を使う必要があるため共有する
#include "SpecularEnergy.hlsli"

static const float PI = 3.14159265359f;

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
    // 半透明パス専用。x=t8のライトリストの有効数(DirectLighting.hlsl側のLightingConstants.LightCount.xと
    // 同じ値)。他のシェーダーはこのフィールドを宣言していないため、末尾に追加してもオフセットは変わらない
    float4 ActiveLightCount;
    // x: 拡散イラディアンスの取得元(0=専用イラディアンスマップ(t9)、1=プリフィルタ済み鏡面の
    // 最終ミップ)。EvaluateIBLSplit参照。yzwは未使用
    float4 IBLParams;
};

// GBuffer.hlslのObjectConstantsと同じレイアウト(AlphaCutoffはBLENDマテリアルでは常に0で
// 実質未使用だが、同じルートシグネチャ/定数バッファを共有するため並び順を合わせる)。
// 末尾のBaseColorFactorはこのシェーダーのみが使う(GBuffer.hlsl/Shadow.hlslは宣言していない)
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
    // glTFのbaseColorFactor(既定[1,1,1,1])。BaseColorTextureと乗算する。テクスチャを持たず
    // baseColorFactorのみで色/不透明度を表現するマテリアル(ガラス等)を正しく再現するために使う
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
// 使っているためt5を使う(GBuffer.hlsl/ProbeCapture.hlslと共通)
Texture2D OcclusionTexture : register(t5);
// カスケードシャドウマップ(t4のTexture2DArray)とそのPCSSサンプリング。
// DirectLighting.hlslと同じ実装を共有しているため、半透明と不透明で影がずれることはない。
// FrameConstants(CascadeViewProj/CascadeSplits/ShadowParams)とDataSamplerを参照するため、
// それらの宣言より後でインクルードする必要がある
#include "ShadowSampling.hlsli"
// IBL(14章)。DeferredLighting.hlslと同じ3枚をこのパスにもバインドする。半透明パスにはSSRが
// 適用されないため、ガラスにとってはこのIBLが唯一の環境の映り込みになる
TextureCube IrradianceTexture : register(t9);
TextureCube PrefilteredEnvTexture : register(t10);
Texture2D BRDFLUTTexture : register(t11);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
    float4 Tangent : TANGENT;
    // ライトマップUV(Assets::Vertex::UV1)。遮蔽マップ専用(GBuffer.hlslと同じ)
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
    return output;
}

// GBuffer.hlslのComputeTangentFrameと同じ(ピクセル単位でGram-Schmidt再直交化する)
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

// GeometrySchlickGGX / GeometrySmith はSpecularEnergy.hlsliの共有定義を使う
// (以前ここにあったDisneyのラフネス再マップ k=(roughness+1)^2/8 は除去した。理由は同ヘッダー参照)

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// 拡散反射項と鏡面反射項を分けて返す(DirectLighting.hlslは両者を足した1つの値を返すが、
// このシェーダーは事前乗算済みアルファ出力のために両者を別々に積算する必要がある。
// 鏡面反射は不透明度で減衰させず背景の上へ加算するため。PSMain末尾のコメント参照)
void EvaluateDirectBRDF(
    float3 N, float3 V, float3 L, float NdotV, float3 albedo, float metallic, float roughness,
    SpecularEnergyContext energy,
    out float3 outDiffuse, out float3 outSpecular)
{
    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float D = DistributionGGX(NdotH, roughness);
    float G = GeometrySmith(NdotV, NdotL, roughness);
    float3 F = FresnelSchlick(VdotH, F0);

    // energyはPSMainで1度だけ計算して渡される(SpecularEnergy.hlsli、14.9節)。
    // このシェーダーは拡散/鏡面を別々に返すため、補正が鏡面側にだけ掛かることがコード上で自明になる
    // (拡散項kdは変更しない。理由は14.9節)
    float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, 1e-4f) * energy.Compensation;

    if (energy.Mode == KURENAI_SPEC_COMP_KULLACONTY)
    {
        // 加算ローブはE(NdotL)を要る。ライトのループ内から呼ばれるため勾配に依存しない
        // SampleLevelを使う(DirectLighting.hlslの同じ箇所と同一の処理)
        const float2 brdfL = BRDFLUTTexture.SampleLevel(ColorSampler, float2(NdotL, energy.Roughness), 0).rg;
        specular += SpecularMultiScatterLobe(F0, energy.EssV, brdfL.x + brdfL.y, energy.Eavg, energy.Mode);
    }

    float3 kd = (1.0f - F) * (1.0f - metallic);
    float3 diffuse = kd * albedo / PI;

    outDiffuse = diffuse * NdotL;
    outSpecular = specular * NdotL;
}

// DeferredLighting.hlslのEvaluateIBLと同じ式(split-sum近似、Karis 2013)。ただし事前乗算済み
// アルファ出力(PSMain末尾のコメント参照)のために拡散項と鏡面項を分けて返す。
// 半透明パスはスクリーンスペースのAO/GIバッファ(SSAO/SSIL)を持たないが、マテリアルの
// 遮蔽マップ(ベイク済みAO)はテクスチャなのでこのパスでも使える。aoにはそれを渡す
// (遮蔽マップを持たないマテリアルは白1x1でao=1となり、従来と同じ結果になる)
void EvaluateIBLSplit(
    float3 N, float3 V, float3 albedo, float metallic, float roughness, float ao,
    out float3 outDiffuse, out float3 outSpecular)
{
    const float NdotV = saturate(dot(N, V));
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    // --- 拡散IBL ---
    // 既定はプリフィルタ済み鏡面の最終ミップ(roughness=1)。IBLParams.x=1のときだけ従来の
    // 専用イラディアンスマップを引く(検証用に残している経路)。詳細な根拠は
    // DeferredLighting.hlslのEvaluateIBLの同じ箇所を参照(14.10節)。半透明だけ取得元が
    // 食い違わないよう、必ず不透明側と同じ切り替えを行う
    const float3 irradiance = (IBLParams.x > 0.5f)
        ? IrradianceTexture.Sample(MaterialSampler, N).rgb
        : PrefilteredEnvTexture.SampleLevel(MaterialSampler, N, ShadowParams.y).rgb;
    // ラフネスを考慮したFresnel-Schlick(Lagarde, "Moving Frostbite to PBR")
    const float3 fresnelRoughness =
        F0 + (max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0) - F0) * pow(saturate(1.0f - NdotV), 5.0f);
    const float3 kd = (1.0f - fresnelRoughness) * (1.0f - metallic);

    // --- 鏡面IBL(split-sum近似) ---
    const float3 R = reflect(-V, N);
    // ShadowParams.y = プリフィルタ済み鏡面マップの最大ミップレベル(KurenaiEngine3D側で設定)
    const float mipLevel = roughness * ShadowParams.y;
    const float3 prefiltered = PrefilteredEnvTexture.SampleLevel(MaterialSampler, R, mipLevel).rgb;
    const float3 brdf = BRDFLUTTexture.Sample(ColorSampler, float2(NdotV, roughness)).rgb;

    // マルチスキャッタリング・エネルギー補正はDeferredLighting.hlslのEvaluateIBLと同じ形。
    // 乗算型(モード1・2)は単一散乱項へ倍率として掛かり、Kulla-Conty(3)は別ローブを加算する
    const int compensationMode = (int)(ShadowParams.w + 0.5f);
    const float3 FssEss = F0 * brdf.x + brdf.y;
    const float Ess = brdf.x + brdf.y;

    // 昼度による減衰はしない(DeferredLighting.hlsl の EvaluateIBL と同じ理由)。
    // 手続き空が太陽高度に応じて自分で暗くなるため、ここで掛けると二重に暗くなる。
    // 半透明パスはスクリーンスペースのAO/GIバッファを持たないが、マテリアルの遮蔽マップは
    // テクスチャなので使える。aoにはそれが入っている(遮蔽マップが無ければ1)
    outDiffuse = kd * albedo * irradiance * ao;
    outSpecular = (prefiltered * FssEss * SpecularEnergyCompensation(F0, brdf, compensationMode)
        + SpecularMultiScatterIBL(F0, FssEss, Ess, compensationMode) * irradiance)
        * SpecularOcclusion(NdotV, roughness, ao);
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

void EvaluateLight(
    GPULight light, float3 worldPos, float3 N, float3 V, float NdotV, float3 albedo, float metallic, float roughness,
    SpecularEnergyContext energy,
    out float3 outDiffuse, out float3 outSpecular)
{
    outDiffuse = float3(0.0f, 0.0f, 0.0f);
    outSpecular = float3(0.0f, 0.0f, 0.0f);

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
            return;
        }

        atten = DistanceAttenuation(distSq, range);
        if (atten <= 0.0f)
        {
            return;
        }

        L = toLight * rsqrt(max(distSq, 1e-8f));

        if (lightType == 2u)
        {
            float spotAtten = SpotAttenuation(light.DirectionAngle.xyz, L, light.DirectionAngle.w, light.Params.x);
            if (spotAtten <= 0.0f)
            {
                return;
            }
            atten *= spotAtten;
        }
    }

    if (dot(N, L) <= 0.0f)
    {
        return;
    }

    float3 diffuse;
    float3 specular;
    EvaluateDirectBRDF(N, V, L, NdotV, albedo, metallic, roughness, energy, diffuse, specular);

    float3 radiance = light.ColorRange.rgb * atten;
    outDiffuse = diffuse * radiance;
    outSpecular = specular * radiance;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    // glTF仕様どおりbaseColorTexture.rgba * baseColorFactorで最終的なベースカラー/アルファを求める。
    // テクスチャを持たないマテリアル(BaseColorTexture=白1x1プレースホルダー、alpha=1)は
    // BaseColorFactorだけで色・不透明度が決まる
    float4 baseColorSample = BaseColorTexture.Sample(MaterialSampler, input.UV) * BaseColorFactor;

    float3 geometricNormal = normalize(input.Normal);
    float2 normalXY = NormalTexture.Sample(MaterialSampler, input.UV).xy * 2.0f - 1.0f;
    float normalZ = sqrt(saturate(1.0f - dot(normalXY, normalXY)));
    float3 normalSample = float3(normalXY, normalZ);
    float3x3 tbn = ComputeTangentFrame(geometricNormal, input.Tangent);
    float3 N = normalize(mul(normalSample, tbn));

    float3 metallicRoughnessSample = MetallicRoughnessTexture.Sample(MaterialSampler, input.UV).rgb;
    float metallic = saturate(MetallicFactor * metallicRoughnessSample.b);
    // RoughnessFactorが負の場合はソースデータにラフネス係数が無かったことを表す
    // (Assets::kInvalidMaterialFactor)。パッカーが勝手な既定値を埋めない方針のため、
    // ここで係数1.0=テクスチャの値をそのまま使う、と解釈する
    float roughnessFactor = (RoughnessFactor < 0.0f) ? 1.0f : RoughnessFactor;
    float roughness = clamp(roughnessFactor * metallicRoughnessSample.g, 0.045f, 1.0f);

    float3 emissive = EmissiveTexture.Sample(MaterialSampler, input.UV).rgb * EmissiveFactor;

    // マテリアルの遮蔽マップ(ベイク済みAO)。GBuffer.hlslと同じ解釈・同じstrength適用を行う。
    // 引くUVは専用のライトマップUV(TEXCOORD1)。理由はGBuffer.hlslの同じ箇所を参照
    float occlusionSample = OcclusionTexture.Sample(MaterialSampler, input.LightmapUV).r;
    float materialAO = lerp(1.0f, occlusionSample, OcclusionStrength);

    float3 albedo = baseColorSample.rgb;
    float3 V = normalize(CameraPosition.xyz - input.WorldPos);
    float NdotV = saturate(dot(N, V)) + 1e-5f;

    // スペキュラのエネルギー補正(SpecularEnergy.hlsli、14.9節)。Ess=(NdotV, ラフネス)だけの
    // 関数でピクセル内では一定なので、ライトのループへ入る前に1度だけ求める。
    // 下のIBL無効時フォールバックもこのF0/brdfをそのまま再利用する
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    const float3 brdf = BRDFLUTTexture.Sample(ColorSampler, float2(NdotV, roughness)).rgb;
    const SpecularEnergyContext energy = MakeSpecularEnergyContext(F0, brdf, roughness, ShadowParams.w);

    float3 directDiffuse = float3(0.0f, 0.0f, 0.0f);
    float3 directSpecular = float3(0.0f, 0.0f, 0.0f);

    // --- 太陽(b0、カスケードシャドウ付き) ---
    float3 sunL = normalize(-LightDirection.xyz);
    float sunNdotL = saturate(dot(N, sunL));
    if (sunNdotL > 0.0f)
    {
        float viewDepth = mul(float4(input.WorldPos, 1.0f), View).z;
        float shadow = ComputeCascadedShadowFactor(input.WorldPos, viewDepth, sunNdotL);

        float3 sunDiffuse;
        float3 sunSpecular;
        EvaluateDirectBRDF(N, V, sunL, NdotV, albedo, metallic, roughness, energy, sunDiffuse, sunSpecular);

        float3 sunRadiance = LightColor.rgb * shadow;
        directDiffuse += sunDiffuse * sunRadiance;
        directSpecular += sunSpecular * sunRadiance;
    }

    // --- t8のライトリスト(影なし。DirectLighting.hlslと同じ仕様) ---
    uint lightCount = (uint)ActiveLightCount.x;
    [loop]
    for (uint i = 0; i < lightCount; ++i)
    {
        float3 lightDiffuse;
        float3 lightSpecular;
        EvaluateLight(Lights[i], input.WorldPos, N, V, NdotV, albedo, metallic, roughness, energy, lightDiffuse, lightSpecular);
        directDiffuse += lightDiffuse;
        directSpecular += lightSpecular;
    }

    // 環境光。半透明パスにはSSRが適用されないため、ガラスにとってはこのIBLが唯一の
    // 「環境の映り込み」になる。デルタ光源(太陽・ポイント/スポット)のスペキュラだけでは、
    // 低ラフネスのガラスは正反射条件を満たす極めて狭い帯にしかハイライトが出ず、
    // 透明なだけの面に見えてしまう。
    // ShadowParams.z = IBL強度倍率(Enable IBL無効なら0)。無効時はDeferredLighting.hlslと同じく
    // IBL導入以前の定数色アンビエント(拡散のみ)へフォールバックする。
    // SSAO/SSILによる遮蔽・間接拡散光は非対応(常にao=1・間接光=0として扱う既知の制約)。
    // ただしマテリアルの遮蔽マップ(ベイク済みAO)はテクスチャなのでこのパスでも効く
    float3 ambientDiffuse;
    float3 ambientSpecular;
    if (ShadowParams.z > 0.0f)
    {
        EvaluateIBLSplit(N, V, albedo, metallic, roughness, materialAO, ambientDiffuse, ambientSpecular);
        ambientDiffuse *= ShadowParams.z;
        ambientSpecular *= ShadowParams.z;
    }
    else
    {
        // IBL無効時のフォールバック。拡散はIBL導入以前と同じ定数色アンビエント、鏡面も同じ定数色を
        // 「方向依存を持たない一様な環境radiance」とみなし、split-sum近似の第2項(BRDF積分LUT。
        // 方向性を持たない(NdotV, ラフネス)のテーブルなのでIBLの有効/無効に関わらず使える)を掛ける。
        // 鏡面項を0にしてしまうと、金属(拡散項が0になる)が環境光の下で真っ黒になり、
        // 低ラフネスのガラスもハイライトを完全に失うため、必ず計算する
        // F0とbrdfはPSMain冒頭でエネルギー補正用に既に求めてあるため再サンプルしない。
        // 定数色アンビエントはプリフィルタ済み鏡面・拡散イラディアンスの両方の代わりを兼ねるため、
        // Kulla-Contyの加算ローブにも同じAmbientColor.rgbを掛ける。
        // 遮蔽マップはIBLの有無に関わらず環境光に効かせる(IBL有効時のEvaluateIBLSplitと同じ扱い)
        const float3 fallbackFssEss = F0 * brdf.x + brdf.y;
        const float fallbackEss = brdf.x + brdf.y;
        ambientDiffuse = albedo * (1.0f - metallic) * AmbientColor.rgb * materialAO;
        ambientSpecular = AmbientColor.rgb
            * (fallbackFssEss * energy.Compensation
               + SpecularMultiScatterIBL(F0, fallbackFssEss, fallbackEss, energy.Mode))
            * SpecularOcclusion(NdotV, roughness, materialAO);
    }

    // 環境光の拡散・鏡面倍率。IBLの有効/無効どちらの経路にも同じように効かせたいので、
    // 分岐の中ではなく合流した後で1か所だけ掛ける。
    // 不透明パスでは同じ倍率がDeferredLighting.hlslとReflectionProbe.hlsliのSpecularIBLWeightに
    // 入っており、半透明の鏡面がEvaluateIBLSplitで別計算になっている以上ここに書くしかない。
    // 【ずらすと同じマテリアルが不透明と半透明で違う明るさになる】必ず両方を同時に直すこと
    ambientDiffuse *= IBLParams.y;
    ambientSpecular *= IBLParams.z;

    // 事前乗算済みアルファ(BlendMode::PremultipliedAlpha)で出力する。
    // 合成結果は out = src.rgb + dst.rgb * (1 - src.a) となるため、
    //   ・拡散光/環境光: 面が背景を覆う割合ぶんだけ寄与するので不透明度alphaを乗じる
    //   ・鏡面反射・自発光: 面が「反射・放射して足す」光であり背景を遮る量とは無関係なので減衰させない
    // という区別をここで付けられる。標準アルファブレンド(src.rgb * src.a + dst.rgb * (1 - src.a))は
    // 出力色全体にalphaを掛けてしまうため、Bistroの酒瓶ガラス(MTLのTf由来でalpha=0.04)のような
    // ほぼ無色透明のマテリアルではハイライトまで1/25に潰れ、ガラスが「透明」ではなく
    // 「何も無い」ように見えてしまっていた
    float alpha = saturate(baseColorSample.a);
    float3 premultipliedColor =
        (ambientDiffuse + directDiffuse) * alpha + directSpecular + ambientSpecular + emissive;

    return float4(premultipliedColor, alpha);
}
