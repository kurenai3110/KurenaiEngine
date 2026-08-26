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
//     (ShadowSampling.hlsli)。ここへ複製すると、片方だけ直したときに半透明と不透明で
//     影が食い違う
//
// DX12のルートシグネチャがCBVをb0/b1の2枠しか持たないため、GBuffer.hlslと同じくb1に
// ObjectConstants(モデル行列)を置く。そのためDirectLighting.hlsl側のb1(LightingConstants、
// 有効ライト数)をここでは使えず、有効ライト数はFrameConstants末尾のActiveLightCountで受け取る

// Smith可視性項とスペキュラのエネルギー補正。DirectLighting.hlslのPBR計算を複製している
// このシェーダーでも、BRDF積分LUT(BRDFLUT.hlsl)と同じ可視性項を使う必要があるため共有する
#include "SpecularEnergy.hlsli"

static const float PI = 3.14159265359f;

// 反射プローブ(19章)の環境ソースと鏡面IBLの重み。DeferredLighting.hlsl・SSR.hlslと同じ定義を
// 共有する。空きスロットが違うだけでレジスタ番号は各シェーダーが決める(ReflectionProbe.hlsli冒頭)。
// このパスはt0〜t4とt8〜t11を既に使っているため、空いているt5〜t7を割り当てる
// (反射プローブは鏡面専任なので拡散イラディアンス用のスロットは要らない)。
// #include自体はFrameConstantsとSamplers.hlsliの宣言より後で行う必要があるため下にある
#define KURENAI_GLOBAL_IRRADIANCE_REGISTER t9
#define KURENAI_GLOBAL_PREFILTERED_REGISTER t10
#define KURENAI_PROBE_PREFILTERED_REGISTER t5
#define KURENAI_PROBE_BUFFER_REGISTER t7
#define KURENAI_PROBE_DISTANCE_REGISTER t12
// DDGI(22章)。t0-t3,t5-t14を既に使っている(t14はbent normal・34章)ため、空いているt15/t16を
// 割り当てる。不透明側のDeferredLighting.hlslもDDGIはt15/t16なので番号が揃う
#define KURENAI_DDGI_IRRADIANCE_REGISTER t15
#define KURENAI_DDGI_DISTANCE_REGISTER t16

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
    // x: 拡散イラディアンス(グローバルIBL)の取得元(0=プリフィルタ済み鏡面の最終ミップ、
    // 1=専用イラディアンスマップ)。ReflectionProbe.hlsliのSampleGlobalIrradiance参照。yzwは未使用
    float4 IBLParams;
    // 反射プローブ用(19章)。x=有効プローブ数、y=影響範囲のデバッグ表示フラグ(このパスでは未使用)、
    // z=視差補正の有効フラグ、w=プローブ間ブレンドの有効フラグ。
    // DeferredLighting.hlslと同じ値が入っているため、半透明と不透明で環境ソースが食い違うことはない
    float4 ProbeParams;
    // 距離キューブ用(19.12節)。意味はDeferredLighting.hlslと同じ
    float4 ProbeParams2;
    // 【以下2つはこのシェーダーでは使わないが宣言だけ必要】cbufferは宣言順レイアウトなので、
    // 後ろのDDGIParams/OcclusionParamsを正しいオフセットで読むには途中のフィールドを飛ばせない。
    // C++側のFrameConstantsと並びを必ず一致させること
    float4x4 PrevViewProj;
    float4 TAAParams;
    // DDGI用(22章、M11 Stage 1)。レイアウトはC++側 KurenaiEngine3D.cpp の FrameConstants の
    // コメント参照。DDGI.hlsliがこの5本を読む
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
    // bent normalによる遮蔽(34章)。DeferredLighting.hlslと同じ規則を適用する
    float4 OcclusionParams;
    // これ以降(TimeParams / Sky* / Cloud* / PlanarReflectionPlane / Fog* / WaterBodyColor)は
    // このシェーダーでは一切読まないため宣言しない。
    // 【半透明メッシュにフォグを掛けない理由】AerialPerspective.hlslはこのTransparentパスより
    // 後のTAA直前に置かれるため、Transparentが描いた色も含めて後段でまとめてフォグが掛かる
    // (このシェーダー自体でフォグを計算する必要が無い)
};

// GBuffer.hlslのObjectConstantsと同じレイアウト(AlphaCutoffはBLENDマテリアルでは常に0で
// 実質未使用だが、同じルートシグネチャ/定数バッファを共有するため並び順を合わせる)。
// 末尾のBaseColorFactorはGBuffer.hlsl/ProbeCapture.hlslも同じ位置で宣言して使う
// (Shadow.hlslは深度しか書かないため先頭のWorldまでしか宣言していない)
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
// ベイク済みアンビエントオクルージョン(遮蔽マップ)。GBuffer.hlsl/ProbeCapture.hlslではt5だが、
// このシェーダーはt5・t7を反射プローブ(KURENAI_PROBE_PREFILTERED/BUFFER_REGISTER、
// 上記マクロ参照)に、t11をBRDFLUTTexture(下記)に割り当てて衝突するため、空いているt13を使う
Texture2D OcclusionTexture : register(t13);
// bent normal(遮蔽マップと同じライトマップUV空間)。このパスはt0〜t13を使い切っているためt14。
// 不透明側はG-Bufferから読むが、フォワードの半透明はここでテクスチャを直接引く(34章)
Texture2D BentNormalTexture : register(t14);
// カスケードシャドウマップ(t4のTexture2DArray)とそのPCSSサンプリング。
// DirectLighting.hlslと同じ実装を共有しているため、半透明と不透明で影がずれることはない。
// FrameConstants(CascadeViewProj/CascadeSplits/ShadowParams)とDataSamplerを参照するため、
// それらの宣言より後でインクルードする必要がある
#include "ShadowSampling.hlsli"
// IBL(14章)と反射プローブ(19章)。グローバルIBLのイラディアンス(t9)/プリフィルタ済み鏡面(t10)と、
// プローブのプリフィルタ済み鏡面キューブマップ配列(t5)・影響範囲バッファ(t7)の宣言、および
// プローブの選択・視差補正・ブレンドはReflectionProbe.hlsliが持つ(DeferredLighting.hlslと共有)。
// 反射プローブは鏡面専任なので、このパスもプローブ側の拡散イラディアンスは持たない。
//
// 半透明パスにはSSRが適用されないため、ガラスにとっては環境ソースが唯一の映り込みになる。
// ここをグローバルIBL固定にすると、密閉された室内のガラスにも空が映る
#include "ReflectionProbe.hlsli"
// DDGI(22章)。拡散イラディアンスだけを差し替える。半透明パスにはSSRが無いため
// 20章の不変条件には関与しない(不透明側のDeferredLighting.hlslと同じ理由)。
// 【重要】DDGIが有効な点は不透明側と同じくDDGIへ寄せること。ここだけ別の経路にすると、
// 同じ画面の中で不透明面と半透明面が別々の間接拡散光を受けることになる
#include "DDGI.hlsli"
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
//
// 環境ソース(拡散イラディアンス・プリフィルタ済み鏡面)は不透明側とまったく同じSampleEnvironmentで
// 求める。これにより反射プローブ・視差補正・ブレンド・イラディアンスの取得元切り替えの規則が
// 半透明と不透明で食い違うことがなくなる。
// IBL強度倍率(ShadowParams.z)はこの関数の中で掛け切る(呼び出し側では掛けない)。
//
// 半透明パスはスクリーンスペースのAO/GIバッファ(SSAO/SSIL)を持たないが、マテリアルの
// 遮蔽マップ(ベイク済みAO)はテクスチャなのでこのパスでも使える。aoにはそれを渡す
// (遮蔽マップを持たないマテリアルは白1x1でao=1となり、従来と同じ結果になる)
void EvaluateIBLSplit(
    float3 N, float3 V, float3 worldPos, float3 albedo, float metallic, float roughness,
    float materialAO, BentOcclusion bent,
    out float3 outDiffuse, out float3 outSpecular)
{
    const float NdotV = saturate(dot(N, V));
    const float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    const float3 R = reflect(-V, N);
    // ShadowParams.y = プリフィルタ済み鏡面マップの最大ミップレベル(KurenaiEngine3D側で設定)
    const float mipLevel = roughness * ShadowParams.y;

    float3 irradiance;
    float3 prefiltered;
    // 不透明側(DeferredLighting.hlslのEvaluateIBL)と同じく、開いている方向で
    // イラディアンスを引く。鏡面のRは変えない
    const float3 irradianceDir = (OcclusionParams.x > 0.5f) ? bent.axis : N;
    SampleEnvironment(worldPos, irradianceDir, R, mipLevel, irradiance, prefiltered);

    // DDGI(22章、M11 Stage 1)が有効なら拡散だけをDDGI由来へ差し替える。DeferredLighting.hlslの
    // EvaluateIBLとまったく同じ規則(鏡面prefilteredには触れない、ボリューム外はlerpの重み0で
    // SampleEnvironmentの値が残る)。半透明もこの規則に合わせないと、不透明面はDDGI・
    // 半透明面は反射プローブという食い違った間接拡散光になる
    if (DDGIParams0.w > 0.5f)
    {
        float ddgiInsideWeight;
        const float3 ddgiIrradiance = SampleDDGIIrradiance(worldPos, N, V, ddgiInsideWeight);
        irradiance = lerp(irradiance, ddgiIrradiance, ddgiInsideWeight);
    }

    // --- 拡散IBL ---
    // ラフネスを考慮したFresnel-Schlick(Lagarde, "Moving Frostbite to PBR")
    const float3 fresnelRoughness =
        F0 + (max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), F0) - F0) * pow(saturate(1.0f - NdotV), 5.0f);
    const float3 kd = (1.0f - fresnelRoughness) * (1.0f - metallic);

    // --- 鏡面IBL(split-sum近似) ---
    // LUTの第3成分(Eavg)はKulla-Conty方式だけが使うため.rgbで引く(DeferredLighting.hlslの
    // EvaluateIBLと同じ。SpecularIBLWeightはfloat3のbrdfを受け取る)
    const float3 brdf = BRDFLUTTexture.Sample(ColorSampler, float2(NdotV, roughness)).rgb;
    // スペキュラオクルージョン・エネルギー補正・IBL強度倍率をまとめて掛ける係数。
    // 半透明パスはスクリーンスペースのAO/GIバッファを持たないため、そちらは1.0を渡す。
    // ComposeSpecularOcclusionはどちらの経路も「遮蔽なしで厳密に1」なので、
    // 片方が無くても結果は歪まない(遮蔽マップもbent normalも無ければ以前と同じ結果になる)
    // 0 = Frostbite近似 / 1 = 球冠交差 / 2 = 球面ガウス(34.11節)
    const int soMode = (int)(OcclusionParams.y + 0.5f);
    const float3 specularWeight =
        SpecularIBLWeight(F0, NdotV, roughness, soMode, bent, N, R, materialAO, 1.0f, brdf,
                          ShadowParams.w, ShadowParams.z);
    // Kulla-Conty方式(ShadowParams.w = 3)が足す加算ローブ。DeferredLighting.hlslの
    // EvaluateIBLと同じ形にしておかないと、同じマテリアルが不透明と半透明で違う明るさになる。
    // 乗算型(1・2)と無効(0)ではこの係数が0になり、項ごと消える
    const float3 multiScatterWeight =
        SpecularIBLMultiScatterWeight(F0, NdotV, roughness, soMode, bent, N, R, materialAO, 1.0f, brdf,
                                      ShadowParams.w, ShadowParams.z);

    // 【昼度(AmbientColor.a)による減衰はしない】DeferredLighting.hlslのEvaluateIBLと同じ理由で、
    // 手続き空(SkyGenerate.hlsl)が太陽高度に応じて自分で暗くなるため、ここで掛けると
    // 二重に暗くなる(21.4節)。
    // 鏡面側のShadowParams.z(IBL強度倍率)と遮蔽はspecularWeightに含まれている。
    // 拡散側のAOの出所と multi-bounce AO も不透明側(DeferredLighting.hlslのEvaluateIBL)と
    // 同じ規則にしないと、同じマテリアルが不透明と半透明で違う明るさになる
    const float diffuseAO = (OcclusionParams.x > 0.5f) ? bent.aoN : materialAO;
    const float3 diffuseOcclusion = (OcclusionParams.z > 0.5f)
        ? GTAOMultiBounce(diffuseAO, albedo)
        : float3(diffuseAO, diffuseAO, diffuseAO);
    outDiffuse = kd * albedo * irradiance * diffuseOcclusion * ShadowParams.z;
    outSpecular = prefiltered * specularWeight + irradiance * multiScatterWeight;
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

    // 環境光。半透明パスにはSSRが適用されないため、ガラスにとってはこの環境ソースが唯一の
    // 「環境の映り込み」になる。デルタ光源(太陽・ポイント/スポット)のスペキュラだけでは、
    // 低ラフネスのガラスは正反射条件を満たす極めて狭い帯にしかハイライトが出ず、
    // 透明なだけの面に見えてしまう。
    // ShadowParams.z = IBL強度倍率(Enable IBL無効なら0)。無効時はDeferredLighting.hlslと同じく
    // 定数色アンビエント(拡散+鏡面)へフォールバックする。
    // SSAO/SSILによる遮蔽・間接拡散光は非対応(常にao=1・間接光=0として扱う既知の制約)。
    // ただしマテリアルの遮蔽マップ(ベイク済みAO)はテクスチャなのでこのパスでも効く
    float3 ambientDiffuse;
    float3 ambientSpecular;
    if (ShadowParams.z > 0.0f)
    {
        // ShadowParams.zはEvaluateIBLSplitの中で拡散・鏡面それぞれに掛かっている
        // bent normalも遮蔽マップと同じLightmapUVで引く。持たないマテリアルは黒1x1が
        // バインドされ、DecodeBentOcclusionがaxis = N・aoB = 1(遮蔽なし)へ落とす。
        // 接空間で焼かれているのでtbnでワールドへ移す(直交行列なので長さは保たれる。
        // 理由はGBuffer.hlslの同じ箇所を参照)
        const float4 bentSample = BentNormalTexture.Sample(MaterialSampler, input.LightmapUV);
        const BentOcclusion bent = DecodeBentOcclusion(float4(mul(bentSample.xyz, tbn), bentSample.a), N);

        EvaluateIBLSplit(N, V, input.WorldPos, albedo, metallic, roughness, materialAO, bent,
                         ambientDiffuse, ambientSpecular);
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
        // 遮蔽マップはIBLの有無に関わらず環境光に効かせる(IBL有効時のEvaluateIBLSplitと同じ扱い)。
        //
        // AmbientColor.rgbは一様な環境のイラディアンスE相当(IBLConvolve.hlslが行っている
        // 1/πと積分のπの相殺を受けていない)なので、環境の放射輝度はL = E / PIになる。
        // 【/PIを落とすと拡散・鏡面ともπ倍明るくなる】不透明・半透明・プローブ焼き込みの
        // 3パスで式を揃えること
        const float3 ambientRadiance = AmbientColor.rgb / PI;
        const float3 fallbackFssEss = F0 * brdf.x + brdf.y;
        const float fallbackEss = brdf.x + brdf.y;
        ambientDiffuse = albedo * (1.0f - metallic) * ambientRadiance * materialAO;
        ambientSpecular = ambientRadiance
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
