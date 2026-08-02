// 空モデル(CIE快晴空、Perez分布)の共有ヘッダー。
//
// 現在このヘッダーの利用者は3つある:
//   (a) SkyGenerate.hlsl      … IBL専用のキューブマップ(256px/面、ミップ無し)をベイクする
//   (b) DeferredLighting.hlsl … 深度が書かれていない背景画素を、画面解像度で直接評価する
//       (キューブマップは256px/面のため、3840px・水平画角68度のカメラでは約20倍に拡大表示され
//       背景としては解像度が足りない。IBLは畳み込むため低解像度のままで正しい)
//   (c) SSR.hlsl              … 水面のSSRレイが画面外へ抜けた・最大距離まで判定がつかなかった
//       画素の解析空フォールバック(P4)
// 雲(P5)はこの3者すべてに自動で行き渡るよう、この共有ヘッダーへ足した(下のSkyParameters::Cloud*と
// SkyColor末尾を参照)。ただしIBL用キューブマップ(SkyGenerate.hlsl)には雲を焼き込まない
// (理由は下の雲セクションの判断Aコメント参照)。将来大気遠近(P8)が乗る予定で、それも
// この1つの定義を経由することで背景・IBL・水面反射が同じ空を見ることを保証する。
//
// 【P7: 空の色をPreetham xyYモデルへ置き換え】以前は色味を昼・薄明・夜の3セットの
// アート的な補間(ComputeSkyTintSet/SkyTintFromSet)だけで決めていたが、P7で日中
// (太陽仰角5度以上)の色度(x, y)をPreetham et al. 1999のxyYモデルから物理的に導出するように
// 変えた(SkyColorUpperUnit参照)。夜・薄明(Preethamの定義域外)は引き続きComputeSkyTintSetの
// アート的な補間を使う——ここは変わっていない。ComputeSkyTintSet/SkyTintFromSet自体もそのまま
// 残っている(夜・薄明のクロスフェードと、地平線より下の接地色GroundTintで必要なため)。
//
// このファイルの式は Tools/generate_sky_cubemap.py(オフラインの参照実装 兼 手続き空を
// 無効にしたときのフォールバック)と一致させる必要がある。係数・定数を変える場合は
// 必ずそちらも同時に直すこと。
//
// 【P9: 照度正規化積分のGPU化】以前は色味の決定(ComputeSkyTint)と照度正規化の積分
// (ComputeSkyZenithScale、θ64分割×φ256分割=16,384サンプル)がKurenaiEngine3D.cppのCPUミラーと
// このファイルの両方に実装されており、「片方を直したら必ずもう片方も直す」という規約でしか
// 整合が保たれていなかった。P9でこれをGPU側(SkyIntegrate.hlsl)へ一本化し、CPUミラーは削除した。
// SkyIntegrate.hlslがこのファイルのComputeSkyTintSet/PerezRelativeLuminance/SkyTintFromSetを
// 直接呼んで積分し、結果(ティント4本+正規化済みの天頂輝度)をGPUSkyParametersとして
// 構造化バッファ(KurenaiEngine3D側 m_SkyParametersBuffer)へ書く。SkyGenerate.hlsl/
// DeferredLighting.hlsl/SSR.hlslはこのバッファをApplySkyParametersFromBufferで読むだけになり、
// CPU側に式のコピーは存在しない。
//
// 【cbufferに依存しない】呼び出し側ごとにcbufferのレイアウトが異なるため
// (SkyGenerate.hlslはSkyBakeConstantsから、DeferredLighting.hlslはFrameConstantsから)、
// 必要な値はすべてSkyParameters構造体で受け取る。PIも定義しない
// (DeferredLighting.hlslが既に自前でPIを定義しており、ここでも定義すると再定義エラーになるうえ、
// 空モデルの関数群はPIを使わないので不要)
#ifndef KURENAI_SKY_HLSLI
#define KURENAI_SKY_HLSLI

// --- generate_sky_cubemap.py と一致させる定数 ---
// 地平線より下は空モデルの適用範囲外。プラトー色から暗い接地色へフェードさせる。
// ゼロにしないのは、IBLの拡散イラディアンス積分で下半球が完全な暗黒にならないようにするため
static const float kGroundFadeStartY = -0.02f;
static const float kGroundFadeEndY = -0.6f;
// CIE快晴空係数(circumsolar項 c=10, d=-3)は反太陽側の水平線で輝度が天頂の0.2倍程度まで落ちる。
// 実際の大気は多重散乱で暗部が持ち上がるためゼロにはしないが、以前ここを0.45にしていたときは
// 輝度の勾配がほぼ消えて空全体が一様なスレートグレーになっていた(実測: 彩度0.26で時刻不変)。
// 勾配が残る値まで下げてある
static const float kRelativeLuminanceFloor = 0.12f;

// 空の色味セット(P9でKurenaiEngine3D.cppから移植)。太陽高度から選び、SkyIntegrate.hlslが
// GPUSkyParametersへ詰めてバッファへ書く
struct SkyTintSet
{
    float3 Zenith;
    float3 Horizon;
    float3 Ground;
    // 太陽方向まわりに乗せる暖色(夕焼け・朝焼け)
    float3 SunGlow;
    float  SunGlowStrength;
};

// SkyIntegrate.hlslが書き、SkyGenerate.hlsl/DeferredLighting.hlsl/SSR.hlslが読む
// 構造化バッファの1要素。KurenaiEngine3D.cppのGPUSkyParameters(alignas(16))と
// 完全に一致させること
struct GPUSkyParameters
{
    float4 ZenithTint;    // xyz
    float4 HorizonTint;   // xyz
    float4 GroundTint;    // xyz
    float4 SunGlowTint;   // xyz=色、w=強さ
    float4 Luminance;     // x=天頂輝度(実効プリ露出込み、雲の減光は含まない)
                           // y=余弦重み積分の値(ログ・検証用)、zw=予備
    // 【P7】Preetham xyYモデル用のパラメータ。x=タービディティ、y=Preethamの重み
    // (0=従来ティントのみ、1=Preethamのみ。SkyIntegrate.hlslが太陽仰角から求めて書く)、zw=予備
    float4 ModelParams;
};

// 空モデルの評価に必要なパラメータ一式。呼び出し側が自分のcbufferから組み立てて渡す
// (SkyGenerate.hlslはSkyBakeConstantsから、DeferredLighting.hlslはFrameConstantsから)
struct SkyParameters
{
    float3 SunDirection;     // 太陽が「ある」向き(正規化済み)。光が進む向きとは符号が逆
    float  ZenithLuminance;  // 天頂輝度。実効プリ露出を掛けた後の値
    float3 ZenithTint;
    float3 HorizonTint;
    float3 GroundTint;
    float3 SunGlowTint;
    float  SunGlowStrength;  // 太陽の暖色の強さ(仰角0度で1、±15度で0)

    // --- Preetham xyYモデル(P7) ---
    float  Turbidity;        // 大気の濁り具合。Preethamの定義域はおおむね1.7〜10
    float  PreethamWeight;   // 0=従来ティントのみ、1=Preethamのみ。太陽仰角0〜5度でクロスフェードする
                              // (夜・薄明はPreethamの定義域外のため。SkyColorUpperUnit参照)

    // --- 雲(P5)。CloudCoverage <= 0 なら雲の計算は一切行わない(判断C、SkyColor参照) ---
    // このフェーズでは積雲1層のみを実装する(計画にある巻雲の多層化はP5の対象外)。
    // 将来2層目を足すときにここへCloudCoverage2/CloudAltitude2...を素直に並べられるよう、
    // フィールドはあえて配列化せず層ごとに独立した名前のスカラー/ベクトルのまま持たせてある
    float  CloudCoverage;      // 0=雲なし、1=全天が雲
    float  CloudAltitude;      // 雲底の高度[m](カメラのワールドY基準。SkyColorはカメラの
                                // ワールド座標を受け取らないため、視線とこの高さの交点は
                                // カメラを原点とした相対座標になる。EvaluateCloudLayer参照)
    float  CloudUvScale;       // ワールド1mあたりのノイズ空間の距離
    float  CloudDensity;       // 消散係数。大きいほど不透明で影が濃い
    float2 CloudScrollOffset;  // 風によるノイズ空間の移動量(CPU側でkCloudNoisePeriodの周期に
                                // wrap済み。KurenaiEngine3D.cppのm_CloudScrollOffset参照)
    float  CloudForwardG;      // Henyey-Greensteinの非対称パラメータ(前方散乱の強さ)
};

// バッファの内容をSkyParametersのティント/輝度フィールドへ流し込むヘルパ(P9)。
// SkyGenerate.hlsl/DeferredLighting.hlsl/SSR.hlslの3つの消費側が同じ詰め替えを
// 個別に書かないようにするため、ここへ1箇所だけ置く。SunDirection・雲パラメータは
// このバッファには入っていないため呼び出し側が別途埋めること。
// 【inoutではなく値渡し+戻り値にしている理由】fxc(SM5.0)はinout引数の一部フィールドしか
// 書かないと「output parameter not completely initialized」(X3508)を出す
// (呼び出し側が既に他のフィールドを埋めていても関係なく、この関数単体で全フィールドの
// 代入が無いと判定される)。値渡し+戻り値ならこの制約に掛からない
SkyParameters ApplySkyParametersFromBuffer(SkyParameters params, GPUSkyParameters data)
{
    params.ZenithLuminance = data.Luminance.x;
    params.ZenithTint = data.ZenithTint.xyz;
    params.HorizonTint = data.HorizonTint.xyz;
    params.GroundTint = data.GroundTint.xyz;
    params.SunGlowTint = data.SunGlowTint.xyz;
    params.SunGlowStrength = data.SunGlowTint.w;
    params.Turbidity = data.ModelParams.x;
    params.PreethamWeight = data.ModelParams.y;
    return params;
}

// Perezの5係数関数。cosThetaは水平線(cosθ→0)で発散するため呼び出し側でクランプ済みの前提
float PerezF(float cosTheta, float gamma, float a, float b, float c, float d, float e)
{
    const float cosGamma = cos(gamma);
    return (1.0f + a * exp(b / cosTheta)) * (1.0f + c * exp(d * gamma) + e * cosGamma * cosGamma);
}

// 天頂輝度を1としたときの相対輝度。
//
// 【分母は必ず天頂方向で評価する】Perez分布の正規化は F(theta, gamma) / F(0, theta_s) であり、
// 分母の F(0, theta_s) は「天頂方向」での評価を意味する。天頂角は0なので第1引数(cosTheta)には
// 1.0を渡す。gammaのほうはtheta_sで正しい(天頂は太陽からtheta_sだけ離れているため)。
// ここへ cos(theta_s) を渡すと、定義上1.0になるはずの天頂の相対輝度が1.0にならず
// (実測: タービディティ2.5相当の係数で太陽仰角45度のとき0.763、仰角5度のとき0.361)、
// 太陽が低いほど誤差が拡大する。輝度の絶対値はSkyIntegrate.hlslの照度正規化で吸収されるが、
// 下のkRelativeLuminanceFloorとの相対関係が変わるため分布の形そのものが歪む。
// この誤りはP7でPreethamのxyYを実装した際に発見し、同じ誤りがあったPreetham側(SkyColorUpperUnit)と
// あわせて修正した。再発しやすい箇所なので根拠を残す
float PerezRelativeLuminance(float cosTheta, float gamma, float thetaSun)
{
    // CIE快晴空の標準係数(Perez et al. 1993 / Preetham et al. 1999, Table 1)
    const float a = -1.0f;
    const float b = -0.32f;
    const float c = 10.0f;
    const float d = -3.0f;
    const float e = 0.45f;
    return PerezF(cosTheta, gamma, a, b, c, d, e) / PerezF(1.0f, thetaSun, a, b, c, d, e);
}

// 太陽の暖色を混ぜる重み。太陽から離れるほど急に落ちる4乗カーブ。
// 太陽が地平線下にあっても、その方位の低空にはまだ暖色が残る(実際の夕焼けの残光と同じ構造)
float SunGlowWeight(float cosGamma, float glowStrength)
{
    const float proximity = saturate(cosGamma);
    const float falloff = proximity * proximity * proximity * proximity;
    return saturate(glowStrength * falloff);
}

// 方向(天頂角と太陽との離角)に対する空の色味。SkyTintSetを直接取るオーバーロード。
// SkyIntegrate.hlsl側の積分はまだGPUSkyParametersが存在しない(これから求める)段階で
// 呼ぶ必要があるため、SkyParameters経由ではなくこちらを直接使う。
// 【式は1箇所だけ】下のSkyTint(SkyParameters)はこの関数を呼ぶだけで、式そのものはここにしかない
float3 SkyTintFromSet(float cosTheta, float cosGamma, SkyTintSet tintSet)
{
    // 水平線側への寄せを3乗カーブにして、高度があるうちは天頂色をほぼ保つ
    const float horizonBlend = pow(1.0f - saturate(cosTheta), 3.0f);
    const float3 base = lerp(tintSet.Zenith, tintSet.Horizon, horizonBlend);
    return lerp(base, tintSet.SunGlow, SunGlowWeight(cosGamma, tintSet.SunGlowStrength));
}

// 方向(天頂角と太陽との離角)に対する空の色味。SkyColorUpper等、SkyParametersを持つ
// 呼び出し側向けの薄いラッパ(式の実体はSkyTintFromSet)
float3 SkyTint(float cosTheta, float cosGamma, SkyParameters params)
{
    SkyTintSet tintSet;
    tintSet.Zenith = params.ZenithTint;
    tintSet.Horizon = params.HorizonTint;
    tintSet.Ground = params.GroundTint;
    tintSet.SunGlow = params.SunGlowTint;
    tintSet.SunGlowStrength = params.SunGlowStrength;
    return SkyTintFromSet(cosTheta, cosGamma, tintSet);
}

// 太陽高度(のサイン)から空の色味を決める(P9でKurenaiEngine3D.cppのComputeSkyTintから移植)。
//
// 【P7時点でも夜・薄明はアート的な近似のまま】本来の夕焼けは、太陽光が大気を長く通る
// ことで短波長がRayleigh散乱により失われる波長依存の消散で生じる。それを解くには
// Preetham/Hosek-Wilkieのような分光モデルか大気散乱の数値積分が要る。P7で日中
// (太陽仰角5度以上)の色度はPreetham xyYモデルから物理的に導出するようになったが(Sky.hlsli
// 冒頭のP7コメント、SkyColorUpperUnit参照)、Preethamは太陽が地平線下では定義域外のため、
// 夜・薄明とその間のクロスフェード、および地平線より下の接地色(GroundTint)はこの関数
// (昼・薄明・夜の3セットを高度で補間するアート的な近似)を使い続ける。
//
// 【重要】ここで色味を暗くしても空が暗くなるわけではない。SkyIntegrate.hlslが
// 「色味の輝度成分込みで積分して目標照度に合わせる」ため、色味は最終的な明るさではなく
// 色相・彩度だけを決める。明るさはSunLighting::SkyIlluminanceLux(SkyIntegrateConstants経由)が持つ
SkyTintSet ComputeSkyTintSet(float sunElevationSin)
{
    // 昼(仰角15度以上)。従来からの値
    const float3 kDayZenith = float3(0.22f, 0.45f, 1.0f);
    const float3 kDayHorizon = float3(0.55f, 0.74f, 1.0f);
    const float3 kDayGround = float3(0.10f, 0.09f, 0.08f);
    // 薄明(仰角0度)。天頂は青を残したまま暗く、水平線は夕焼けの橙へ
    const float3 kDuskZenith = float3(0.13f, 0.22f, 0.60f);
    const float3 kDuskHorizon = float3(0.95f, 0.50f, 0.28f);
    const float3 kDuskGround = float3(0.06f, 0.05f, 0.05f);
    // 夜(仰角-15度以下)。月光で散乱した深い青。ここを昼と同じ色にしていたため
    // 「夜なのに昼と同じ空色」になっていた。
    // 月光は分光的にはほぼ太陽光そのもので、夜空が青く見えるのは暗所視の
    // プルキンエ現象による知覚的なもの。したがって青へ寄せるのは正しいが、
    // 寄せすぎるとネオンブルーになる(R比7倍まで振ったときは実測B/R=13になった)ので
    // 昼空(B/R約4.5)と同程度の彩度に留める
    const float3 kNightZenith = float3(0.09f, 0.15f, 0.40f);
    const float3 kNightHorizon = float3(0.16f, 0.24f, 0.50f);
    const float3 kNightGround = float3(0.02f, 0.02f, 0.03f);
    // 太陽方向の暖色(夕焼けの芯)
    const float3 kSunGlow = float3(1.0f, 0.38f, 0.12f);

    const float kSin15Deg = sin(radians(15.0f));
    // 仰角0度→15度で薄明から昼へ
    const float dayBlend = smoothstep(0.0f, kSin15Deg, sunElevationSin);
    // 仰角0度→-15度で薄明から夜へ
    const float nightBlend = smoothstep(0.0f, kSin15Deg, -sunElevationSin);

    SkyTintSet result;
    result.Zenith = lerp(lerp(kDuskZenith, kNightZenith, nightBlend), kDayZenith, dayBlend);
    result.Horizon = lerp(lerp(kDuskHorizon, kNightHorizon, nightBlend), kDayHorizon, dayBlend);
    result.Ground = lerp(lerp(kDuskGround, kNightGround, nightBlend), kDayGround, dayBlend);
    result.SunGlow = kSunGlow;
    // 暖色は仰角0度で最大、±15度で0になる三角窓。
    // dayBlendもnightBlendも仰角0度で0・±15度で1なので、両方の補数の積がそのまま窓になる
    result.SunGlowStrength = (1.0f - dayBlend) * (1.0f - nightBlend);
    return result;
}

// ============================================================================
// Preetham xyYモデル(P7)
//
// 従来のSkyColorUpperは「色味(SkyTintFromSet、アート的な4色補間)」と「輝度分布(Perez分布)」を
// 完全に分離して持っていた。P7では日中(太陽が地平線上、仰角5度以上)の色度(x, y)をPreetham et al.
// 1999のxyYモデルから求め、輝度(Y)と合成してXYZ→線形sRGBへ変換する。夜・薄明(Preethamの定義域外)
// は従来どおりSkyTintFromSetによるアート的な補間を使う(SkyColorUpperUnitの早期脱出/クロスフェード
// 参照)。
//
// xyY→線形sRGB(Rec.709/D65)。負成分ぶんだけ全チャンネルへ白を足すデサチュレーションを
// 入れてあるが、これは保険であって常用される経路ではない。
//
// 【実測: この保険は現状の使用域では一度も発動しない】P7の実装時に「Preethamの色度は色域外を
// 頻繁に生成する」という前提で入れたが、実際に測ると発動しなかった。タービディティ1.7〜8.0 ×
// 太陽仰角5〜60度で上半球を32×64方向に走査したところ、負値が出た方向は0.0%だった。
// Preethamの色度はCIE図の中央付近(おおむねx=0.2〜0.5、y=0.2〜0.45)に収まり、そこは
// sRGBの三角形の内側だからである。min>=0のとき何もしない実装なので残しても害は無く、
// タービディティの範囲やモデルを変えたときの保険として置いてある。
//
// 単純クランプにしていないのは、万一発動したときに彩度が飛ぶため。デサチュレーションは
// 輝度を持ち上げてしまうので、操作後に元のYへ戻るよう再スケールする
// (でないと色域外が出る方向だけ勝手に明るくなり、SkyIntegrate.hlslの照度正規化の前提が崩れる)
float3 XyYToLinearSRGB(float x, float y, float Y)
{
    // yが0近傍だとX,Zがゼロ除算で発散するため下限をクランプする
    const float safeY = max(y, 1e-4f);
    const float X = (x / safeY) * Y;
    const float Z = ((1.0f - x - y) / safeY) * Y;

    float3 rgb;
    rgb.r = 3.2406f * X - 1.5372f * Y - 0.4986f * Z;
    rgb.g = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
    rgb.b = 0.0557f * X - 0.2040f * Y + 1.0570f * Z;

    // 色域外(負値)を白へ寄せるデサチュレーション。全成分に同じ量を足す = 白を混ぜる
    const float minComponent = min(rgb.r, min(rgb.g, rgb.b));
    if (minComponent < 0.0f)
    {
        rgb -= minComponent;
    }

    // デサチュレーションで持ち上がった輝度を元のYへ戻す再スケール(Yが0近傍のときのゼロ除算に注意)
    const float rescaledLuminance = dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
    if (rescaledLuminance > 1e-6f)
    {
        rgb *= Y / rescaledLuminance;
    }

    return rgb;
}

// 天頂輝度を1としたときの空の色(水平線以上)。SkyColorUpperはこれをZenithLuminance倍するだけ。
//
// 【重要: params.ZenithLuminanceを絶対に参照しない】ZenithLuminance自体はSkyIntegrate.hlslが
// 「この関数の結果のRec.709輝度」を積分して目標照度から逆算する値である。ここでZenithLuminanceを
// 参照すると、値が決まる前にその値を使うという循環定義になってしまう
float3 SkyColorUpperUnit(float3 dir, SkyParameters params)
{
    // Perez分布は水平線で不安定になるため天頂角を89.5度までにクランプする
    const float clampedY = max(dir.y, cos(radians(89.5f)));
    const float cosTheta = clamp(clampedY, 1e-3f, 1.0f);

    // 分母(F(0, theta_s))は天頂方向で評価するため cos(theta_s) は使わない。
    // 詳しい理由はPerezRelativeLuminanceの直上のコメント参照
    const float thetaSun = acos(clamp(params.SunDirection.y, -1.0f, 1.0f));

    const float cosGamma = clamp(dot(dir, params.SunDirection), -1.0f, 1.0f);
    const float gamma = acos(cosGamma);

    // 【夜の厳密一致を担保する早期脱出】太陽が地平線下(仰角5度未満)ではPreethamは定義域外なので
    // 従来のアート的なティント補間だけを使う。ここでPreetham側の計算を一切行わずに返すことで、
    // この分岐に限ってはP7以前(P9完了時点)と画素まで厳密に一致する
    if (params.PreethamWeight <= 0.0f)
    {
        float legacyRelative = max(PerezRelativeLuminance(cosTheta, gamma, thetaSun), 0.0f);
        legacyRelative = kRelativeLuminanceFloor + (1.0f - kRelativeLuminanceFloor) * legacyRelative;
        return legacyRelative * SkyTint(cosTheta, cosGamma, params);
    }

    // --- Preetham et al. 1999のxyYモデル(Table 1〜2の係数、タービディティTの1次関数) ---
    const float T = params.Turbidity;

    // Y(輝度)の5係数。従来の固定係数(a=-1,b=-0.32,c=10,d=-3,e=0.45)をタービディティ依存へ差し替える
    const float yA = 0.1787f * T - 1.4630f;
    const float yB = -0.3554f * T + 0.4275f;
    const float yC = -0.0227f * T + 5.3251f;
    const float yD = 0.1206f * T - 2.5771f;
    const float yE = -0.0670f * T + 0.3703f;
    // 【分母はF(1, thetaSun)、cosThetaSunではない】Preethamの正規化はF(θ,γ)/F(0,θs)で、
    // 分母は「天頂方向(θ=0、cosθ=1)での評価」。天頂は太陽からthetaSunだけ離れているため
    // gammaのほうはthetaSunで正しいが、第1引数(cosθの位置)にcos(thetaSun)を渡すと
    // 天頂での評価(θ=0)ではなく太陽方向での評価になってしまう。この間違いを入れると
    // 天頂の値がY_z/x_z/y_zに一致しなくなり、太陽が低いほど誤差が拡大する
    // (実測: タービディティ2.5・仰角5度でxの誤差-22%)
    float preethamRelative = max(
        PerezF(cosTheta, gamma, yA, yB, yC, yD, yE) / PerezF(1.0f, thetaSun, yA, yB, yC, yD, yE), 0.0f);
    // 輝度フロアは実測で調整した値(kRelativeLuminanceFloorのコメント参照)。Preethamにしても
    // circumsolar項による反太陽側水平線の暗化は同じ構造で起きるため、そのまま適用する
    preethamRelative = kRelativeLuminanceFloor + (1.0f - kRelativeLuminanceFloor) * preethamRelative;

    // x(色度)の5係数
    const float xA = -0.0193f * T - 0.2592f;
    const float xB = -0.0665f * T + 0.0008f;
    const float xC = -0.0004f * T + 0.2125f;
    const float xD = -0.0641f * T - 0.8989f;
    const float xE = -0.0033f * T + 0.0452f;

    // y(色度)の5係数
    const float cyA = -0.0167f * T - 0.2608f;
    const float cyB = -0.0950f * T + 0.0092f;
    const float cyC = -0.0079f * T + 0.2102f;
    const float cyD = -0.0441f * T - 1.6537f;
    const float cyE = -0.0109f * T + 0.0529f;

    // 天頂の色度(x_z, y_z)。太陽天頂角thetaSun[ラジアン]の3次式
    const float thetaSun2 = thetaSun * thetaSun;
    const float thetaSun3 = thetaSun2 * thetaSun;
    const float xz =
        T * T * (0.00166f * thetaSun3 - 0.00375f * thetaSun2 + 0.00209f * thetaSun + 0.0f) +
        T * (-0.02903f * thetaSun3 + 0.06377f * thetaSun2 - 0.03202f * thetaSun + 0.00394f) +
        (0.11693f * thetaSun3 - 0.21196f * thetaSun2 + 0.06052f * thetaSun + 0.25885f);
    const float yz =
        T * T * (0.00275f * thetaSun3 - 0.00610f * thetaSun2 + 0.00317f * thetaSun + 0.0f) +
        T * (-0.04214f * thetaSun3 + 0.08970f * thetaSun2 - 0.04153f * thetaSun + 0.00516f) +
        (0.15346f * thetaSun3 - 0.26756f * thetaSun2 + 0.06669f * thetaSun + 0.26688f);

    // 分母は輝度と同じ理由でF(1, thetaSun)(天頂方向での評価)。これにより天頂方向(theta=0)を
    // 評価するとchromaX==xz・chromaY==yzに厳密に一致する(定義上そうあるべき値)
    const float chromaX = xz * (PerezF(cosTheta, gamma, xA, xB, xC, xD, xE) /
                                 PerezF(1.0f, thetaSun, xA, xB, xC, xD, xE));
    const float chromaY = yz * (PerezF(cosTheta, gamma, cyA, cyB, cyC, cyD, cyE) /
                                 PerezF(1.0f, thetaSun, cyA, cyB, cyC, cyD, cyE));

    const float3 preethamColor = XyYToLinearSRGB(chromaX, chromaY, preethamRelative);

    // 完全に昼(仰角5度以上)なら従来ティントの計算は行わずそのまま返す(コスト削減)
    if (params.PreethamWeight >= 1.0f)
    {
        return preethamColor;
    }

    // 薄明の遷移域(仰角0〜5度): 従来ティントとPreethamをクロスフェードする
    float legacyRelative = max(PerezRelativeLuminance(cosTheta, gamma, thetaSun), 0.0f);
    legacyRelative = kRelativeLuminanceFloor + (1.0f - kRelativeLuminanceFloor) * legacyRelative;
    const float3 legacyColor = legacyRelative * SkyTint(cosTheta, cosGamma, params);
    return lerp(legacyColor, preethamColor, params.PreethamWeight);
}

// 水平線以上を仮定した空の色(呼び出し側で地面フェードと合成する)
float3 SkyColorUpper(float3 dir, SkyParameters params)
{
    return params.ZenithLuminance * SkyColorUpperUnit(dir, params);
}

// ============================================================================
// 雲(P5、積雲1層のレイヤーモデル)
//
// 【判断A: IBL用キューブマップには雲を焼かない】
// SkyGenerate.hlslはSkyParameters組み立て時にCloudCoverage=0で埋めて呼ぶため、この節の関数は
// IBLベイクの経路では一切実行されない。雲を焼き込むと、雲が風で動くたびにキューブの焼き直し
// (空生成6回+プリフィルタ36回のディスパッチ)が必要になるが、上半球の平均照度は雲の位置が
// 変わってもほぼ不変なのでこの再ベイク連鎖は純粋な無駄になる。加えてCPU側の照度正規化
// (KurenaiEngine3D.cpp ComputeSkyZenithScale、16,384サンプルの積分)は雲を知らないため、
// 雲を焼き込むと「正規化の目標」と「実際に焼かれた明るさ」が食い違う。CPUにfBmを実装して
// 同期させるのは負債が大きすぎるため、IBLは常に雲のない晴天のまま焼く。
//
// 【判断B: 雲による減光はキューブのベイク時にだけ掛ける】
// 判断Aの結果、IBLは常に晴天基準の明るさになる。被覆率50%の空で島が晴天と同じ明るさに
// 照らされるのは不自然なため、KurenaiEngine3D.cppのRender()がキューブへ焼くSkyBakeConstants::
// ZenithLuminanceにだけ平均透過率(被覆率から求める近似。SkyBakeConstants側のコメント参照)を
// 掛けて全体を暗くする。**このSkyParameters::ZenithLuminance(背景・水面反射へ渡る値)は
// 減光しない**——ここも減光すると、雲の隙間から見える青空まで暗くなり、そこへ下のSkyColorで
// さらに雲を重ねることで二重に暗くなってしまう。
// ============================================================================

// ノイズの基本周期(格子セル数)。雲のUVは「視線と雲底平面の交点」から作るため、
// ワールド座標(≒視線方向)に比例して無限に大きくなる。風のスクロールオフセットを
// CPU側で有限に保つためにノイズを周期化しており、この定数がその周期そのものになる。
// 【KurenaiEngine3D.cppのm_CloudScrollOffsetのwrapと同じ値であること】
// CPU側は毎フレームこの値でstd::fmodしてスクロールオフセットを巻き戻しており、値がずれると
// CPU側で巻き戻した位置とシェーダー側の周期境界が食い違い、風が吹くたびに雲がジャンプする
static const float kCloudNoisePeriod = 256.0f;

// オクターブ数。既定4。親エージェント(またはコストを測る側)がこの1定数を変えるだけで
// オクターブ数を調整できるようにしてある
static const int kCloudOctaves = 4;

// 光路長のクランプに使う下限(dir.yがこれを下回ったらこの値で頭打ちにする)。
// 1/dir.yは水平線(dir.y→0)で発散するため、クランプしないと地平線際で1画素に何百もの
// 雲セルが入ってエイリアシングになる。0.05は「約2.9度以上の仰角では実質クランプがかからず、
// それより下では経路長が最大20倍で頭打ちになる」という見た目からの調整値
static const float kCloudMinDirY = 0.05f;

// 自己影(太陽方向への密度の積分)のステップ数と、太陽方向へ辿る水平距離[m]。
// 距離は雲1個(ノイズ1セル≒2km。CloudUvScaleの既定値から)の内側で明暗が付く長さにしてある。
// ステップ数はシェーダ内定数なのでコストを測る側が調整できる
static const int kCloudShadowSteps = 5;
static const float kCloudShadowSpanMeters = 1500.0f;

// 太陽方向の消散係数へ掛ける倍率。
// 【なぜ視線側と同じ係数ではいけないか】当初これを持たず、太陽方向の光学的深さを
// 「密度 × 消散係数 × ステップ距離[m]」で積んでいたところ、1ステップだけで光学的深さが
// 数百に達して自己影が常に飽和し、雲の画素が例外なく(96,96,96)という単一の灰色になった
// (雲の芯と縁の区別が数値上まったく付かなかった)。視線側の光路長は「層を斜めに貫く倍率」
// という無次元量なのに、太陽側だけメートルで積んでいたことが原因。
// 現在は太陽側も同じ無次元量(1/sin(太陽仰角))で積み、そのうえで太陽光は雲の上面から入って
// 内部で多重散乱するぶん実効的な経路が短いという近似としてこの倍率を掛ける
static const float kCloudSunExtinctionScale = 0.12f;

// 地平線際のフェード開始/終了(dir.yのしきい値)。kCloudMinDirYによる経路長クランプだけでは
// 「クランプされた雲がべったり空を覆う」領域が地平線際に残ってしまうため、
// 見た目としても薄れさせてエイリアシング対策を仕上げる。dir.y<=0(地平線より下)は
// このフェードとは別に、下のSkyColorで雲そのものを無条件に無効化する
static const float kCloudHorizonFadeEndY = 0.0f;
static const float kCloudHorizonFadeStartY = 0.2f;

// 雲の見かけのアルベド(反射率相当)、単散乱の寄与の強さ、多重散乱の下限項。
// いずれも物理値ではなく白い積雲らしい見た目になるよう調整した係数で、絶対輝度は
// ここでは一切決めない(必ずSkyParameters::ZenithLuminanceに掛ける形で表現する。
// ZenithLuminanceには既に実効プリ露出が掛かっているため、こうしておけば露出換算を
// 別途書く必要がない)。
// 多重散乱の項に下限と上限があるのは、積雲の厚い芯と薄い縁で明るさが変わるため。
// 【なぜ定数1つではいけないか】当初これを定数0.5にしていたところ、太陽から離れた方向では
// 位相関数の値が等方散乱比0.23まで落ちるため単散乱の寄与が全体の1割に満たず、
// 雲の芯が例外なく(166,166,166)という単一の値に張り付いて立体感がまったく出なかった。
// 多重散乱も厚みで減衰する量なので、自己影の透過率で下限〜上限を補間する形にしてある
// (物理的な導出ではなく、厚い芯が暗く薄い縁が明るいという積雲の見え方に合わせた近似)
static const float kCloudAlbedo = 1.0f;
static const float kCloudSingleScatterScale = 0.35f;
static const float kCloudAmbientTermMin = 0.25f;
static const float kCloudAmbientTermMax = 0.75f;

// このファイル内だけで使うPI。DeferredLighting.hlsl/SSR.hlsl側の`PI`とは別名にしてあるため
// (ファイル冒頭のコメントのとおりSky.hlsliはPIを再定義しない、という既存の方針を守るため)、
// インクルード順によらず再定義エラーは起きない
static const float kCloudPI = 3.14159265359f;

// 値ノイズ用のハッシュ関数(Dave Hoskinsのhash12。SSAO.hlsl/SSIL_VisibilityBitmask.hlslの
// Hash12と同じ式)。このハッシュ自体は周期性を持たないため、雲のノイズを周期化するには
// 呼び出し側(CloudPeriodicHash)でセル番号をkCloudNoisePeriodの剰余に落としてから渡す必要がある
float CloudHash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

// 格子座標(セルインデックス)を周期periodで巻き戻してからハッシュする。floorベースの剰余
// なので負のセル座標でも常に[0, period)に収まる(素朴に`fmod(cell, period)`だと負のセルで
// 負の値が返り、隣接セルとの参照がずれてノイズが破綻する)
float CloudPeriodicHash(float2 cell, float period)
{
    const float2 wrapped = cell - period * floor(cell / period);
    return CloudHash12(wrapped);
}

// 格子の4隅をsmoothstepで補間する標準的な値ノイズ。uvは「格子1マス=1.0」の単位
// (呼び出し側でワールド距離にCloudUvScaleを掛けてこの空間へ変換済み)
float CloudValueNoise(float2 uv, float period)
{
    const float2 cell = floor(uv);
    const float2 f = frac(uv);
    const float2 w = f * f * (3.0f - 2.0f * f);

    const float n00 = CloudPeriodicHash(cell + float2(0.0f, 0.0f), period);
    const float n10 = CloudPeriodicHash(cell + float2(1.0f, 0.0f), period);
    const float n01 = CloudPeriodicHash(cell + float2(0.0f, 1.0f), period);
    const float n11 = CloudPeriodicHash(cell + float2(1.0f, 1.0f), period);

    const float nx0 = lerp(n00, n10, w.x);
    const float nx1 = lerp(n01, n11, w.x);
    return lerp(nx0, nx1, w.y);
}

// kCloudOctaves段のfBm。オクターブごとに周期(period)も周波数と同じ倍率で2倍にしていく
// (格子1マスあたりの絶対的な広さが半分になっても、格子が表すワールド範囲の周期性は
// オクターブ0と揃っていないと継ぎ目の位置がオクターブごとにずれて周期性そのものが壊れるため)
float CloudFbm(float2 uv)
{
    float amplitude = 0.5f;
    float frequency = 1.0f;
    float period = kCloudNoisePeriod;
    float sum = 0.0f;
    float amplitudeSum = 0.0f;
    [unroll]
    for (int octave = 0; octave < kCloudOctaves; ++octave)
    {
        sum += amplitude * CloudValueNoise(uv * frequency, period);
        amplitudeSum += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
        period *= 2.0f;
    }
    // 等比級数の和で割って[0,1]へ正規化する(各オクターブのCloudValueNoiseは[0,1]を返すため)
    return sum / amplitudeSum;
}

// remap(x, lo, hi) = (x - lo) / (hi - lo)。CloudCoverage<=0(lo=hi=1)では呼び出し側
// (SkyColor)が先に早期脱出するため、ここでのゼロ除算は起こり得ない
float CloudRemap(float x, float lo, float hi)
{
    return (x - lo) / (hi - lo);
}

// 雲(1層、積雲)の透過率と散乱光を求める。呼び出し側でdir.y > 0を確認してから呼ぶこと
// (このフェーズでは積雲1層のみ。将来2層目(巻雲等)を足す場合はこの関数を層ごとに複製する)
void EvaluateCloudLayer(float3 dir, SkyParameters params, out float transmittance, out float3 scatteredLight)
{
    // (d) 光路長。dir.yが小さいほど視線は雲底平面を浅い角度で貫き経路が伸びるため、
    // 1/dir.yに比例させる。kCloudMinDirYへのクランプで地平線際の発散を防ぐ
    const float safeDirY = max(dir.y, kCloudMinDirY);
    const float pathLengthScale = 1.0f / safeDirY;

    // 視線と雲底平面(高度CloudAltitude)の交点のXZ。SkyColorはカメラのワールド位置を
    // 受け取らないため、この交点はカメラを原点とした相対座標になる
    // (CloudAltitudeが「カメラのワールドY基準」である理由。SkyParameters::CloudAltitude参照)
    const float2 hitXZ = dir.xz * (params.CloudAltitude / safeDirY);
    const float2 uv = hitXZ * params.CloudUvScale + params.CloudScrollOffset;

    // (c) 雲の密度。fBmの出力を被覆率で塊に整形する。CloudCoverage=0ならlo=hi=1になり
    // remapの分子(n-1)は常に0以下、densityは常に0になる(判断Cの根拠の一部。
    // ただし実際にはSkyColorの早期脱出でこの関数自体が呼ばれない)
    const float n = CloudFbm(uv);
    const float density = saturate(CloudRemap(n, 1.0f - params.CloudCoverage, 1.0f));

    // ビアの法則。経路長はメートル、CloudDensity(消散係数)はCPU側UIで調整する無次元の強さ
    const float opticalDepth = density * params.CloudDensity * pathLengthScale;
    transmittance = exp(-opticalDepth);

    // 自己影: 雲底のUVから太陽方向へkCloudShadowSteps段、densityを積分してビアの法則で
    // 太陽光の減衰(sunTransmittance)を求める。太陽方向はXZへ投影して使う
    // (レイヤーモデルには高度方向の厚みが無いため、太陽の仰角そのものは自己影の
    // ステップ距離に反映できない。割り切り)
    const float2 sunDirXZ = normalize(params.SunDirection.xz + 1e-4f); // 太陽が天頂付近のときのゼロ除算対策
    const float2 shadowStepUv =
        sunDirXZ * (kCloudShadowSpanMeters * params.CloudUvScale / float(kCloudShadowSteps));
    float shadowDensitySum = 0.0f;
    float2 shadowUv = uv;
    [unroll]
    for (int step = 0; step < kCloudShadowSteps; ++step)
    {
        shadowUv += shadowStepUv;
        const float shadowN = CloudFbm(shadowUv);
        shadowDensitySum += saturate(CloudRemap(shadowN, 1.0f - params.CloudCoverage, 1.0f));
    }
    // 太陽方向の光学的深さも視線側と同じ無次元量で積む(平均密度 × 消散係数 × 1/sin(太陽仰角))。
    // 単位を揃えないと自己影が飽和して雲が一様な灰色になる(kCloudSunExtinctionScaleのコメント参照)
    const float averageShadowDensity = shadowDensitySum / float(kCloudShadowSteps);
    const float sunPathLengthScale = 1.0f / max(params.SunDirection.y, kCloudMinDirY);
    const float sunOpticalDepth =
        averageShadowDensity * params.CloudDensity * kCloudSunExtinctionScale * sunPathLengthScale;
    const float sunTransmittance = exp(-sunOpticalDepth);

    // Henyey-Greenstein位相関数。cosAngle=1(dirが太陽方向と一致=太陽を直視する向き)で
    // 前方散乱が最大になり、半逆光で雲の縁が光る効果が出る(CloudForwardGが強さ)
    const float cosAngle = dot(dir, params.SunDirection);
    const float g = params.CloudForwardG;
    const float g2 = g * g;
    const float phaseDenom = pow(max(1.0f + g2 - 2.0f * g * cosAngle, 1e-4f), 1.5f);
    const float phase = (1.0f - g2) / (4.0f * kCloudPI * phaseDenom);

    // 位相関数は立体角で積分すると1になるよう正規化されているため、素の値は1/(4π)≒0.08の
    // オーダーになる。等方散乱を1とした相対値へ直してから重みを掛けないと、単散乱の寄与が
    // 下の多重散乱の下限項に対して2桁小さくなり、太陽側の縁が光る効果がまったく見えなくなる
    const float phaseNormalized = phase * 4.0f * kCloudPI;

    // 単散乱の簡易近似: 自己影を通って弱まった太陽光(sunTransmittance)を位相関数で配分する。
    // 多重散乱の項も同じ自己影の透過率で下限〜上限を補間し、厚い芯が暗く薄い縁が明るくなるようにする。
    // (1-transmittance)は視線の経路のうち実際に散乱へ回った分のスケール(下の行で掛ける)
    const float multiScatter = lerp(kCloudAmbientTermMin, kCloudAmbientTermMax, sunTransmittance);
    const float3 inScatter =
        kCloudAlbedo * (sunTransmittance * phaseNormalized * kCloudSingleScatterScale + multiScatter);
    scatteredLight = params.ZenithLuminance * inScatter * (1.0f - transmittance);

    // (e) 地平線際のフェード。kCloudMinDirYによる経路長クランプと合わせてのエイリアシング対策
    const float fade = smoothstep(kCloudHorizonFadeEndY, kCloudHorizonFadeStartY, dir.y);
    transmittance = lerp(1.0f, transmittance, fade);
    scatteredLight *= fade;
}

float3 SkyColor(float3 dir, SkyParameters params)
{
    if (dir.y >= kGroundFadeStartY)
    {
        const float3 clearColor = SkyColorUpper(dir, params);

        // (h) 早期脱出。CloudCoverage<=0、または地平線より下(dir.y<=0、(e)節)では
        // 雲の計算を一切行わずclearColorをそのまま返す。判断C(被覆率0のときP4完了時点と
        // 画素まで一致すること)の担保はここで行う——雲側の計算(EvaluateCloudLayer)は
        // 一度も呼ばれず、返す値もSkyColorUpperの結果そのままなので数値は変わりようがない
        if (params.CloudCoverage <= 0.0f || dir.y <= 0.0f)
        {
            return clearColor;
        }

        // (g) 合成。lerpではなくこの形にするのは、雲の隙間からのぞく青空をそのまま残すため
        // (lerpだと被覆率で単純に混ぜてしまい、隙間の青空まで雲色へ寄ってしまう)。
        // 地平線より下(この関数の後続のelse分岐)には雲を一切掛けない
        float transmittance;
        float3 scatteredLight;
        EvaluateCloudLayer(dir, params, transmittance, scatteredLight);
        return clearColor * transmittance + scatteredLight;
    }

    // 水平線より下: プラトー色(kGroundFadeStartYの高さへ射影した方向の空色)から接地色へフェード。
    // (g) 雲は掛けない——ここはSkyColorUpperを直接呼ぶだけで、雲を合成する上のif内へは入らない
    float3 plateauDir = dir;
    plateauDir.y = kGroundFadeStartY;
    plateauDir = normalize(plateauDir);
    const float3 plateauColor = SkyColorUpper(plateauDir, params);

    const float3 groundColor = params.ZenithLuminance * params.GroundTint;
    const float groundT = saturate((dir.y - kGroundFadeStartY) / (kGroundFadeEndY - kGroundFadeStartY));
    return lerp(plateauColor, groundColor, groundT);
}

#endif // KURENAI_SKY_HLSLI
