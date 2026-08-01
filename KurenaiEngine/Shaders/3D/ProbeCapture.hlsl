// 反射プローブのキャプチャパス(フォワード)。プローブ位置から6方向を1面ずつ2Dレンダーターゲットへ
// 描画し、その結果をIBLConvolve.hlslのCSCopyCaptureToCubeFaceがキューブマップの該当面へ書き写す。
// レンダーターゲットは2枚(放射輝度と距離)。詳細はPSOutputのコメント参照。
// キューブマップへ直接描画(面ごとのRTV)はRHIが持っていないため、この「2Dへ描いてUAVでコピー」
// という経路を採っている(既に実績のある面ごとUAV書き込みの仕組みをそのまま再利用できる)。
//
// ライティングはTransparent.hlsl(半透明フォワードパス)と同じ式を使う。プローブに映るのは
// 「直接光 + スカイボックス由来のグローバルIBL」までで、SSAO/SSIL/SSRのスクリーンスペース手法や
// 他のプローブの寄与は含まない(含めるとプローブ同士が相互参照して発散するため、
// 反射の中の反射は1バウンスで打ち切るのが定石)。
//
// 【定数バッファの与え方】b0はFrameConstantsをそのまま使うが、エンジン側(KurenaiEngine3D::Render)は
// このパス専用のバッファへ次の値を詰めて渡す:
//   ViewProj       … プローブのその面のビュー・プロジェクション(ラスタライズに使う)
//   View           … 「カメラ」のビュー行列。カスケード選択の深度(CascadeSplits)がカメラ視錐台
//                     基準で求められているため、ここだけはプローブではなくカメラのものを渡す
//   CameraPosition … プローブのワールド座標(視線ベクトルVの起点。プローブから見た放射輝度を
//                     捉えるのが目的なので実際のカメラ位置ではない)
// これによりシェーダー側はFrameConstantsの宣言を一切変えずに済む。
//
// 既知の制約: カスケードシャドウマップはカメラ視錐台に合わせて分割・フィットされているため、
// カメラから遠く離れた位置のプローブを焼くとシャドウマップの範囲外になり影が落ちない
// (ComputeShadowFactorが範囲外を「影なし」として返す)。プローブは基本的に視界内で焼く前提とする。
#include "SpecularEnergy.hlsli"
#include "Samplers.hlsli"

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
    // x=t8のライトリストの有効数(Transparent.hlslと同じくFrameConstants末尾で受け取る。
    // b1はObjectConstantsが占有していてLightingConstantsを置けないため)
    float4 ActiveLightCount;
    // ここから下はこのシェーダーでは使わないが、cbufferのレイアウトは宣言順で決まり
    // 途中のフィールドを飛ばせないため、後続のDDGIParams/OcclusionParamsのオフセットを合わせる目的で
    // 宣言する(C++側 KurenaiEngine3D.cpp の FrameConstants と並びを一致させること)
    float4 IBLParams;
    float4 ProbeParams;
    float4 ProbeParams2;
    // TAA(23章)用。このシェーダーでは未使用だが、C++側でDDGIParamsより手前に置かれているため
    // オフセット合わせのためだけに宣言する
    float4x4 PrevViewProj;
    float4 TAAParams;
    // DDGI(22章)。多重バウンスのために前フレームのイラディアンスを引くのに使う
    float4 DDGIParams0;
    float4 DDGIParams1;
    float4 DDGIParams2;
    float4 DDGIParams3;
    // x=このフレームの実効プリ露出(アトラスは露出非依存で持つため読み出し時に掛け戻す)
    float4 DDGIParams4;
    // bent normalによる遮蔽(25章)。プローブの中身も不透明パスと同じ規則で焼かないと、
    // つまみを動かしたときにプローブだけ古い見た目のまま残る。
    // KurenaiEngine3D側の再ベイク署名にもこの値を混ぜてあること
    float4 OcclusionParams;
};

// GBuffer.hlsl/Transparent.hlslのObjectConstantsと同じレイアウト
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
// 使っているためt5を使う(GBuffer.hlsl/Transparent.hlslと共通)
Texture2D OcclusionTexture : register(t5);
// bent normal(遮蔽マップと同じライトマップUV空間)。GBuffer.hlslと同じくt6(25章)
Texture2D BentNormalTexture : register(t6);
// カスケードシャドウマップ(t4のTexture2DArray)とそのPCSSサンプリング。
// DirectLighting.hlsl/Transparent.hlslと同じ実装を共有しているため、プローブに焼かれる影と
// 本編の影が食い違うことはない。FrameConstants(CascadeViewProj/CascadeSplits/ShadowParams)と
// DataSamplerを参照するため、それらの宣言より後でインクルードする必要がある
#include "ShadowSampling.hlsli"
// スカイボックス由来のグローバルIBL。プローブに映る面の環境光として使う
TextureCube IrradianceTexture : register(t9);
TextureCube PrefilteredEnvTexture : register(t10);
Texture2D BRDFLUTTexture : register(t11);
// DDGI(22章)の多重バウンス用。前フレームのイラディアンスを拡散の環境光として使う
#define KURENAI_DDGI_IRRADIANCE_REGISTER t12
#define KURENAI_DDGI_DISTANCE_REGISTER t13
#include "DDGI.hlsli"

// 多重バウンスの減衰。1未満でなければならない(理由はEvaluateGlobalIBLのコメント参照)。
// 0.95は「1バウンスあたり5%のエネルギーを捨てる」という意味で、反射率1の白い部屋でも
// 等比級数 1 + 0.95 + 0.95² + ... = 20 で必ず収束する
static const float kDDGIBounceAttenuation = 0.95f;

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

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// DirectLighting.hlslのEvaluateDirectBRDFと同じ(拡散+鏡面を足した1つの値を返す)
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

// DirectLighting.hlslのEvaluateLightと同じ(影なし)
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

// スカイボックス由来のグローバルIBL(DeferredLighting.hlslのEvaluateIBLと同じ式。
// キャプチャ時にはスクリーンスペースのAO/GIバッファが無いが、マテリアルの遮蔽マップは
// テクスチャなので使える。焼いた絵とメインパスの絵が食い違わないよう、aoにはそれを渡す)。
// 昼度(AmbientColor.a)による夜間減衰は、手続き空の導入でどこでも掛けなくなった(21.4節)。
// 空のキューブマップ自体が太陽高度に応じて暗くなるため、焼き込み時にも使用時にも不要
float3 EvaluateGlobalIBL(float3 N, float3 V, float3 worldPos, float3 albedo, float metallic, float roughness,
                         float materialAO, BentOcclusion bent, float3 brdf, int compensationMode)
{
    const float NdotV = saturate(dot(N, V));
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    // 【多重バウンス(22章)】DDGIが有効なら、拡散の環境光を前フレームのDDGIイラディアンスにする。
    //
    // これが無いとプローブへ焼かれるのは「直接光 + 空」までの1バウンスで、壁に当たった光が
    // 床を照らすところまでしか出ない(その床の照り返しが天井を照らす分は出ない)。
    // 前フレームの結果を入力に回すと
    //   フレーム1: 直接光のみ → E1(1バウンス)
    //   フレーム2: E1を環境光として使う → E2(2バウンス)
    //   フレーム3: E2を使う → E3(3バウンス) ...
    // と毎フレーム1バウンスずつ積み上がる。「Nバウンスまで計算する」のではなく
    // フィードバックループが勝手に収束するのがDDGIのinfinite bouncesの意味である。
    //
    // 減衰(kDDGIBounceAttenuation)は発散対策。反射率が1に近い白い部屋では
    // E(n+1) ≈ E(n)·ρ で ρ→1 のとき収束が遅く、数値誤差で1を超えると発散する。
    // 1未満を掛けて等比級数が必ず収束するようにしている(エネルギーを少し捨てて安定を買う)
    const float3 irradiance = (DDGIParams0.w > 0.5f)
        ? SampleDDGIIrradiance(worldPos, N, V) * kDDGIBounceAttenuation
        : IrradianceTexture.Sample(MaterialSampler, N).rgb;
    const float3 fresnelRoughness =
        F0 + (max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0) - F0) * pow(saturate(1.0f - NdotV), 5.0f);
    const float3 kd = (1.0f - fresnelRoughness) * (1.0f - metallic);
    // 拡散側のAOの出所とmulti-bounce AOは不透明パスと同じ規則にする
    const float diffuseAO = (OcclusionParams.x > 0.5f) ? bent.aoN : materialAO;
    const float3 diffuseOcclusion = (OcclusionParams.z > 0.5f)
        ? GTAOMultiBounce(diffuseAO, albedo)
        : float3(diffuseAO, diffuseAO, diffuseAO);
    const float3 diffuseIBL = kd * albedo * irradiance * diffuseOcclusion;

    const float3 R = reflect(-V, N);
    const float mipLevel = roughness * ShadowParams.y;
    const float3 prefiltered = PrefilteredEnvTexture.SampleLevel(MaterialSampler, R, mipLevel).rgb;
    // 乗算型(モード1・2)は単一散乱項へ倍率として掛かり、Kulla-Conty(3)は加算ローブを足す
    // (DeferredLighting.hlslのEvaluateIBLと同じ形。加算ぶんは拡散イラディアンスに掛かる)。
    // スペキュラオクルージョンは両方の項へ掛ける ―― ReflectionProbe.hlsliのSpecularIBLWeightと
    // SpecularIBLMultiScatterWeightがどちらも掛けているのと揃えるため
    const float3 FssEss = F0 * brdf.x + brdf.y;
    const float Ess = brdf.x + brdf.y;
    const float3 specularIBL =
        (prefiltered * FssEss * SpecularEnergyCompensation(F0, brdf, compensationMode)
         + SpecularMultiScatterIBL(F0, FssEss, Ess, compensationMode) * irradiance)
        * ComposeSpecularOcclusion((int)(OcclusionParams.y + 0.5f), bent, N, R, NdotV, roughness, materialAO, 1.0f);

    // 環境光の拡散・鏡面倍率(IBLParams.y / .z)。メインパスと同じ倍率を焼き込み時にも掛けないと、
    // プローブの中身だけつまみを動かす前の明るさで残ってしまう。
    // これを焼き上がりへ反映させるため、C++側の再ベイク署名にも両方を混ぜてある
    return diffuseIBL * IBLParams.y + specularIBL * IBLParams.z;
}

// キャプチャは2枚のレンダーターゲットへ書く。
//   SV_TARGET0 … 放射輝度(HDR)。畳み込んでプローブのイラディアンス/プリフィルタ済み鏡面になる
//   SV_TARGET1 … プローブ位置から描画点までのワールド距離(19.12節)。視差補正の精密化と
//                 光漏れの抑制に使う。深度バッファから逆算せずここで直に出しているのは、
//                 面ごとの逆投影を組む必要がなく1行で済むため
struct PSOutput
{
    float4 Radiance : SV_TARGET0;
    float Distance : SV_TARGET1;
};

PSOutput PSMain(PSInput input)
{
    float4 baseColorSample = BaseColorTexture.Sample(MaterialSampler, input.UV) * BaseColorFactor;

    // 不透明パスと同じアルファカットアウト(葉・フェンス等をプローブでも正しく抜く)
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
    // (Assets::kInvalidMaterialFactor)。GBuffer.hlslと同じく係数1.0として扱う
    float roughnessFactor = (RoughnessFactor < 0.0f) ? 1.0f : RoughnessFactor;
    float roughness = clamp(roughnessFactor * metallicRoughnessSample.g, 0.045f, 1.0f);

    float3 emissive = EmissiveTexture.Sample(MaterialSampler, input.UV).rgb * EmissiveFactor;

    // マテリアルの遮蔽マップ(ベイク済みAO)。GBuffer.hlslと同じ解釈・同じstrength適用を行う。
    // 引くUVは専用のライトマップUV(TEXCOORD1)。理由はGBuffer.hlslの同じ箇所を参照
    float occlusionSample = OcclusionTexture.Sample(MaterialSampler, input.LightmapUV).r;
    float materialAO = lerp(1.0f, occlusionSample, OcclusionStrength);
    // bent normalも同じライトマップUVで引く。接空間で焼かれているのでtbnでワールドへ移す
    // (直交行列なので長さ=遮蔽の強さは保たれる。理由はGBuffer.hlslの同じ箇所を参照)
    const float4 bentSample = BentNormalTexture.Sample(MaterialSampler, input.LightmapUV);
    const BentOcclusion bent = DecodeBentOcclusion(float4(mul(bentSample.xyz, tbn), bentSample.a), N);

    float3 albedo = baseColorSample.rgb;
    // CameraPositionにはプローブのワールド座標が入っている(ファイル冒頭参照)
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
        // カスケード選択はカメラ視錐台基準(FrameConstants.Viewはカメラのビュー行列)
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
    // ShadowParams.z = IBL強度倍率(Enable IBL無効なら0)。無効時はDeferredLighting.hlslと同じ
    // 定数色アンビエントへフォールバックし、プローブが真っ黒に焼けるのを防ぐ
    if (ShadowParams.z > 0.0f)
    {
        color += EvaluateGlobalIBL(N, V, input.WorldPos, albedo, metallic, roughness, materialAO, bent, brdf, energy.Mode) * ShadowParams.z;
    }
    else
    {
        // AmbientColor.rgbは一様な環境のイラディアンスE相当なので、その環境の放射輝度はL = E / PI。
        // 拡散項の値は従来と厳密に一致する(DeferredLighting.hlslの同じ分岐と揃えてある)
        const float3 ambientRadiance = AmbientColor.rgb / PI;

        // 鏡面項を落とすと金属(拡散項が0)が真っ黒に焼き込まれ、そのプローブを引く面の
        // 反射まで黒くなるため必ず計算する。F0/brdf/energyはPSMain冒頭で既に求めてある
        const float3 fallbackFssEss = F0 * brdf.x + brdf.y;
        const float fallbackEss = brdf.x + brdf.y;

        // 拡散項の遮蔽はディフューズAOの出所切り替え(OcclusionParams.x)に従う。鏡面項は
        // DeferredLighting.hlslの同じ分岐と同様、方向を持たない一様環境の近似のため
        // bent normalのコーン交差ではなく従来どおりmaterialAOのSpecularOcclusionのままでよい
        const float diffuseAO = (OcclusionParams.x > 0.5f) ? bent.aoN : materialAO;
        color += albedo * (1.0f - metallic) * ambientRadiance * diffuseAO
            + ambientRadiance
                * (fallbackFssEss * energy.Compensation
                   + SpecularMultiScatterIBL(F0, fallbackFssEss, fallbackEss, energy.Mode))
                * SpecularOcclusion(NdotV, roughness, materialAO);
    }

    color += emissive;

    PSOutput output;
    output.Radiance = float4(color, 1.0f);
    // CameraPositionにはプローブのワールド座標が入っている(ファイル冒頭参照)ため、
    // これがそのまま「プローブから見たこの方向の被写体までの距離」になる
    output.Distance = length(input.WorldPos - CameraPosition.xyz);
    return output;
}
