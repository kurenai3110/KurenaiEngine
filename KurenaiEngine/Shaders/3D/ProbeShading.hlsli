// プローブへ焼き込む1点ぶんのシェーディングを1か所へ集めた共有ヘッダー。
// 反射プローブ/DDGIのラスタ経路(ProbeCapture.hlsl)と、DDGIのレイトレース経路
// (DDGIProbeTrace.hlsl)が**同じ式**を使うためのもの。
//
// 【なぜ共有しなければならないのか】DDGIのレイ取得をDXRへ載せ替えるとき、
// 「ラスタ経路と同じ式で陰影を付ける」ことがA/B比較の前提になる。式をコピーすると
// 「一致させ続けなければならない式」が1つ増え、片方だけ直したときに
// コンパイルも通り絵も「それらしく」出てしまう。定義を1つにして構造的に保証する。
//
// 【インクルードする側の責務】このヘッダーより前に、次をすべて用意しておくこと。
//   - #include "SpecularEnergy.hlsli"  (GeometrySmith / SpecularEnergyContext /
//                                       BentOcclusion / GTAOMultiBounce /
//                                       ComposeSpecularOcclusion / SpecularOcclusion 等)
//   - #include "Samplers.hlsli"        (MaterialSampler / ColorSampler)
//   - static const float PI            (このヘッダーは自前で定義しない。
//                                       includer 側の既存の定義と衝突させないため)
//   - cbuffer FrameConstants : register(b0) を **宣言順どおり DDGIParams4 まで**。
//     ここで読むのは LightDirection / LightColor / AmbientColor / ShadowParams /
//     IBLParams / OcclusionParams / ActiveLightCount / DDGIParams0..4。
//     途中のフィールドを省くとオフセットがずれ、コンパイルは通るのに
//     見当違いの値を読む(SSR.hlslが実際に踏んだ罠)
//   - 下のレジスタマクロをすべて #define しておくこと

#ifndef KURENAI_PROBESHADING_HLSLI
#define KURENAI_PROBESHADING_HLSLI

#if !defined(KURENAI_PROBE_LIGHT_REGISTER) || !defined(KURENAI_PROBE_IRRADIANCE_REGISTER) || \
    !defined(KURENAI_PROBE_PREFILTERED_REGISTER) || !defined(KURENAI_PROBE_BRDFLUT_REGISTER)
#error "ProbeShading.hlsli をインクルードする前に KURENAI_PROBE_*_REGISTER をすべて定義すること"
#endif

// DirectLighting.hlsl側のstruct GPULightと並び・ストライド(64バイト)を一致させる必要がある
struct GPULight
{
    float4 PositionType;
    float4 ColorRange;
    float4 DirectionAngle;
    float4 Params;
};
StructuredBuffer<GPULight> Lights : register(KURENAI_PROBE_LIGHT_REGISTER);

// スカイボックス由来のグローバルIBL。プローブに映る面の環境光として使う
TextureCube IrradianceTexture : register(KURENAI_PROBE_IRRADIANCE_REGISTER);
TextureCube PrefilteredEnvTexture : register(KURENAI_PROBE_PREFILTERED_REGISTER);
Texture2D BRDFLUTTexture : register(KURENAI_PROBE_BRDFLUT_REGISTER);

// DDGI(22章)の多重バウンス用。前フレームのイラディアンスを拡散の環境光として使う。
// レジスタは includer が KURENAI_DDGI_*_REGISTER で指定する
#include "DDGI.hlsli"

// 多重バウンスの減衰。1未満でなければならない(理由はEvaluateGlobalIBLのコメント参照)。
// 0.95は「1バウンスあたり5%のエネルギーを捨てる」という意味で、反射率1の白い部屋でも
// 等比級数 1 + 0.95 + 0.95² + ... = 20 で必ず収束する
static const float kDDGIBounceAttenuation = 0.95f;

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
    // 1未満を掛けて等比級数が必ず収束するようにしている(エネルギーを少し捨てて安定を買う)。
    //
    // 焼いているプローブがDDGIボリュームの外にある場合(ボリュームは1個しか置けないため、
    // プローブがDDGI格子の外に立つことがある)はDDGIIrradianceのinsideWeightが0になるので、
    // グローバルIBL側へ
    // フォールバックする。DeferredLighting.hlslのEvaluateIBLと同じ規則
    float3 irradiance = IrradianceTexture.Sample(MaterialSampler, N).rgb;
    if (DDGIParams0.w > 0.5f)
    {
        float ddgiInsideWeight;
        const float3 ddgiIrradiance = SampleDDGIIrradiance(worldPos, N, V, ddgiInsideWeight) * kDDGIBounceAttenuation;
        irradiance = lerp(irradiance, ddgiIrradiance, ddgiInsideWeight);
    }
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

// プローブへ焼く「環境光の項」。IBLの有無で分岐する部分ごと共有する。
//
// 【分岐ごと共有する理由】ShadowParams.z(IBL強度倍率)が0のときのフォールバックを
// 片方の経路だけが持っていると、IBLを切ったシーンでレイトレース経路のプローブだけが
// 真っ黒に焼ける。分岐の外へ出しておけば取り違えようがない
float3 EvaluateProbeEnvironment(
    float3 N, float3 V, float3 worldPos, float3 albedo, float metallic, float roughness,
    float NdotV, float materialAO, BentOcclusion bent, float3 brdf, SpecularEnergyContext energy)
{
    // 【早期returnにしない理由】fxcのSM5経路は、途中でreturnする関数に対して
    // X4000(use of potentially uninitialized variable)を誤検出する。
    // 1つの戻り値へ集める形にしておけば警告が出ない
    float3 result;

    // ShadowParams.z = IBL強度倍率(Enable IBL無効なら0)。無効時はDeferredLighting.hlslと同じ
    // 定数色アンビエントへフォールバックし、プローブが真っ黒に焼けるのを防ぐ
    if (ShadowParams.z > 0.0f)
    {
        result = EvaluateGlobalIBL(N, V, worldPos, albedo, metallic, roughness, materialAO, bent, brdf, energy.Mode)
               * ShadowParams.z;
    }
    else
    {
        // AmbientColor.rgbは一様な環境のイラディアンスE相当なので、その環境の放射輝度はL = E / PI。
        // 拡散項の値は従来と厳密に一致する(DeferredLighting.hlslの同じ分岐と揃えてある)
        const float3 ambientRadiance = AmbientColor.rgb / PI;
        const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

        // 鏡面項を落とすと金属(拡散項が0)が真っ黒に焼き込まれ、そのプローブを引く面の
        // 反射まで黒くなるため必ず計算する
        const float3 fallbackFssEss = F0 * brdf.x + brdf.y;
        const float fallbackEss = brdf.x + brdf.y;

        // 拡散項の遮蔽はディフューズAOの出所切り替え(OcclusionParams.x)に従う。鏡面項は
        // DeferredLighting.hlslの同じ分岐と同様、方向を持たない一様環境の近似のため
        // bent normalのコーン交差ではなく従来どおりmaterialAOのSpecularOcclusionのままでよい
        const float diffuseAO = (OcclusionParams.x > 0.5f) ? bent.aoN : materialAO;
        result = albedo * (1.0f - metallic) * ambientRadiance * diffuseAO
            + ambientRadiance
                * (fallbackFssEss * energy.Compensation
                   + SpecularMultiScatterIBL(F0, fallbackFssEss, fallbackEss, energy.Mode))
                * SpecularOcclusion(NdotV, roughness, materialAO);
    }

    return result;
}

#endif // KURENAI_PROBESHADING_HLSLI
