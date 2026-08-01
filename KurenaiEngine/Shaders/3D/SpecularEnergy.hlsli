// スペキュラBRDFのエネルギーに関わる共通処理。
// DirectLighting.hlsl(直接光)・Transparent.hlsl(半透明フォワード)・BRDFLUT.hlsl(LUT生成)・
// DeferredLighting.hlsl(IBL)から#includeする。NormalEncoding.hlsliに次ぐ2つ目の共有ヘッダー。
//
// このファイルが可視性項(GeometrySmith)とエネルギー補正の両方を持っているのは意図的で、
// 「BRDF積分LUTを焼くときのBRDFと、実行時に評価するBRDFが必ず同じものである」という
// 不変条件をコメントではなく構造で保証するため。両者がずれるとLUT由来のEssが実際のBRDFを
// 記述しなくなり、下のエネルギー補正が成り立たなくなる。

#ifndef KURENAI_SPECULAR_ENERGY_HLSLI
#define KURENAI_SPECULAR_ENERGY_HLSLI

// BRDF積分LUTは必ずColorSampler(s1、Linear + Clamp)で引くこと。
// LUTはUVそのものが定義域(u = NdotV、v = roughness、どちらも[0,1])なので、
// MaterialSampler(s0)側にWrapが入るパスで引くと端が反対側へ回り込んで別のEssが混ざる
// (経緯と実際に出た絵はSamplers.hlsliのColorSamplerのコメント参照)。
// LUT生成側(BRDFLUT.hlsl)はテクスチャを読まないためサンプラーを使わない
#include "Samplers.hlsli"

// Smith幾何項のSchlick近似。kはSmith-GGXへの最良フィットである α/2 (α = roughness^2) を使う。
//
// 以前は直接光だけ k = (roughness+1)^2 / 8 を使っていた。これはDisneyのラフネス再マップ
// α' = ((roughness+1)/2)^2 (Burley 2012) を挟んだもので、点光源のハイライトが低ラフネスで
// 鋭すぎる問題("hotness")を意図的に抑えるための非物理な調整である
// (Karis, SIGGRAPH 2013 "Real Shading in Unreal Engine 4"。同文献はこの再マップについて
//  「解析光源にのみ使うこと。IBLに適用すると斜めの角度で暗くなりすぎる」と明記している)。
//
// この再マップは以下の2つの理由で除去した:
//   ・Smith-GGXの正しいフィットから意図的にずらしており物理的に不正確
//   ・BRDF積分LUTは k = roughness^2/2 で焼かれているため、kの違う直接光へLUT由来のEssを
//     適用してもエネルギー補正が整合しない
// 除去により直接光の鏡面は斜め視線・低ラフネスで明るくなる(実測で最大5倍。roughness 1.0と
// 正面視では変化なし)。グレージング角でフレネルが1へ漸近する物理的に正しい挙動である。
float GeometrySchlickGGX(float NdotX, float roughness)
{
    const float a = roughness * roughness;
    const float k = a / 2.0f;
    return NdotX / (NdotX * (1.0f - k) + k);
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

// スペキュラBRDFのmultiple-scattering energy compensation(エネルギー補正)。
//
// Cook-Torrance(GGX)のスペキュラ項はマイクロファセット間の「1回だけの反射」しか考慮しておらず、
// 上のSmith幾何項が遮蔽として捨てた光 ―― 現実には隣のマイクロファセットへ跳ね返って外へ出ていく
// ぶん ―― を数えていない。失われる量はラフネスが高いほど大きく、F0=1のときの方向アルベドで
// 測るとラフネス1.0では0.307まで落ちる(=7割が消える)。結果として粗い金属が本来より暗く、
// 多重反射のたびに金属色が乗るはずの彩度も低く見える。
//
// EssがBRDF積分LUTから得られる理由: BRDFLUT.hlslはSchlickのフレネル
// F = F0*(1-Fc) + Fc を括り出して A = ∫(1-Fc)*Gvis、B = ∫Fc*Gvis を焼いている。
// F0*A + B はそのF0における方向アルベドそのものなので、F0=1を代入した A + B が
// ここで必要な「F=1のときの方向アルベド」に一致する。
//
// ------------------------------------------------------------------------------------
// 3つの方式を切り替えられるようにしてある(FrameConstants.ShadowParams.w = モード番号)。
// C++側 KurenaiEngine3D::SpecularCompensationMode と値を一致させること。
//
//   1 Linear : comp = 1 + F0(1/Ess - 1)          補正後アルベド = Ess + F0(1-Ess)
//              失われたぶんをF0で「1回だけ」跳ね返して戻す等比級数の第1項近似。
//   2 Series : comp = 1 / (1 - F0(1-Ess))        補正後アルベド = Ess / (1 - F0(1-Ess))
//              同じ等比級数を全項足したもの。
//   3 Kulla-Conty: 乗算ではなく、広い加算ローブを足す本来の形
//              (Kulla & Conty, "Revisiting Physically Based Shading at Imageworks",
//               SIGGRAPH 2017)。E(NdotV)・E(NdotL)・Eavgの3つを使うためµo/µi対称で、
//              相反性を満たす。IBL側はそのsplit-sum版
//              (Fdez-Agüera, "A Multiple-Scattering Microfacet Model for Real-Time
//                Image-Based Lighting", JCGT 2019)。
//
// 【重要な性質】F0=1 では Linear と Series は代数的に同一(どちらも 1/Ess)になり、
// さらに3方式とも補正後アルベドが厳密に1.0になる。つまりWhite Furnace Testは
// 3方式を区別できない。区別できるのは (a) F0<1、(b) 方向分布(点光源・太陽光での
// ローブ形状)の2つだけである(詳細と実測はdocs/Architecture.html 14.9節)。
// ------------------------------------------------------------------------------------

#define KURENAI_SPEC_COMP_OFF        0
#define KURENAI_SPEC_COMP_LINEAR     1
#define KURENAI_SPEC_COMP_SERIES     2
#define KURENAI_SPEC_COMP_KULLACONTY 3

// SchlickフレネルF(µ)の半球平均。Favg = 2∫F(µ)µdµ = F0 + (1-F0)/21
// (∫(1-µ)^5 µ dµ = B(2,6) = 1/42 より)
float3 FresnelAverage(float3 F0)
{
    return F0 + (1.0f - F0) / 21.0f;
}

// 乗算型(モード1・2)の補正倍率。モード0とモード3では恒等元の1を返す。
//
// 適用箇所が複数あり呼び出し側で分岐を書くと直し忘れが起きるため、無効時も1を返して
// 呼び出し側は常に無条件で乗算できるようにしてある
// (modeは定数バッファ由来で波面内で一様のため、分岐コストは実質ゼロ)。
//
// F0   : スペキュラの垂直入射反射率(lerp(0.04, albedo, metallic))
// brdf : BRDF積分LUTのサンプル値(x=スケールA, y=バイアスB, z=Eavg)
// mode : FrameConstants.ShadowParams.w を int 化したもの
float3 SpecularEnergyCompensation(float3 F0, float3 brdf, int mode)
{
    // Essの実測レンジはおおむね[0.31, 1.0](ラフネス1.0・NdotV=1.0で最小)。
    // LUTの生成が壊れた場合にNaN/Infがシーン全体へ伝播しないよう下限をクランプしておく
    const float Ess = max(brdf.x + brdf.y, 1e-3f);

    if (mode == KURENAI_SPEC_COMP_LINEAR)
    {
        return 1.0f + F0 * (1.0f / Ess - 1.0f);
    }
    if (mode == KURENAI_SPEC_COMP_SERIES)
    {
        return 1.0f / max(1.0f - F0 * (1.0f - Ess), 1e-3f);
    }
    return float3(1.0f, 1.0f, 1.0f);
}

// Kulla-Contyのマルチスキャッタ加算ローブ(直接光用)。モード3以外では0を返す。
//
//   Fms  = Favg^2 * Eavg / (1 - Favg(1 - Eavg))
//   f_ms = (1 - E(NdotV))(1 - E(NdotL)) / (π (1 - Eavg))
//   戻り値 = Fms * f_ms   (呼び出し側で単一散乱項へ加算し、NdotLを掛ける)
//
// 乗算型と違いE(NdotL)を要るため、ライトごとにLUTのフェッチが1回増える。
// F0=1・一様環境では Ess + Fms(1-Ess) = 1 に厳密に一致する。
//
// EssV/EssL : それぞれ NdotV / NdotL での A+B
// Eavg      : LUTの第3成分(ラフネスだけの関数)
float3 SpecularMultiScatterLobe(float3 F0, float EssV, float EssL, float Eavg, int mode)
{
    if (mode != KURENAI_SPEC_COMP_KULLACONTY)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const float3 Favg = FresnelAverage(F0);
    // ラフネス0ではEavg→1で分母が0へ落ちる。分子の(1-EssV)(1-EssL)も同時に0へ向かうため
    // 値としては0に収束するが、0除算でNaNを出さないよう下限を入れる
    const float3 Fms = Favg * Favg * Eavg / max(1.0f - Favg * (1.0f - Eavg), 1e-4f);
    const float fms = (1.0f - EssV) * (1.0f - EssL) / max(3.14159265359f * (1.0f - Eavg), 1e-4f);
    return Fms * fms;
}

// Kulla-ContyのIBL版(Fdez-Agüera 2019 のsplit-sum形)。モード3以外では0を返す。
//
//   Ems = 1 - Ess
//   Fms = FssEss * Favg / (1 - Ems * Favg)
//   戻り値 = Fms * Ems   (呼び出し側で拡散イラディアンスを掛けて鏡面IBLへ加算する)
//
// 直接光側がEavgを使うのに対しこちらがEssで閉じているのは、split-sum近似がすでに
// 環境を方向で平均した形になっているため。F0=1・一様環境では FssEss + Fms*Ems = 1 に厳密。
//
// FssEss : F0 * brdf.x + brdf.y(単一散乱ぶんの方向アルベド)
// Ess    : brdf.x + brdf.y
float3 SpecularMultiScatterIBL(float3 F0, float3 FssEss, float Ess, int mode)
{
    if (mode != KURENAI_SPEC_COMP_KULLACONTY)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const float3 Favg = FresnelAverage(F0);
    const float Ems = 1.0f - Ess;
    const float3 Fms = FssEss * Favg / max(1.0f - Ems * Favg, 1e-4f);
    return Fms * Ems;
}

// 直接光パス(DirectLighting.hlsl / Transparent.hlsl)で、ピクセル内で一定な量をまとめたもの。
// Ess(NdotV)・Eavgは(NdotV, ラフネス)だけの関数なので、ライトのループへ入る前に1度だけ求める
// (ループ内でLUTを引くとライト数ぶんテクスチャフェッチが増えてしまう)。
//
// Kulla-Conty方式だけは E(NdotL) も必要で、これはライト方向に依存するためループ内で引かざるを
// 得ない。そのフェッチは各シェーダーのEvaluateDirectBRDF内にある(BRDFLUTTextureのレジスタが
// シェーダーごとに違うため、共有ヘッダーには置けない)
struct SpecularEnergyContext
{
    int Mode;             // KURENAI_SPEC_COMP_*
    float3 Compensation;  // 乗算型(モード1・2)の倍率。それ以外は1
    float EssV;           // NdotVでの A+B
    float Eavg;           // LUT第3成分(ラフネスだけの関数)
    float Roughness;      // E(NdotL)を引き直すのに必要
};

// brdf       : BRDF積分LUTのサンプル値(x=A, y=B, z=Eavg)
// modeParam  : FrameConstants.ShadowParams.w をそのまま渡す
SpecularEnergyContext MakeSpecularEnergyContext(float3 F0, float3 brdf, float roughness, float modeParam)
{
    const int mode = (int)(modeParam + 0.5f);

    SpecularEnergyContext context;
    context.Mode = mode;
    context.Compensation = SpecularEnergyCompensation(F0, brdf, mode);
    context.EssV = brdf.x + brdf.y;
    context.Eavg = brdf.z;
    context.Roughness = roughness;
    return context;
}

// スペキュラオクルージョン(Lagarde & de Rousiers, "Moving Frostbite to Physically Based
// Rendering 3.0", 2014)。拡散光用に求めたAOをそのまま鏡面へ掛けると粗い面で暗くなりすぎるため、
// ラフネスが高いほど指数を1に近づけてAOの効きを弱める。
//
// ao = 1(遮蔽なし)のときは必ず1を返す: 底 NdotV + 1 は1以上、指数は常に正なので
// pow(NdotV + 1, e) >= 1 となり、saturate(... - 1 + 1) が1に飽和する。
// そのためAOを持たないパス・マテリアルへ無条件に掛けても見た目は変わらない。
//
// この関数はReflectionProbe.hlsliのSpecularIBLWeight(不透明パス+SSR)と
// Transparent.hlsl・ProbeCapture.hlslの両方から呼ばれる。不透明と半透明で鏡面の
// 遮蔽量がずれると同じマテリアルが描画パスによって違う明るさになるため、
// エネルギー補正と同じくここに定義を1つだけ置く
float SpecularOcclusion(float NdotV, float roughness, float ao)
{
    const float specularOcclusionExponent = exp2(-16.0f * roughness - 1.0f);
    return saturate(pow(NdotV + ao, specularOcclusionExponent) - 1.0f + ao);
}

#endif // KURENAI_SPECULAR_ENERGY_HLSLI
