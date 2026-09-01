// ポイント/スポットライト(punctual light)1灯ぶんの評価を1か所へ集めた共有ヘッダー。
// 直接光パス(DirectLighting.hlsl)とタイルライトカリング(LightCulling.hlsl)が同じ
// GPULight の定義を使い、直接光と MegaLights が**同じ BRDF・同じ減衰**でライトを評価するためのもの。
//
// 【なぜ共有しなければならないのか】MegaLights は「候補から確率的に1灯選び、選ぶ確率で割り戻す」
// 手法で、選ぶ確率(目標関数)と最終的なシェーディングの両方がこの式に依存する。
// 式をコピーすると「一致させ続けなければならない式」が増え、片方だけ直したときに
// コンパイルも通り絵も「それらしく」出てしまう。定義を1つにして構造的に保証する
// (ProbeShading.hlsli がラスタ経路とレイトレース経路のために同じことをしているのと同じ判断)。
//
// 【インクルードする側の責務】
//   - KURENAI_PUNCTUAL_LIGHT_REGISTER を、ライトリスト(StructuredBuffer<GPULight>)を置く
//     レジスタへ #define しておくこと(DirectLighting.hlsl は t8、LightCulling.hlsl は t0)
//   - BRDF まで使う場合は KURENAI_PUNCTUAL_LIGHTING_BRDF も #define し、
//     **このヘッダーより前に**次をすべて用意しておくこと:
//       - static const float PI      (このヘッダーは自前で定義しない。includer 側の
//                                     既存の定義と衝突させないため。ProbeShading.hlsli と同じ規約)
//       - #include "SpecularEnergy.hlsli"  (GeometrySmith / SpecularEnergyContext /
//                                           SpecularMultiScatterLobe / KURENAI_SPEC_COMP_KULLACONTY)
//       - Texture2D BRDFLUTTexture         (EvaluateDirectBRDF が Kulla-Conty の加算ローブで引く)
//       - ColorSampler                     (SpecularEnergy.hlsli が Samplers.hlsli 経由で持ってくる)
//
// 【ProbeShading.hlsli と同時にインクルードしてはいけない】あちらも同じ struct GPULight を
// 宣言しているため、両方を読むと再定義になる。プローブ側は自前のレジスタマクロを持つ別経路で、
// 統合するなら片方へ寄せること(現状どのシェーダーも両方は読まない)。

#ifndef KURENAI_PUNCTUAL_LIGHTING_HLSLI
#define KURENAI_PUNCTUAL_LIGHTING_HLSLI

#ifndef KURENAI_PUNCTUAL_LIGHT_REGISTER
#error "PunctualLighting.hlsli をインクルードする前に KURENAI_PUNCTUAL_LIGHT_REGISTER を定義すること"
#endif

// ポイント/スポットライト1灯ぶんのデータ。C++側 KurenaiEngine3D.cpp の GPULight と
// 並び・ストライド(64バイト)を一致させる必要がある。既存の SSAOConstants/SSILConstants と同様、
// パッキング規則の解釈揺れを避けるためメンバはすべて float4 単位で宣言する
struct GPULight
{
    float4 PositionType;   // xyz=ワールド座標, w=LightType(0=Directional, 1=Point, 2=Spot)
    // rgb = Color * Intensity[cd] * exposure(EV100)。カンデラ→露出済みの最終放射輝度で、
    // CPU側(MakeGPULight)で計算してあるためシェーダ側はそのまま乗算するだけでよい
    float4 ColorRange;     // rgb=露出済み放射輝度, w=Range
    float4 DirectionAngle; // xyz=向き(正規化済み), w=spotAngleScale
    // x=spotAngleOffset, y=CastShadow(1でスクリーンスペースシャドウを撃つ / 0で撃たない),
    // zw=未使用(エリアライト用に予約)
    float4 Params;
};
StructuredBuffer<GPULight> Lights : register(KURENAI_PUNCTUAL_LIGHT_REGISTER);

// Karis 2013 / Frostbite の windowed inverse-square。Range を超えると厳密に0になり、
// 打ち切り境界でのハードエッジが出ない
float DistanceAttenuation(float distSq, float range)
{
    float factor = distSq / max(range * range, 1e-4f); // (d/r)^2
    float window = saturate(1.0f - factor * factor);   // 1 - (d/r)^4
    // 光源に極端に近づいたときの発散を抑える。定数1.0を足す実装はシーンスケール依存になるため、
    // 最小距離二乗でのクランプにする
    return (window * window) / max(distSq, 0.0001f);
}

// Frostbite の lightAngleScale / lightAngleOffset。CPU側(MakeGPULight)で事前計算した値を
// GPULight.DirectionAngle.w / Params.x として受け取る
//   scale  = 1 / max(0.001, cos(inner) - cos(outer))
//   offset = -cos(outer) * scale
float SpotAttenuation(float3 spotDirection, float3 L, float angleScale, float angleOffset)
{
    float t = saturate(dot(spotDirection, -L) * angleScale + angleOffset);
    return t * t;
}

#ifdef KURENAI_PUNCTUAL_LIGHTING_BRDF

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * d * d, 1e-6f);
}

// GeometrySchlickGGX / GeometrySmith はSpecularEnergy.hlsliの共有定義を使う
// (**Disneyのラフネス再マップ k=(roughness+1)^2/8 をここへ足してはいけない**。理由は同ヘッダー参照)

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// SpecularEnergyContext(スペキュラのエネルギー補正のうちピクセル内で一定な量)は
// SpecularEnergy.hlsliの共有定義を使う。

// Cook-Torrance を1灯ぶん評価する(シャドウ・ライト色・減衰は呼び出し側で乗算する)。
// 太陽(b0)とポイント/スポットライト(ライトリスト)の両方から共通で呼ばれる。
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

    // 補正は鏡面項にのみ掛ける(拡散項kdは変更しない。理由は14.9節)
    float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, 1e-4f) * energy.Compensation;

    if (energy.Mode == KURENAI_SPEC_COMP_KULLACONTY)
    {
        // 加算ローブはE(NdotL)を要る。ライトのループ内から呼ばれるため、勾配に依存しない
        // SampleLevelを使う(Sampleは動的な分岐・ループ内で勾配が未定義になり得る)
        const float2 brdfL = BRDFLUTTexture.SampleLevel(ColorSampler, float2(NdotL, energy.Roughness), 0).rg;
        specular += SpecularMultiScatterLobe(F0, energy.EssV, brdfL.x + brdfL.y, energy.Eavg, energy.Mode);
    }

    float3 kd = (1.0f - F) * (1.0f - metallic);
    float3 diffuse = kd * albedo / PI;

    return (diffuse + specular) * NdotL;
}

// 透過項に掛ける遮蔽の下限を決める係数。透過率へ掛けたものが「完全な影でも残る割合」になる
// (透過率0.55なら下限0.33)。葉が重なった樹冠を単発のシャドウ問い合わせで表すための近似で、
// 本来は多重散乱で解くべきところ。上げるほど樹冠の内側が明るく平坦になる
static const float kTranslucencyShadowFloor = 0.6f;

// 薄いものの透過(translucency)を1灯ぶん評価する。シャドウ・ライト色は呼び出し側で乗算する。
//
// 【何のためにあるか】葉・花弁・紙のように薄いものは、裏から当たった光を透かして
// 表側が明るく見える。Cook-Torranceは NdotL<=0 の面を真っ黒にするため、そのままでは
// 逆光の樹冠が空の環境光だけで照らされ、青灰色に沈む(45章)。
//
// 【物理的な位置づけ】厚みを持つ媒質の散乱を解くのではなく、
// 「薄い両面の被写体」を近似する定番の形(いわゆる wrap / back-lit translucency)。
// 拡散のみで鏡面は持たない ―― 透過してきた光は媒質内で散乱しきっており、
// 表面反射のローブを作らないため。
float3 EvaluateTranslucency(float3 N, float3 V, float3 L, float3 albedo, float translucency)
{
    if (translucency <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    // 裏面がどれだけ光を受けているか。表(N)の裏返し(-N)で測る
    float backNdotL = saturate(dot(-N, L));

    // 前方散乱。光の進行方向(-L)を見込む角度ほど強く透ける(葉を太陽にかざしたときの見え方)。
    // 全周に一定量を残すのは、散乱しきった成分がどの方向にも一様に抜けるため
    float forward = saturate(dot(V, -L));
    float lobe = 0.35f + 0.65f * (forward * forward * forward);

    // アルベドを掛けるのは、透けてくる光が媒質の色に染まるため(白い花弁は白く、葉は緑に透ける)。
    // 1/PIは拡散項と同じ正規化
    return albedo * (translucency * backNdotL * lobe / PI);
}

#endif // KURENAI_PUNCTUAL_LIGHTING_BRDF

#endif // KURENAI_PUNCTUAL_LIGHTING_HLSLI
