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

// BRDF積分LUT専用のサンプラー。LUTはUVそのものが定義域(u = NdotV、v = roughness、どちらも[0,1])
// なので、汎用サンプラー(s0)のWrapで引いてはいけない。Wrapだと u→1(視線が法線と一致する面の中央)
// でバイリニアのタップが u≈0(グレージング角)のテクセルへ回り込み、まったく別のEssが混ざる。
// 実際にWhite Furnace Testの球の中心へ数ピクセルの斑点として現れた
// (異方性フィルタも併用していたため、画面空間の勾配のぶんだけ回り込みが広がっていた)。
// v(roughness)側も同様で、roughness=0の面にroughness≈1の値が混ざる。
// エンジン側はここへLinear + Clampのサンプラーをバインドする(KurenaiEngine3D::m_LUTSampler)。
// LUT生成側(BRDFLUT.hlsl)はテクスチャを読まないためこの宣言を使わないが、共有ヘッダーに置くことで
// 「LUTを引く側は必ずこのサンプラーを使う」ことを構造で担保する
SamplerState BRDFLUTSampler : register(s1);

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
// Kulla & Conty, "Revisiting Physically Based Shading at Imageworks"(SIGGRAPH 2017)の
// multiple-scattering energy compensationの実時間近似を使う:
//
//   Ess  = brdf.x + brdf.y
//   comp = 1 + F0 * (1 / Ess - 1)
//   specular *= comp
//
// 補正後の方向アルベドは Ess * comp = Ess + F0 * (1 - Ess) となり、「失われたぶんを
// F0で1回だけ跳ね返して戻す」形(等比級数の第1項近似)になる。F0=1(完全な金属)なら
// 厳密に1でエネルギー保存し、F0=0.04(誘電体)なら失われたぶんの4%しか戻さない ――
// これも正しく、誘電体は残りを透過・吸収するため。
//
// EssがBRDF積分LUTから追加チャンネル無しで得られる理由: BRDFLUT.hlslはSchlickのフレネル
// F = F0*(1-Fc) + Fc を括り出して A = ∫(1-Fc)*Gvis、B = ∫Fc*Gvis を焼いている。
// F0*A + B はそのF0における方向アルベドそのものなので、F0=1を代入した A + B が
// ここで必要な「F=1のときの方向アルベド」に一致する。LUTのフォーマット変更は不要。
//
// 既知の近似: 本来のKulla-ContyはE(NdotV)・E(NdotL)・Eavg(全方向平均)の3つを使うが、
// この実時間近似はE(NdotV)のみで済ませているため、視線方向と光源方向の非対称性(相反性)は
// 厳密には再現されない。
//
// F0         : スペキュラの垂直入射反射率(lerp(0.04, albedo, metallic))
// brdf       : BRDF積分LUTのサンプル値(x=スケールA, y=バイアスB)
// enableFlag : ImGuiトグル。FrameConstants.ShadowParams.wを渡す(0以下で補正を無効化)
float3 SpecularEnergyCompensation(float3 F0, float2 brdf, float enableFlag)
{
    if (enableFlag <= 0.0f)
    {
        // A/B比較用のトグル。適用箇所が5つあり呼び出し側で分岐を書くと直し忘れが起きるため、
        // 無効時は恒等元の1を返して呼び出し側は常に無条件で乗算できるようにする
        // (enableFlagは定数バッファ由来で波面内で一様のため、分岐コストは実質ゼロ)
        return float3(1.0f, 1.0f, 1.0f);
    }

    // Essの実測レンジはおおむね[0.31, 1.0](ラフネス1.0・NdotV=1.0で最小)。
    // LUTの生成が壊れた場合にNaN/Infがシーン全体へ伝播しないよう下限をクランプしておく
    const float Ess = max(brdf.x + brdf.y, 1e-3f);
    return 1.0f + F0 * (1.0f / Ess - 1.0f);
}

#endif // KURENAI_SPECULAR_ENERGY_HLSLI
