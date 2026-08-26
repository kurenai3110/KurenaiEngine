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
    // DDGIのクリップマップLOD(31.4.2節)。**要素数はC++側のkDDGIMaxLODCountと一致させること。**
    // 読むのはDDGI.hlsliだけだが、cbufferは宣言順でオフセットが決まるため、
    // DDGIParams4の後ろのフィールドを読むシェーダーはすべてここへ同じ宣言が要る
    // (飛ばすと以降のフィールドが64バイトずれ、コンパイルは通るのに別の値を読む)
    float4 DDGILODOrigin[4];
    float4 DDGILODBase[4];
    // bent normalによる遮蔽(34章)。プローブの中身も不透明パスと同じ規則で焼かないと、
    // つまみを動かしたときにプローブだけ古い見た目のまま残る。
    // KurenaiEngine3D側の再ベイク署名にもこの値を混ぜてあること
    float4 OcclusionParams;
    // これ以降(TimeParams / Sky* / Cloud* / PlanarReflectionPlane / Fog* / WaterBodyColor)は
    // このシェーダーでは一切読まないため宣言しない。読まないフィールドを並べても
    // オフセットの担保にはならず、実際にずれていても気づけないため
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

Texture2D BaseColorTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D MetallicRoughnessTexture : register(t2);
Texture2D EmissiveTexture : register(t3);
// ベイク済みアンビエントオクルージョン(遮蔽マップ)。t4はカスケードシャドウマップ配列が
// 使っているためt5を使う(GBuffer.hlsl/Transparent.hlslと共通)
Texture2D OcclusionTexture : register(t5);
// bent normal(遮蔽マップと同じライトマップUV空間)。GBuffer.hlslと同じくt6(34章)
Texture2D BentNormalTexture : register(t6);
// カスケードシャドウマップ(t4のTexture2DArray)とそのPCSSサンプリング。
// DirectLighting.hlsl/Transparent.hlslと同じ実装を共有しているため、プローブに焼かれる影と
// 本編の影が食い違うことはない。FrameConstants(CascadeViewProj/CascadeSplits/ShadowParams)と
// DataSamplerを参照するため、それらの宣言より後でインクルードする必要がある
#include "ShadowSampling.hlsli"
// プローブへ焼く1点ぶんのシェーディング(ライトリスト・グローバルIBL・多重バウンス)は
// ProbeShading.hlsliが唯一の定義。DDGIのレイトレース経路(DDGIProbeTrace.hlsl)と
// 同じ式でなければA/B比較が「経路の差」を測れなくなるため、複製を持たない
#define KURENAI_PROBE_LIGHT_REGISTER t8
#define KURENAI_PROBE_IRRADIANCE_REGISTER t9
#define KURENAI_PROBE_PREFILTERED_REGISTER t10
#define KURENAI_PROBE_BRDFLUT_REGISTER t11
// DDGI(22章)の多重バウンス用。前フレームのイラディアンスを拡散の環境光として使う
#define KURENAI_DDGI_IRRADIANCE_REGISTER t12
#define KURENAI_DDGI_DISTANCE_REGISTER t13
#include "ProbeShading.hlsli"

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

    // --- 環境光(スカイボックス由来のグローバルIBL、無効時は定数色へフォールバック) ---
    // 分岐ごとProbeShading.hlsliで共有している。DDGIのレイトレース経路と同じ式にするため
    color += EvaluateProbeEnvironment(
        N, V, input.WorldPos, albedo, metallic, roughness, NdotV, materialAO, bent, brdf, energy);

    color += emissive;

    PSOutput output;
    output.Radiance = float4(color, 1.0f);
    // CameraPositionにはプローブのワールド座標が入っている(ファイル冒頭参照)ため、
    // これがそのまま「プローブから見たこの方向の被写体までの距離」になる
    output.Distance = length(input.WorldPos - CameraPosition.xyz);
    return output;
}
