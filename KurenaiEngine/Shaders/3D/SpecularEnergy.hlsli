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

// === bent normal による遮蔽(25章) =========================================
//
// ベイクした「正規化しない可視方向の平均」bRaw を3つの量へ分解して使う。
//
//   軸  axis = normalize(bRaw)   遮蔽コーンの向き
//   aoB = length(bRaw)           コーンの広がり(= sin²αv)。スペキュラ用
//   aoN = dot(N, bRaw)           コサイン重み付きAOの定義そのもの。ディフューズ用
//
// 従来の「AOスカラー + 正規化済みbent normal」構成ではコーンの広がりの情報が失われ、
// スペキュラ遮蔽の錐体交差の構成に誤差が入る。だから正規化せずに焼いている
struct BentOcclusion
{
    float3 axis;
    float  aoB;
    float  aoN;
};

// bentSample: G-Buffer(または材質テクスチャ)から読んだ float4。.xyz = bRaw、.a = 有効フラグ
BentOcclusion DecodeBentOcclusion(float4 bentSample, float3 N)
{
    BentOcclusion o;

    // 【長さではなくフラグで判定する】bent normalを持たないマテリアルは黒1x1が
    // バインドされる。長さ0を「遮蔽なし」と解釈すると完全遮蔽(SO=0)と区別がつかないため、
    // .aを明示的な有効フラグにして曖昧さを消してある
    if (bentSample.a < 0.5f)
    {
        o.axis = N;
        o.aoB = 1.0f;
        o.aoN = 1.0f;
        return o;
    }

    o.aoB = saturate(length(bentSample.xyz));
    // 縮退時は必ずNへ落とす。bRaw / max(aoB, 1e-4) は単位ベクトルにならず、
    // 完全遮蔽ではゼロベクトルになって dot(axis, R) = 0 が帯の中途半端な位置に落ち、
    // SOを誤って持ち上げてしまう
    o.axis = (o.aoB > 1e-3f) ? bentSample.xyz / o.aoB : N;
    o.aoN = saturate(dot(N, bentSample.xyz));
    return o;
}

// bent normalによるスペキュラ遮蔽。可視コーン(軸axis・広がりaoB)と鏡面反射ローブ
// (軸R・ラフネス由来の広がり)を球冠どうしの交差として解き、その面積比を返す。
//
// Frostbite近似(上のSpecularOcclusion)との決定的な違いは方向を見ること ――
// 壁際で壁を向いた反射だけが暗くなり、壁と反対を向いた反射は暗くならない。
//
// 【tRefによる正規化(ratio estimator)が要る理由】DFG LUTは既に半球で積分済みなので、
// 素の錐体交差比をそのまま掛けると「反射コーンのうち地平線より上の割合」を二重に数えてしまう。
// 同じ地平線クリップを含む基準値tRefで割ると打ち消され、aoB = 1 で厳密にSO = 1になる
// (手で指数を調整する必要が無くなる)
// 2つの球冠(半頂角a1・a2、中心間角d)が重なる立体角の近似
// (Oat & Sander, "Ambient Aperture Lighting", 2007)。
// 厳密な球面二角形の面積は分岐が多く高価で、遮蔽の重みという用途には過剰なので、
// 「完全に含む」「交差なし」の2つの境界の間をsmoothstepで繋ぐ
float SphericalCapIntersectionArea(float a1, float a2, float d)
{
    const float amin = min(a1, a2);
    const float amax = max(a1, a2);
    // 小さいほうの球冠の立体角。完全に含まれる場合の上限になる。
    // amin = 0(完全遮蔽でコーンが潰れた場合)ならここが0になり、正しく遮蔽率0が返る
    const float capMin = 6.2831853f * (1.0f - cos(amin));

    if (d <= amax - amin) { return capMin; }  // 一方が他方を完全に含む
    if (d >= a1 + a2)     { return 0.0f; }    // まったく交差しない

    const float inner = amax - amin;
    const float t = 1.0f - saturate((d - inner) / max(a1 + a2 - inner, 1e-4f));
    return capMin * smoothstep(0.0f, 1.0f, t);
}

float SpecularOcclusionBand(float3 axis, float3 N, float3 R, float aoB, float roughness)
{
    // 可視コーンの半頂角。aoB = sin²αv の定義から αv = asin(sqrt(aoB))。
    // aoB = 1 で π/2(半球 = 遮蔽なし)、aoB = 0 で 0(コーンが潰れる = 完全遮蔽)
    const float kHalfPi = 1.5707963267948966f;
    const float av = min(asin(sqrt(saturate(aoB))), kHalfPi);

    // 鏡面ローブの半頂角。ラフネスが上がるほど広がる。
    // GBuffer.hlslでroughnessは[0.045, 1]にクランプされているため、
    // 仕様書が触れているroughness > 1.5でのcosAsの縮退はこのエンジンでは起きない
    const float as = acos(saturate(1.0f - roughness * roughness));

    const float d    = acos(clamp(dot(axis, R), -1.0f, 1.0f));
    const float dRef = acos(clamp(dot(N, R), -1.0f, 1.0f));

    const float inter = SphericalCapIntersectionArea(av, as, d);
    // 基準値: 遮蔽が無い場合(可視コーン = 半球)の重なり。
    // これで割ることでDFG LUTの半球積分との二重計上が打ち消され、
    // aoB = 1(このときaxis = Nなのでd = dRef)で厳密にSO = 1になる
    const float ref = SphericalCapIntersectionArea(kHalfPi, as, dRef);

    return ref > 1e-6f ? saturate(inter / ref) : 0.0f;
}

// スペキュラ遮蔽の合成。
//
// ベイク由来(bent normal、方向を持つ)とスクリーンスペース由来(SSAO/SSIL、方向を持たない)は
// 独立した情報なので積を取る。どちらも「遮蔽なし」のとき厳密に1を返すため、
// 片方しか持たないパス(半透明・プローブ焼き込み)へ無条件に掛けてよい。
//
// 【useBent = false の経路は従来と完全に同じ式であること】materialAOとssaoを掛けてから
// 1回だけFrostbite近似へ通す。ここを「それぞれ通してから掛ける」に変えると
// pow()が非線形なため結果が変わり、回帰確認で見た目が変わってしまう
float ComposeSpecularOcclusion(bool useBent, BentOcclusion bent, float3 N, float3 R,
                               float NdotV, float roughness, float materialAO, float ssao)
{
    if (!useBent)
    {
        return SpecularOcclusion(NdotV, roughness, materialAO * ssao);
    }

    // bent normal経路ではmaterialAO(遮蔽マップのスカラー)を使わない。
    // 同じベイクの方向付きの表現であるbent normalへ役割ごと置き換わるため、
    // 両方掛けると同じ遮蔽を二重に数えることになる
    return SpecularOcclusionBand(bent.axis, N, R, bent.aoB, roughness)
         * SpecularOcclusion(NdotV, roughness, ssao);
}

// multi-bounce AO(Jimenez et al., "Practical Realtime Strategies for Accurate Indirect
// Occlusion", SIGGRAPH 2016)。アルベドが明るいほど、遮蔽された場所でも光が跳ね返って
// 戻ってくるぶんAOを弱める。見た目を大きく変えるためUIで独立して切り替えられるようにしてある
float3 GTAOMultiBounce(float ao, float3 albedo)
{
    const float3 a = 2.0404f * albedo - 0.3324f;
    const float3 b = -4.7951f * albedo + 0.6417f;
    const float3 c = 2.7552f * albedo + 0.6903f;
    return saturate(ao * (ao * (ao * a + b) + c));
}

#endif // KURENAI_SPECULAR_ENERGY_HLSLI
