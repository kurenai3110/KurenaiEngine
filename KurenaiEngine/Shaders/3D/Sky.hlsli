// 空モデル(CIE快晴空、Perez分布)の共有ヘッダー。
//
// 現在このヘッダーの利用者は5つある:
//   (a) SkyGenerate.hlsl         … IBL専用のキューブマップ(256px/面、ミップ無し)をベイクする
//   (b) DeferredLighting.hlsl    … 深度が書かれていない背景画素を、画面解像度で直接評価する
//       (キューブマップは256px/面のため、3840px・水平画角68度のカメラでは約20倍に拡大表示され
//       背景としては解像度が足りない。IBLは畳み込むため低解像度のままで正しい)
//   (c) SSR.hlsl                 … 水面のSSRレイが画面外へ抜けた・最大距離まで判定がつかなかった
//       画素の解析空フォールバック
//   (d) AerialPerspective.hlsl   … 大気遠近のin-scatter項。遠方の地物が無限遠で背景の空色へ
//       厳密に収束するようにするため、フォグの合成先としてこのモデルの色をそのまま使う
//   (e) PlanarReflection.hlsl    … 平面反射の鏡像にも同じ大気遠近を掛けるため、(d)と同じ理由でin-scatter項に使う
// 雲はこの5者すべてに自動で行き渡るよう、この共有ヘッダーへ足した(下のSkyParameters::Cloud*と
// SkyColor末尾を参照)。ただしIBL用キューブマップ(SkyGenerate.hlsl)には雲を焼き込まない
// (理由は下の雲セクションの判断Aコメント参照)。大気遠近の消散係数・スケールハイト自体は
// このヘッダーの管轄ではない(空モデルではなく大気遠近固有の値のため、HeightFog.hlsli側に持つ)。
//
// 【空の色】日中(太陽仰角5度以上)の色度(x, y)はPreetham et al. 1999のxyYモデルから
// 物理的に導出する(SkyColorUpperUnit参照)。夜・薄明はPreethamの定義域外なので、
// ComputeSkyTintSet/SkyTintFromSetによる昼・薄明・夜3セットのアート的な補間を使う
// (地平線より下の接地色GroundTintも同じ経路)。
//
// このファイルの式は Tools/generate_sky_cubemap.py(オフラインの参照実装 兼 手続き空を
// 無効にしたときのフォールバック)と一致させる必要がある。係数・定数を変える場合は
// 必ずそちらも同時に直すこと。
//
// 【照度正規化積分はGPU側に一本化してある】色味の決定と照度正規化の積分
// (θ64分割×φ256分割=16,384サンプル)はSkyIntegrate.hlslだけが持ち、CPUミラーは置かない
// (二重実装にすると「片方を直したら必ずもう片方も直す」という規約でしか整合が保てない)。
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

// 雲層(有限距離にある)へ大気遠近を掛けるために使う(EvaluateCloudLayerの(f)節)。
// HeightFog.hlsliはcbuffer/レジスタに一切依存しない純粋関数だけのヘッダーなので、
// この共有ヘッダーから読んでも利用者5者(冒頭の(a)〜(e))の結合は増えない。
// 二重includeはHeightFog.hlsli側のインクルードガードで無害
#include "HeightFog.hlsli"

// SkyView LUTのUVパラメータ化。焼く側(AtmosphereLUT.hlsl)と厳密に同じ写像を使う
#include "AtmosphereCommon.hlsli"

// ============================================================================
// SkyView LUT
//
// 日中の空の色はHillaire (2020)の大気モデルを焼いたこのLUTから引く。
// 雲の3Dノイズと同じく、レジスタはインクルードする側がマクロで決める:
//   KURENAI_SKYVIEW_REGISTER   SkyView LUT(Texture2D)
//
// **このマクロを定義しないシェーダーでは日中の空が黒くなる**ので、SkyColorUpperUnitを
// 呼ぶ利用者は全員定義すること(SkyGenerate/SkyIntegrate/DeferredLighting/SSR/
// AerialPerspective/PlanarReflectionの6者)。雲の3Dノイズと違い「定義しなければ
// 従来の経路が残る」形にはできない — Preethamの実装そのものを置き換えたためで、
// 定義漏れをコンパイルエラーで捕まえるためにあえてフォールバックを用意していない。
//
// サンプラーはs1(ColorSampler、両セットともLinear+Clamp)。UVそのものが定義域なので
// Wrapで引いてはならない類のLUTで、BRDF積分LUTと同じ扱いになる(Samplers.hlsli参照)
// ============================================================================
#ifdef KURENAI_SKYVIEW_REGISTER
#include "Samplers.hlsli"
Texture2D SkyViewLUTTexture : register(KURENAI_SKYVIEW_REGISTER);
#endif

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

// 空の色味セット。太陽高度から選び、SkyIntegrate.hlslが
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
    // Preetham xyYモデル用のパラメータ。x=タービディティ、y=Preethamの重み
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

    // --- 雲の明るさを太陽照度基準にするための係数(EvaluateCloudLayer参照) ---
    // 空照度に対する天頂輝度の比の逆数(積分値)。SkyIntegrate.hlslがGPUSkyParameters::
    // Luminance.yへ書いた値をApplySkyParametersFromBufferがそのまま渡す。定義上
    // 「空の照度 = SkyIlluminanceOverZenith × ZenithLuminance」という無次元量になる
    float  SkyIlluminanceOverZenith;
    // 太陽照度/空照度の比。CPU側(KurenaiEngine3D.cpp)のSunLighting::KeyIlluminanceLux /
    // SkyIlluminanceLuxから求め、FrameConstants::SkyParams.z経由で渡ってくる。
    // ApplySkyParametersFromBufferでは埋まらないため、呼び出し側(各MakeSkyParameters)が
    // 別途代入すること
    float  SunToSkyIlluminanceRatio;

    // --- 日中の空(HillaireのSkyView LUT) ---
    // 大気の濁り具合。**この関数は読まない**。濁りはSkyView LUTを焼く側
    // (AtmosphereLUT.hlsl)でMieの密度倍率として効き、焼き上がったLUTに織り込まれている。
    // フィールドを残してあるのはSkyIntegrate.hlslが従来ティント経路(夜)でも
    // GPUSkyParametersを組み立てるためで、値そのものはログ・デバッグ用
    float  Turbidity;
    // 0=従来ティントのみ、1=物理モデル(Hillaire)のみ。太陽仰角0〜5度でクロスフェードする。
    // Hillaireは低い太陽も素で扱えるが、月光・薄明視(21.9.7)が従来ティント経路に
    // 乗っているためこの分岐が要る(SkyColorUpperUnit参照)
    float  PhysicalSkyWeight;
    // 空の彩度。**物理量ではなく明示的なアート指定**。1.0で物理モデルの色度そのまま。
    // 色度図上で白色点(D65)から遠ざける倍率で、色相は変えずに鮮やかさだけを変える。
    // 物理モデル側で空の青さは足りるので、既定の1.0から動かす必要は本来無い
    // (SkyColorUpperUnitの該当箇所参照)
    float  SkySaturation;

    // --- 雲(積雲1層と、2層目の巻雲)。CloudCoverage <= 0 なら積雲側の計算は
    // 一切行わない(判断C、SkyColor参照)。フィールドはあえて配列化せず層ごとに独立した
    // 名前のスカラー/ベクトルのまま持たせてある(層ごとに高度・UVスケール等の単位や
    // 意味合いが異なり、配列化してもインデックスの意味を別途覚える必要が生じるため) ---
    float  CloudCoverage;      // 0=雲なし、1=全天が雲
    float  CloudAltitude;      // 雲底の高度[m](カメラのワールドY基準。SkyColorはカメラの
                                // ワールド座標を受け取らないため、視線とこの高さの交点は
                                // カメラを原点とした相対座標になる。EvaluateCloudLayer参照)
    float  CloudUvScale;       // ワールド1mあたりのノイズ空間の距離
    float  CloudDensity;       // 消散係数。大きいほど不透明で影が濃い
    float2 CloudScrollOffset;  // 風によるノイズ空間の移動量(CPU側でkCloudNoisePeriodの周期に
                                // wrap済み。KurenaiEngine3D.cppのm_CloudScrollOffset参照)
    float  CloudForwardG;      // Henyey-Greensteinの非対称パラメータ(前方散乱の強さ)
    float  CloudThickness;     // 雲底から雲頂までの厚み[m]。0ならレイマーチせず
                                // 従来の厚みゼロの平面として扱う(巻雲はこちら)

    // --- 巻雲。積雲より高層にある2層目。CirrusCoverage <= 0 なら巻雲側の計算は
    // 一切行わない(判断C、SkyColor参照)。判断A(IBLキューブに雲を焼かない)・判断B
    // (平均透過率だけをベイク時に掛ける)は積雲とまったく同じ理由でこちらにも適用する
    // (Sky.hlsli冒頭の雲セクション、KurenaiEngine3D.cppのComputeCloudAverageTransmittance参照)。
    // 前方散乱の強さと自己影ステップ数はシェーダ内定数(kCirrusForwardG/kCirrusShadowSteps)
    // のため、ここにはフィールドを持たない(cbufferを増やす価値がないため) ---
    float  CirrusCoverage;     // 0=巻雲なし、1=全天が巻雲
    float  CirrusAltitude;     // 雲底の高度[m](カメラのワールドY基準。積雲と同じ規約)
    float  CirrusUvScale;      // ワールド1mあたりのノイズ空間の距離
    float  CirrusDensity;      // 消散係数。積雲より1桁小さい値を想定(巻雲は光学的に薄いため)
    float2 CirrusScrollOffset; // 風によるノイズ空間の移動量(積雲と同じくkCloudNoisePeriodでwrap済み。
                                // KurenaiEngine3D.cppのm_CirrusScrollOffset参照)
    float  CirrusAnisotropy;   // fBmのUV(U方向)を伸ばして筋状にする倍率。V方向は1.0固定

    // --- 雲へ掛ける大気遠近。雲は「深度を持たない背景」として描かれるため
    // AerialPerspective.hlslの早期脱出(depth <= 0)に入り、フォグを一切受けていなかった。
    // だが雲は無限遠ではなく高度1,500m(積雲)・8,000m(巻雲)の有限距離にある層で、
    // 視線が寝るほど斜距離が伸びる(仰角15度で積雲まで5.8km)。掛けないと消散係数を上げたとき
    // 「地物は溶けたのに雲だけ剃刀のようにくっきり」という絵になる(詳細はEvaluateCloudLayer末尾)。
    //
    // 値はFrameConstants::FogParams0とCameraPosition.yから各MakeSkyParametersが埋める。
    // ApplySkyParametersFromBufferでは埋まらない(空パラメータバッファはフォグを知らない)ため
    // 呼び出し側が別途代入すること。FogEnabledが0のときEvaluateCloudLayerはフォグの計算を
    // 一切行わず、フォグを持たない場合と厳密に同じ値を返す ---
    float  FogEnabled;         // 0=フォグ無効(このヘッダーでは何もしない)、1=有効
    float  FogSigma0;          // 基準高度での消散係数[1/m](FogParams0.x)
    float  FogScaleHeight;     // スケールハイト[m](FogParams0.y)
    float  FogRefHeight;       // 基準高度[m](ワールドY。FogParams0.z)
    float  FogViewerHeight;    // 視点のワールドY。雲底高度がカメラ基準の相対値なので、
                                // 絶対高度へ戻して消散係数を評価するために要る

    // --- 星空 ---
    // 【SkyColorでしか使わない】星は背景と水面の映り込みにだけ描き、IBLキューブ
    // (SkyGenerate.hlsl)とフォグのin-scatter(AerialPerspective.hlsl)へは入れない。
    // それらのMakeSkyParametersはStarsIntensityに0を入れること。
    // 理由: キューブは256px/面しかなく点光源を焼くとエイリアシングし、
    // プリフィルタ後の鏡面反射でファイアフライになる。星明かりの「照明」としての寄与は
    // KurenaiEngine3D.cppのkStarlightIlluminanceLuxが一様な下限として既にモデル化済みで、
    // ここは見た目だけを足す担当
    float  StarsIntensity;     // 0で完全に無効(1命令も足さない)。昼はCPU側で0になる
    float  StarsDensity;       // 天球を分割するセルの細かさ。1セルにつき星1個
    float  StarsTwinkle;       // またたきの強さ。0で無効
    float  StarsTime;          // またたきの位相に使う時刻[秒]
    float  StarsPixelAngle;    // 1画素が張る角度[rad]。星がこれを下回らないようにして、
                                // サブピクセルのちらつきを防ぐ
};

// SkyParametersの雲用フォグフィールドを埋めるヘルパ。5つあるMakeSkyParametersが
// 同じ5行を書き写さないようにここへ1箇所だけ置く(値渡し+戻り値なのは
// ApplySkyParametersFromBufferと同じ理由=fxcのX3508回避)。
// fogParams0はFrameConstants::FogParams0(x=消散係数, y=スケールハイト, z=基準高度, w=有効フラグ)。
//
// 【viewerHeightは厳密でなくてよい】この値は消散係数を高度で評価するためだけに使う
// (sigma = sigma0 * exp(-(h - refHeight)/scaleHeight))。スケールハイトは既定1,000mなので、
// 数メートルのずれは消散係数を0.1%も動かさない。したがって呼び出し側は次のいずれでもよい:
//   - SSR.hlsl        … 反射レイの起点は水面(y≒0.15m)だがカメラのy(1.6m)を渡している
//   - PlanarReflection.hlsl … CameraPositionは鏡映後のカメラ位置(yが負になる)だが、そもそも
//     このシェーダーはSkyColorUpperしか呼ばずEvaluateCloudLayerへ到達しないため影響が無い
//     (将来SkyColorを呼ぶよう変えるなら、鏡映前のカメラ高さを渡し直すこと)
SkyParameters ApplyCloudFogParameters(SkyParameters params, float4 fogParams0, float viewerHeight)
{
    params.FogEnabled = fogParams0.w;
    params.FogSigma0 = fogParams0.x;
    params.FogScaleHeight = fogParams0.y;
    params.FogRefHeight = fogParams0.z;
    params.FogViewerHeight = viewerHeight;

    // 【星空は既定で無効にする】この5行はフォグとは無関係だが、あえてここへ置いている。
    // HLSLのローカル構造体は代入していないメンバの値が未定義で、5つあるMakeSkyParametersの
    // どれか1つが星のフィールドを埋め忘れると、そのシェーダーはゴミの強度で星を描き始める
    // (IBLキューブへ点光源が焼き込まれ、鏡面反射のファイアフライという分かりにくい形で出る)。
    // このヘルパは5つ全員が必ず通るので、ここで0にしておけば「明示的に有効化した
    // シェーダーだけが星を描く」という安全側の既定になる。
    // 星を出すシェーダー(DeferredLighting.hlsl / SSR.hlsl)は、この呼び出しの**後**で上書きすること
    params.StarsIntensity = 0.0f;
    params.StarsDensity = 0.0f;
    params.StarsTwinkle = 0.0f;
    params.StarsTime = 0.0f;
    params.StarsPixelAngle = 0.0f;
    return params;
}

// バッファの内容をSkyParametersのティント/輝度フィールドへ流し込むヘルパ。
// SkyGenerate.hlsl/DeferredLighting.hlsl/SSR.hlslの3つの消費側が同じ詰め替えを
// 個別に書かないようにするため、ここへ1箇所だけ置く。SunDirection・雲パラメータ・
// SunToSkyIlluminanceRatio(CPU側SunLightingから来る値)はこのバッファには入っていないため
// 呼び出し側が別途埋めること。
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
    params.PhysicalSkyWeight = data.ModelParams.y;
    params.SkySaturation = data.ModelParams.z;
    // 空照度/天頂輝度の積分値(EvaluateCloudLayerが太陽照度を天頂輝度の単位で表すのに使う。
    // SkyParameters::SkyIlluminanceOverZenithのコメント参照)
    params.SkyIlluminanceOverZenith = data.Luminance.y;
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
// 再発しやすい箇所なので根拠を残す(Preetham側のSkyColorUpperUnitも同じ規則に従うこと)
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

// 太陽高度(のサイン)から空の色味を決める。
//
// 【夜・薄明はアート的な近似】本来の夕焼けは、太陽光が大気を長く通る
// ことで短波長がRayleigh散乱により失われる波長依存の消散で生じる。それを解くには
// Preetham/Hosek-Wilkieのような分光モデルか大気散乱の数値積分が要る。日中
// (太陽仰角5度以上)の色度は物理モデルから導出する(Sky.hlsli冒頭のコメント、
// SkyColorUpperUnit参照)が、物理モデルは太陽が地平線下では定義域外のため、
// 夜・薄明とその間のクロスフェード、および地平線より下の接地色(GroundTint)はこの関数
// (昼・薄明・夜の3セットを高度で補間するアート的な近似)が受け持つ。
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
// SkyView LUTを引くときのdir.yの下限(仰角およそ0.057度)。
// Perezは水平線で発散するので89.5度のクランプ(SkyColorUpperUnitのclampedY)が要ったが、
// Hillaireは特異点を持たないのでここまで下げられる。0にしないのは、大気遠近が下向きの
// 視線に対してもSkyColorUpperを呼ぶため(地平線より下を引くとLUTの地面側のテクセルに入る)
static const float kSkyViewMinDirY = 1e-3f;

// Preetham xyYモデル
//
// 日中(太陽が地平線上、仰角5度以上)の色度(x, y)はPreetham et al.
// 1999のxyYモデルから求め、輝度(Y)と合成してXYZ→線形sRGBへ変換する。夜・薄明(Preethamの定義域外)
// はSkyTintFromSet(アート的な4色補間)を使う(SkyColorUpperUnitの早期脱出/クロスフェード
// 参照)。
//
// xyY→線形sRGB(Rec.709/D65)。負成分ぶんだけ全チャンネルへ白を足すデサチュレーションを
// 入れてあるが、これは保険であって常用される経路ではない。
//
// 【この保険は現状の使用域では一度も発動しない】タービディティ1.7〜8.0 ×
// 太陽仰角5〜60度で上半球を32×64方向に走査しても、負値が出る方向は0.0%である。
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

// 線形sRGB(Rec.709/D65) → 色度(x, y)と輝度Y。XyYToLinearSRGBの逆変換。
// 空の彩度を色度空間で効かせるために使う(SkyView LUTはRGBで返るため、
// 白色点から遠ざける操作をするにはいったん色度へ戻す必要がある)
void LinearSRGBToXyY(float3 rgb, out float x, out float y, out float Y)
{
    const float X = 0.4124f * rgb.r + 0.3576f * rgb.g + 0.1805f * rgb.b;
    Y             = 0.2126f * rgb.r + 0.7152f * rgb.g + 0.0722f * rgb.b;
    const float Z = 0.0193f * rgb.r + 0.1192f * rgb.g + 0.9505f * rgb.b;

    const float sum = X + Y + Z;
    if (sum < 1e-6f)
    {
        // 真っ黒。色度は定義できないので白色点を返す(Yが0なので何を返しても結果は黒)
        x = 0.3127f;
        y = 0.3290f;
        return;
    }
    x = X / sum;
    y = Y / sum;
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

    // 【夜の厳密一致を担保する早期脱出】太陽が地平線下(仰角0度未満)では従来のアート的な
    // ティント補間だけを使う。月光・薄明視(21.9.7)がこの経路に乗っているため、
    // この分岐は物理モデル側の計算を一切行わずに返すので、日中の空のモデルを差し替えても
    // 夜の画素は1ビットも動かない
    if (params.PhysicalSkyWeight <= 0.0f)
    {
        float legacyRelative = max(PerezRelativeLuminance(cosTheta, gamma, thetaSun), 0.0f);
        legacyRelative = kRelativeLuminanceFloor + (1.0f - kRelativeLuminanceFloor) * legacyRelative;
        return legacyRelative * SkyTint(cosTheta, cosGamma, params);
    }

    // --- 日中の空: Hillaire (2020) のSkyView LUT ---
    //
    // 【なぜPreethamではないのか】参考写真と突き合わせると、空の青さはPreetham
    // というモデルの限界に当たる。写真の最も青い空はB/R=4.84だが、
    // Preethamは論文の係数から実装とは独立に計算しても1.34〜1.74しか出さない(実装の実測も
    // この範囲内でモデルに忠実だった)。Rayleigh散乱はλ^-4に比例するので、物理から始めれば
    // B/Rは散乱係数の時点で5.70になる。地平線がマゼンタに寄る癖(Preethamは仰角0.5度で
    // 緑の落ち込みが-7.6)も、Rayleigh/Mie/オゾンを分けて持てば構造的に起きない。
    //
    // LUTは天頂のRec.709輝度が1になるよう正規化して焼いてあるので、
    // 「天頂輝度を1としたときの空の色」というこの関数の規約はPreetham時代と同じまま
    // (正規化の必要性はAtmosphereLUT.hlslのSkyViewセクション冒頭に書いてある)。
    //
    // 【地平線のクランプ】Perezは水平線で発散するためclampedY(仰角0.5度)が要ったが、
    // Hillaireは特異点を持たないのでもっと下まで引ける。ただし大気遠近は下向きの
    // 視線に対してもSkyColorUpperを呼ぶため(遠くの地物のin-scatterに地平線際の空の色を
    // 使う)、クランプ自体は残して「地平線のすぐ上」へ写す必要がある。
    // SkyViewDirectionToUvはdir.yを天頂角の余弦、dir.xzを方位として独立に読むので、
    // yだけ差し替えた非正規化のベクトルを渡してよい(下向き真下でも方位の退化処理へ落ちる)
    const float skyViewY = max(dir.y, kSkyViewMinDirY);
    const float2 skyViewUv =
        SkyViewDirectionToUv(float3(dir.x, skyViewY, dir.z), params.SunDirection);
    float3 physicalColor =
        max(SkyViewLUTTexture.SampleLevel(ColorSampler, skyViewUv, 0.0f).rgb, 0.0f);

    // 【空の彩度(アート指定)】色度図上で白色点(D65)から遠ざけ、色相と輝度を保ったまま
    // 鮮やかさだけを上げ下げする。1.0で無変換。
    //
    // 空の青さは物理モデル側で足りるため、このつまみは「届かなかったときの逃げ道」でしかない。
    // **仰角による重み付け(地平線際では効かせない)を足してはいけない**。それはPreethamの
    // 地平線がマゼンタに寄る癖を増幅しないための回避策であって、その癖が無いモデルでは
    // 根拠が無い。地平線の緑の落ち込みの実測で妥当性を確認すること
    if (params.SkySaturation != 1.0f)
    {
        float chromaX, chromaY, chromaLuminance;
        LinearSRGBToXyY(physicalColor, chromaX, chromaY, chromaLuminance);
        const float2 kWhitePointD65 = float2(0.3127f, 0.3290f);
        const float saturatedX = kWhitePointD65.x + (chromaX - kWhitePointD65.x) * params.SkySaturation;
        const float saturatedY = kWhitePointD65.y + (chromaY - kWhitePointD65.y) * params.SkySaturation;
        physicalColor = XyYToLinearSRGB(saturatedX, saturatedY, chromaLuminance);
    }

    // 完全に昼(仰角5度以上)なら従来ティントの計算は行わずそのまま返す(コスト削減)
    if (params.PhysicalSkyWeight >= 1.0f)
    {
        return physicalColor;
    }

    // 薄明の遷移域(仰角0〜5度): 従来ティントと物理モデルをクロスフェードする。
    // 夜(仰角0度未満)は上の早期脱出で従来ティントのみになる
    float legacyRelative = max(PerezRelativeLuminance(cosTheta, gamma, thetaSun), 0.0f);
    legacyRelative = kRelativeLuminanceFloor + (1.0f - kRelativeLuminanceFloor) * legacyRelative;
    const float3 legacyColor = legacyRelative * SkyTint(cosTheta, cosGamma, params);
    return lerp(legacyColor, physicalColor, params.PhysicalSkyWeight);
}

// 水平線以上を仮定した空の色(呼び出し側で地面フェードと合成する)
float3 SkyColorUpper(float3 dir, SkyParameters params)
{
    return params.ZenithLuminance * SkyColorUpperUnit(dir, params);
}

// ============================================================================
// 雲(積雲と巻雲の2層のレイヤーモデル)
//
// 【判断A: IBL用キューブマップには雲を焼かない】
// SkyGenerate.hlslはSkyParameters組み立て時にCloudCoverage=CirrusCoverage=0で埋めて呼ぶため、
// この節の関数はIBLベイクの経路では一切実行されない。雲を焼き込むと、雲が風で動くたびに
// キューブの焼き直し(空生成6回+プリフィルタ36回のディスパッチ)が必要になるが、上半球の
// 平均照度は雲の位置が変わってもほぼ不変なのでこの再ベイク連鎖は純粋な無駄になる。加えて
// CPU側の照度正規化(KurenaiEngine3D.cpp ComputeSkyZenithScale、16,384サンプルの積分)は
// 雲を知らないため、雲を焼き込むと「正規化の目標」と「実際に焼かれた明るさ」が食い違う。
// CPUにfBmを実装して同期させるのは負債が大きすぎるため、IBLは常に雲のない晴天のまま焼く。
// この判断は巻雲(2層目)にもそのまま適用する。
//
// 【判断B: 雲による減光はキューブのベイク時にだけ掛ける】
// 判断Aの結果、IBLは常に晴天基準の明るさになる。被覆率50%の空で島が晴天と同じ明るさに
// 照らされるのは不自然なため、KurenaiEngine3D.cppのRender()がキューブへ焼くSkyBakeConstants::
// ZenithLuminanceにだけ平均透過率(被覆率から求める近似。SkyBakeConstants側のコメント参照)を
// 掛けて全体を暗くする。**このSkyParameters::ZenithLuminance(背景・水面反射へ渡る値)は
// 減光しない**——ここも減光すると、雲の隙間から見える青空まで暗くなり、そこへ下のSkyColorで
// さらに雲を重ねることで二重に暗くなってしまう。巻雲(2層目)についても同じ理由で、
// KurenaiEngine3D.cppのComputeCloudAverageTransmittanceが2層の透過率の積を返す形へ拡張してあり、
// 積雲と同じくベイク時にだけ掛かる(CirrusOvercastTransmittance参照)。
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
// ステップ数はシェーダ内定数なのでコストを測る側が調整できる。
// ステップ数はCloudLayerParams::ShadowStepsとして層ごとに渡す
// (巻雲は0を渡し自己影を完全にスキップする。kCirrusShadowSteps参照)。距離(SpanMeters)と
// 太陽側消光倍率(下のkCloudSunExtinctionScale)は両層で式を共有するため定数のまま据え置く
static const int kCumulusShadowSteps = 5;
static const float kCloudShadowSpanMeters = 1500.0f;

// 巻雲の前方散乱パラメータと自己影ステップ数。UI(CPU側)ではなくシェーダ内定数にしてある
// 理由はSkyParameters::CirrusAnisotropyのコメント・KurenaiEngine3D.h側のコメント参照
// (cbufferを増やす価値がないため)。
// 【自己影を0にする理由】巻雲は光学的に薄く(CirrusDensityは積雲の1桁下)、自己影がほとんど
// 見た目に効かない。EvaluateCloudLayerはShadowSteps==0のとき自己影の計算を完全にスキップし
// sunTransmittance=1.0として扱う(kCloudShadowSpanMeters等をfBm評価に使わずコストを払わない)
static const float kCirrusForwardG = 0.3f;
static const int kCirrusShadowSteps = 0;

// 太陽方向の消散係数へ掛ける倍率。
// 【太陽方向の光学的深さをメートルで積んではいけない】視線側の光路長は「層を斜めに貫く倍率」
// という無次元量なので、太陽側だけ「密度 × 消散係数 × ステップ距離[m]」で積むと、
// 1ステップだけで光学的深さが数百に達して自己影が常に飽和し、雲の芯と縁の区別が数値上
// まったく付かなくなる(画素が例外なく単一の灰色になる)。
// 太陽側も同じ無次元量(1/sin(太陽仰角))で積み、そのうえで太陽光は雲の上面から入って
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
// 【多重散乱の項を定数1つにしてはいけない】太陽から離れた方向では位相関数の値が
// 等方散乱比0.23まで落ちるため単散乱の寄与が全体の1割に満たず、定数にすると雲の芯が
// 単一の値に張り付いて立体感が出ない。多重散乱も厚みで減衰する量なので、
// 自己影の透過率で下限〜上限を補間する形にしてある
// (物理的な導出ではなく、厚い芯が暗く薄い縁が明るいという積雲の見え方に合わせた近似)。
// これらはCloudLayerParams::Albedo/SingleScatterScale/AmbientTermMin/Maxとして層ごとに渡す。
// 式(EvaluateCloudLayer)は1箇所のまま、値だけを積雲・巻雲で変える。
//
// 【雲の明るさの基準はzenithLuminance(青空の天頂輝度)ではなく太陽照度】
// 青空基準にすると雲は原理的に青空より暗くしかならず、日向の積雲でも曇り空のような
// 灰色にしかならない。実際の日向の積雲は青空の3〜5倍明るく、雲を照らしているのは
// 空ではなく太陽だからである。この3定数の目標は「太陽から離れた方向で、
// 完全に照らされた雲(sunTransmittance=1、薄い縁)の輝度が青空の天頂輝度のおよそ3〜4倍に
// なること」と「厚い芯(sunTransmittance=0)ではその15%程度に留まること」で、
// この3つの値はその狙いから逆算した出発点であり実測値ではない
static const float kCumulusAlbedo = 1.0f;
static const float kCumulusSingleScatterScale = 1.0f;   // 0.35から変更
static const float kCumulusAmbientTermMin = 0.15f;      // 0.25から変更
static const float kCumulusAmbientTermMax = 0.75f;      // 据え置き

// 巻雲側の値。巻雲はkCirrusShadowSteps=0のためsunTransmittanceが常に1.0になり、
// AmbientTermMin側は事実上使われない(lerp(Min,Max,1.0)=Max)が、式を1箇所に保つため
// フィールド自体はCloudLayerParamsに残し、Min=Maxとして無効化しておく。
// 単散乱強度(SingleScatterScale)を積雲よりやや強めにしてあるのは、巻雲は氷晶による
// 前方散乱が卓越し薄い縁が霞むように光る見た目を意図した調整値であり、実測値ではない
static const float kCirrusAlbedo = 1.0f;
static const float kCirrusSingleScatterScale = 0.5f;
static const float kCirrusAmbientTermMin = 0.4f;
static const float kCirrusAmbientTermMax = 0.4f;

// このファイル内だけで使うPI。DeferredLighting.hlsl/SSR.hlsl側の`PI`とは別名にしてあるため
// (ファイル冒頭のコメントのとおりSky.hlsliはPIを再定義しない、という既存の方針を守るため)、
// インクルード順によらず再定義エラーは起きない
static const float kCloudPI = 3.14159265359f;

// 雲の日陰側(自己影で太陽光がほとんど届かない雲底)を照らす空明かりの強さ。
// 雲の明るさの基準を太陽照度へ変えたことで単散乱・多重散乱ともsunTransmittanceに
// 比例するようになったため、太陽が完全に遮られる(sunTransmittance→0)と光源そのものが
// 無くなってしまう。実際には空全体からの拡散光が雲底にも回り込むため、この項だけは
// 従来どおり天頂輝度基準で残す。0.2fは見た目からの調整値であり実測値ではない
static const float kCloudSkyAmbientTerm = 0.2f;

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

// 1層ぶんの雲パラメータ。EvaluateCloudLayerはこの構造体を受け取ることで、積雲・巻雲の
// 式を1箇所(EvaluateCloudLayer本体)に保ったまま値だけを層ごとに変える。
// SkyParametersの層ごとのスカラー/ベクトルからこの構造体を組み立てるのはMakeCumulusLayerParams/
// MakeCirrusLayerParams(このすぐ下)の役目
struct CloudLayerParams
{
    float  Coverage;
    float  Altitude;         // 雲底の高度[m]
    float  UvScale;          // ワールド1mあたりのノイズ空間の距離
    float  Density;          // 消散係数
    float2 ScrollOffset;
    float  ForwardG;
    float2 AnisotropicScale; // fBmのUVを異方的に伸ばす倍率(積雲は(1,1)、巻雲は筋状にする)
    int    ShadowSteps;      // 自己影の積分ステップ数。0なら自己影を計算しない
    // 雲底から雲頂までの厚み[m]。**0なら従来の厚みゼロの平面として扱う**。
    // 巻雲は0を入れるため、巻雲は平面の経路だけを通る
    float  Thickness;
    // アルベド・単散乱強度・多重散乱の下限/上限。式はEvaluateCloudLayerの1箇所だけで、
    // 値だけを層ごとに変える(kCumulusAlbedo等/kCirrusAlbedo等のコメント参照)
    float  Albedo;
    float  SingleScatterScale;
    float  AmbientTermMin;
    float  AmbientTermMax;
};

// 積雲(1層目、下層)のCloudLayerParamsを組み立てる。SkyParameters::Cloud*をそのまま渡すだけ
CloudLayerParams MakeCumulusLayerParams(SkyParameters params)
{
    CloudLayerParams layer;
    layer.Coverage = params.CloudCoverage;
    layer.Altitude = params.CloudAltitude;
    layer.UvScale = params.CloudUvScale;
    layer.Density = params.CloudDensity;
    layer.ScrollOffset = params.CloudScrollOffset;
    layer.ForwardG = params.CloudForwardG;
    layer.AnisotropicScale = float2(1.0f, 1.0f); // 積雲は等方(筋状にしない)
    layer.ShadowSteps = kCumulusShadowSteps;
    layer.Thickness = params.CloudThickness; // 0でなければスラブをレイマーチする
    layer.Albedo = kCumulusAlbedo;
    layer.SingleScatterScale = kCumulusSingleScatterScale;
    layer.AmbientTermMin = kCumulusAmbientTermMin;
    layer.AmbientTermMax = kCumulusAmbientTermMax;
    return layer;
}

// 巻雲(2層目、上層)のCloudLayerParamsを組み立てる。前方散乱・自己影ステップ数は
// UIつまみを持たずシェーダ内定数(kCirrusForwardG/kCirrusShadowSteps参照)
CloudLayerParams MakeCirrusLayerParams(SkyParameters params)
{
    CloudLayerParams layer;
    layer.Coverage = params.CirrusCoverage;
    layer.Altitude = params.CirrusAltitude;
    layer.UvScale = params.CirrusUvScale;
    layer.Density = params.CirrusDensity;
    layer.ScrollOffset = params.CirrusScrollOffset;
    layer.ForwardG = kCirrusForwardG;
    // 【巻雲は厚みゼロのまま】巻雲は光学的に薄いシート状で、レイマーチする値が無い。
    // 0を入れることで平面の経路を通す
    layer.Thickness = 0.0f;
    layer.AnisotropicScale = float2(params.CirrusAnisotropy, 1.0f); // U方向だけ伸ばして筋状にする
    layer.ShadowSteps = kCirrusShadowSteps;
    layer.Albedo = kCirrusAlbedo;
    layer.SingleScatterScale = kCirrusSingleScatterScale;
    layer.AmbientTermMin = kCirrusAmbientTermMin;
    layer.AmbientTermMax = kCirrusAmbientTermMax;
    return layer;
}

// 雲(1層ぶん)の透過率と散乱光を求める。呼び出し側でdir.y > 0を確認してから呼ぶこと。
// 【層のパラメータは引数で渡す】SkyParametersから直接params.Cloud*を読むと積雲・巻雲で式を
// 複製することになるため、層ごとの設定をCloudLayerParamsへまとめて渡す。
// sunDirection/zenithLuminanceは層に依らずSkyParametersの値をそのまま渡すだけなので、
// CloudLayerParamsへは含めず別引数のままにしてある。
// sunToSkyIlluminanceRatio/skyIlluminanceOverZenithは雲の明るさを太陽照度基準にするための
// 係数(SkyParameters::SunToSkyIlluminanceRatio/SkyIlluminanceOverZenith参照)。SkyParameters
// 全体ではなくこの2つだけを個別の引数にしているのは、既存のsunDirection/zenithLuminanceと
// 同じ「層に依らない値は個別の引数で渡す」規約に揃えるため
// ============================================================================
// ボリュメトリック積雲
//
// 積雲だけを、雲底(Altitude)から雲頂(Altitude + Thickness)までのスラブとしてレイマーチする。
// Thickness == 0 の層(巻雲)は従来どおり厚みゼロの平面として扱い、下の EvaluateCloudLayer の
// 平面分岐をそのまま通る。
//
// 【密度の組み立て】3つを掛け合わせる:
//   ウェザーマップ … 既存の2次元 CloudFbm。「どこにどれだけ雲があるか」の平面分布。
//                    CloudUvScale=1/1000・CloudCoverage=0.40 の意味を保つため、
//                    ここは3Dノイズに置き換えず残す
//   形状(3D)       … CloudNoiseGenerate.hlsl が焼いた 128^3。塊の3次元的な形
//   高さプロファイル … 積雲の「平らな底・丸い頭」を作る解析的な形状関数
// さらにディテール(32^3)で縁だけを削り、房状の輪郭にする。
//
// 【3Dテクスチャのレジスタはインクルードする側が決める】このヘッダーは cbuffer にも
// レジスタにも依存しない方針なので、DDGI.hlsli と同じくマクロで受け取る。
// 定義しなかったシェーダー(SkyGenerate/AerialPerspective/PlanarReflection)では
// ボリュームの経路がコンパイルされず、平面の経路だけが残る。
//   KURENAI_CLOUD_SHAPE_REGISTER    形状ノイズ(Texture3D)
//   KURENAI_CLOUD_DETAIL_REGISTER   ディテールノイズ(Texture3D)
// ============================================================================
#ifdef KURENAI_CLOUD_SHAPE_REGISTER
#include "Samplers.hlsli"
Texture3D CloudShapeNoiseTexture : register(KURENAI_CLOUD_SHAPE_REGISTER);
Texture3D CloudDetailNoiseTexture : register(KURENAI_CLOUD_DETAIL_REGISTER);
#define KURENAI_CLOUD_VOLUME 1
#endif

// レイマーチのステップ数。**コストの唯一のつまみ**なので、負荷を調整するときはここを動かす
// (cbufferを増やさないためシェーダ内定数にしてある。kCirrusShadowStepsと同じ扱い)
static const int kCumulusRaymarchSteps = 12;

// 形状ノイズがワールド空間で1周する距離[m]。雲の塊(ウェザーマップの1セル=1,000m)より
// 大きくしておくと、同じ模様が隣の雲で繰り返されているのが読み取りにくくなる
static const float kCloudShapeWorldPeriod = 4000.0f;
// ディテールノイズが1周する距離[m]。縁を削るための高周波成分なので形状より1桁細かい
static const float kCloudDetailWorldPeriod = 400.0f;
// ディテールがスラブの厚み方向に何周するか。横方向と同じ密度の細かさを縦にも持たせる
static const float kCloudDetailVerticalRepeat = 3.0f;
// ディテールで縁を削る強さ。0で削らない(形状そのまま)、大きいほど輪郭が房状に痩せる
static const float kCloudDetailErode = 0.35f;
// 3Dの変調(形状 × 高さプロファイル × ディテールの浸食)のスラブ内平均を1へ揃える係数。
// **見た目の調整値ではなく実測から逆算した値**で、CloudNoiseGenerate.hlslと同じ式を
// オフラインで再現し、スラブ内を一様に40万点サンプルして平均を求めた:
//   base の平均           = 0.781 (標準偏差 0.055)
//   高さプロファイルの平均 = 0.676
//   base × プロファイル    = 0.528
//   ↑をディテール(浸食0.35)で削った後 = 0.458  → 1/0.458 = 2.183
// これを掛けることでスラブ全体の光学的深さの平均が平面レイヤーのときと一致し、
// CloudDensityと判断B(被覆率→平均透過率)の意味がどちらも変わらない。
// 【kCloudDetailErode・高さプロファイルの形・ノイズの生成方法のいずれかを変えたら測り直すこと】
static const float kCloudVolumeDensityNormalize = 4.315f;
// 形状ノイズにコントラストを付ける範囲。**実測した分布から決めた値**で、
// remap(shape.r, WorleyFbm(gba) - 1, 1) の出力は平均0.784・標準偏差0.055とほとんど定数だった
// (下限が -0.6 付近になるため [0,1] が [0.375, 1] へ圧縮される)。このままだと3Dの形が
// ほとんど効かないので、実測の15%点(0.726)〜92%点(0.856)を[0,1]へ引き伸ばす。
// 変換後は平均0.455・標準偏差0.329となり、雲の芯と隙間がはっきり分かれる。
// 【この2つを変えたら上のkCloudVolumeDensityNormalizeを測り直すこと】
static const float kCloudShapeContrastLow = 0.726f;
static const float kCloudShapeContrastHigh = 0.856f;

// 高さプロファイルの形。積雲は「平らな底・丸い頭」なので、下端は短く立ち上げ、
// 上端は長くなだらかに落とす。いずれも見た目からの調整値で物理的な導出ではない
static const float kCloudProfileBaseSoftness = 0.10f;
static const float kCloudProfileTopStart = 0.45f;

// 雲頂ほど明るく雲底ほど暗いという勾配を作る解析項の強さ(EvaluateCloudLayerの自己影参照)。
// 「サンプルより上にどれだけ雲が残っているか」を厚みに比例した光学的深さとして扱う係数。
//
// 【桁に注意】この係数はCloudDensity(既定8.0)と1/sin(太陽仰角)(仰角50度で1.3)に
// 掛かるため、1.2のような値にすると雲底(hf=0)での光学的深さが 8.0 × 1.2 × 1.3 ≒ 12 に
// なって透過率が事実上0、つまり雲底が真っ黒になる。
// 視線方向の自己影がkCloudSunExtinctionScale=0.12という小さい係数を使っているのと
// 同じ桁に揃えること
static const float kCloudTopShadowScale = 0.12f;

// 積雲の高さプロファイル。hf=0が雲底、hf=1が雲頂
float CloudVerticalProfile(float hf)
{
    return smoothstep(0.0f, kCloudProfileBaseSoftness, hf) * (1.0f - smoothstep(kCloudProfileTopStart, 1.0f, hf));
}

// 3チャンネルのWorleyを1つのfBmへまとめる。重みはCloudNoiseGenerate.hlsl側で
// オクターブへ与えたのと同じ等比(0.625 : 0.25 : 0.125)にしてある
float CloudWorleyFbmFromChannels(float3 channels)
{
    return dot(channels, float3(0.625f, 0.25f, 0.125f));
}

#if KURENAI_CLOUD_VOLUME
// スラブ内の1サンプルの密度。
//   noiseXZ … 2次元ノイズ空間での位置(= サンプルのXZ * UvScale + ScrollOffset)。
//              風のスクロールが既に入っているので、3Dテクスチャ側もこれを流用するだけで
//              同じ速度・同じ向きに流れる
//   hf      … スラブ内の高さ(0=雲底、1=雲頂)
//   weather … ウェザーマップの値(既存の2次元 CloudFbm を被覆率で整形したもの)
//
// 【設計: 3Dノイズは密度を「作る」のではなく「再分配する」】
// この関数は weather に 3D の変調を掛けた値を返し、その変調はスラブ内の平均が1になるよう
// 正規化してある(kCloudVolumeDensityNormalize)。したがってスラブ全体の光学的深さの平均は
// 平面レイヤーのときと一致し、CloudDensity(消散係数)と判断B(被覆率→平均透過率)の意味が
// どちらも変わらない。3Dノイズが変えるのは「同じ量の雲が高さ方向にどう分布するか」だけである。
//
// 【3Dの形をウェザーマップでもう一度しきい値処理してはいけない】
// saturate(CloudRemap(base, 1 - weather, 1)) のように書くと二重に閾値を掛けることになる。
// weather は既に被覆率で整形済み(大半の方向で0に近い)なので 1-weather はほぼ1になり、
// base(平均0.781・標準偏差0.055とほぼ定数)を上回れず密度が常に0へ落ちる。
float CloudSampleDensity(float2 noiseXZ, float hf, float weather, CloudLayerParams layer)
{
    // 3Dテクスチャは Wrap で引くので範囲を気にせず割るだけでよい。
    // 分母に layer.UvScale を含めるのは、noiseXZ が既に UvScale を掛けた空間にいるため
    const float shapeScale = 1.0f / max(kCloudShapeWorldPeriod * layer.UvScale, 1e-9f);
    // W(奥行き)にはスラブ内の高さをそのまま入れる。厚み全体でテクスチャの奥行きを1周ぶん
    // 使い切るので、雲底から雲頂まで途中で同じ断面が繰り返されない
    const float3 shapeUvw = float3(noiseXZ * shapeScale, hf);
    const float4 shape = CloudShapeNoiseTexture.SampleLevel(VolumeSampler, shapeUvw, 0.0f);

    // Perlin-Worley(R)を、周波数を上げたWorley(GBA)を下限として引き伸ばす。
    // Rだけだと塊が丸すぎ、Worleyを重ねることで綿状の輪郭になる
    const float shapeFbm = CloudWorleyFbmFromChannels(shape.gba);
    const float rawBase = saturate(CloudRemap(shape.r, shapeFbm - 1.0f, 1.0f));
    // rawBase はほとんど定数(実測: 平均0.784・標準偏差0.055)なので、実測した分布の
    // 15%点〜92%点を[0,1]へ引き伸ばしてコントラストを付ける(定数のコメント参照)
    const float base = saturate(CloudRemap(rawBase, kCloudShapeContrastLow, kCloudShapeContrastHigh));

    // 高さプロファイルで「平らな底・丸い頭」に整形する
    const float shaped = base * CloudVerticalProfile(hf);
    if (shaped <= 0.0f)
    {
        // 雲底の直下と雲頂の直上ではプロファイルが0になる。ここで抜けると
        // ディテール(2枚目のテクスチャフェッチ)を丸ごと省ける
        return 0.0f;
    }

    // ディテールで縁だけを削る。密度が高い芯はほとんど削れず、薄い縁だけが房状に痩せる
    const float detailScale = 1.0f / max(kCloudDetailWorldPeriod * layer.UvScale, 1e-9f);
    const float3 detailUvw = float3(noiseXZ * detailScale, hf * kCloudDetailVerticalRepeat);
    const float3 detail = CloudDetailNoiseTexture.SampleLevel(VolumeSampler, detailUvw, 0.0f).rgb;
    const float detailFbm = CloudWorleyFbmFromChannels(detail);
    const float modulation = saturate(CloudRemap(shaped, detailFbm * kCloudDetailErode, 1.0f));

    return weather * modulation * kCloudVolumeDensityNormalize;
}
#endif

// 1サンプル(または平面1枚)の散乱光。平面とボリュームの両方から呼ぶため、
// 式は必ずこの1箇所に置くこと
float3 CloudInScatter(
    float sunTransmittance, float phaseNormalized, CloudLayerParams layer, float sunIlluminance,
    float zenithLuminance)
{
    // 単散乱の簡易近似: 自己影を通って弱まった太陽光を位相関数で配分する。
    // 多重散乱の項も同じ自己影の透過率で下限〜上限を補間し、厚い芯が暗く薄い縁が明るくなるようにする
    const float sunLitTerm =
        sunTransmittance * phaseNormalized * layer.SingleScatterScale
        + lerp(layer.AmbientTermMin, layer.AmbientTermMax, sunTransmittance);

    // 日陰側(自己影で太陽光が届かない雲底)は空の光だけで照らされる。この項だけは
    // 天頂輝度基準で残す(kCloudSkyAmbientTermのコメント参照)。1/PIはランバート面の輝度換算
    return layer.Albedo * (sunLitTerm * sunIlluminance / kCloudPI + kCloudSkyAmbientTerm * zenithLuminance);
}

// 雲へ掛ける大気遠近の設定。SkyParametersの該当5フィールドをそのまま束ねたもの。
// 【なぜ5つのスカラーを個別に渡さずに束ねるか】既存の規約は「層に依らない値は個別の引数で渡す」
// (sunDirection/zenithLuminance等)だが、それは1〜2個だから成り立つ書き方で、5つ増やすと
// 引数列だけで順番を間違えやすくなる。CloudLayerParamsと同じく「意味のまとまりを1つの型にする」
// 側へ寄せた。層に依らない値である点は変わらないので、SkyColorが1回だけ組み立てて両層へ渡す
struct CloudFogParams
{
    float Enabled;      // 0=無効(EvaluateCloudLayerはフォグの計算を一切行わない)
    float Sigma0;       // 基準高度での消散係数[1/m]
    float ScaleHeight;  // スケールハイト[m]
    float RefHeight;    // 基準高度[m](ワールドY)
    float ViewerHeight; // 視点のワールドY(SkyParameters::FogViewerHeightのコメント参照)
};

CloudFogParams MakeCloudFogParams(SkyParameters params)
{
    CloudFogParams fog;
    fog.Enabled = params.FogEnabled;
    fog.Sigma0 = params.FogSigma0;
    fog.ScaleHeight = params.FogScaleHeight;
    fog.RefHeight = params.FogRefHeight;
    fog.ViewerHeight = params.FogViewerHeight;
    return fog;
}

void EvaluateCloudLayer(
    float3 dir, CloudLayerParams layer, float3 sunDirection, float zenithLuminance,
    float sunToSkyIlluminanceRatio, float skyIlluminanceOverZenith, CloudFogParams fog,
    out float transmittance, out float3 scatteredLight)
{
    // (d) 光路長。dir.yが小さいほど視線は雲底平面を浅い角度で貫き経路が伸びるため、
    // 1/dir.yに比例させる。kCloudMinDirYへのクランプで地平線際の発散を防ぐ
    const float safeDirY = max(dir.y, kCloudMinDirY);
    const float pathLengthScale = 1.0f / safeDirY;

    // 視線と雲底平面(高度layer.Altitude)の交点のXZ。SkyColorはカメラのワールド位置を
    // 受け取らないため、この交点はカメラを原点とした相対座標になる
    // (Altitudeが「カメラのワールドY基準」である理由。SkyParameters::CloudAltitude参照)
    const float2 hitXZ = dir.xz * (layer.Altitude / safeDirY);
    const float2 uv = hitXZ * layer.UvScale + layer.ScrollOffset;

    // (c) 雲の密度。fBmの出力を被覆率で塊に整形する。Coverage=0ならlo=hi=1になり
    // remapの分子(n-1)は常に0以下、densityは常に0になる(判断Cの根拠の一部。
    // ただし実際にはSkyColorの早期脱出でこの関数自体が呼ばれない)。
    // AnisotropicScaleはここでUVへ掛ける(積雲は(1,1)なので無変化、巻雲はU方向だけ伸びて筋状になる)
    const float n = CloudFbm(uv * layer.AnisotropicScale);
    const float density = saturate(CloudRemap(n, 1.0f - layer.Coverage, 1.0f));

    // 透過率の算出は下の (b) 節で行う(平面かボリュームかで分かれるため)。
    // ここまでで求めた density / uv は平面の経路がそのまま使う

    // 自己影: 雲底のUVから太陽方向へlayer.ShadowSteps段、densityを積分してビアの法則で
    // 太陽光の減衰(sunTransmittance)を求める。太陽方向はXZへ投影して使う
    // (レイヤーモデルには高度方向の厚みが無いため、太陽の仰角そのものは自己影の
    // ステップ距離に反映できない。割り切り)。
    // 【ShadowSteps==0なら自己影を完全にスキップする】巻雲(kCirrusShadowSteps=0)は光学的に薄く
    // 自己影がほとんど効かないため、コストを払う意味がない。ステップ数がCloudLayerParamsの
    // 実行時の値になったため、下のループは[unroll]ではなく[loop]にしてある
    // (トリップ数がシェーダ内定数でなくなり、コンパイル時に展開できないため)
    // 1/sin(太陽仰角)。自己影だけでなくボリューム経路の「サンプルより上に残っている雲」の
    // 項でも使うため、if の外で求める。
    // 自己影を持たない層(巻雲、ShadowSteps=0)ではこの値は使われないままになる
    const float sunPathLengthScale = 1.0f / max(sunDirection.y, kCloudMinDirY);

    float sunTransmittance = 1.0f;
    if (layer.ShadowSteps > 0)
    {
        const float2 sunDirXZ = normalize(sunDirection.xz + 1e-4f); // 太陽が天頂付近のときのゼロ除算対策
        const float2 shadowStepUv =
            sunDirXZ * (kCloudShadowSpanMeters * layer.UvScale / float(layer.ShadowSteps));
        float shadowDensitySum = 0.0f;
        float2 shadowUv = uv;
        [loop]
        for (int step = 0; step < layer.ShadowSteps; ++step)
        {
            shadowUv += shadowStepUv;
            const float shadowN = CloudFbm(shadowUv * layer.AnisotropicScale);
            shadowDensitySum += saturate(CloudRemap(shadowN, 1.0f - layer.Coverage, 1.0f));
        }
        // 太陽方向の光学的深さも視線側と同じ無次元量で積む(平均密度 × 消散係数 × 1/sin(太陽仰角))。
        // 単位を揃えないと自己影が飽和して雲が一様な灰色になる(kCloudSunExtinctionScaleのコメント参照)
        const float averageShadowDensity = shadowDensitySum / float(layer.ShadowSteps);
        const float sunOpticalDepth =
            averageShadowDensity * layer.Density * kCloudSunExtinctionScale * sunPathLengthScale;
        sunTransmittance = exp(-sunOpticalDepth);
    }

    // Henyey-Greenstein位相関数。cosAngle=1(dirが太陽方向と一致=太陽を直視する向き)で
    // 前方散乱が最大になり、半逆光で雲の縁が光る効果が出る(layer.ForwardGが強さ)
    const float cosAngle = dot(dir, sunDirection);
    const float g = layer.ForwardG;
    const float g2 = g * g;
    const float phaseDenom = pow(max(1.0f + g2 - 2.0f * g * cosAngle, 1e-4f), 1.5f);
    const float phase = (1.0f - g2) / (4.0f * kCloudPI * phaseDenom);

    // 位相関数は立体角で積分すると1になるよう正規化されているため、素の値は1/(4π)≒0.08の
    // オーダーになる。等方散乱を1とした相対値へ直してから重みを掛けないと、単散乱の寄与が
    // 下の多重散乱の下限項に対して2桁小さくなり、太陽側の縁が光る効果がまったく見えなくなる
    const float phaseNormalized = phase * 4.0f * kCloudPI;

    // 【雲の明るさの基準は太陽の照度】太陽照度は「太陽照度/空照度」(CPUのSunLightingから、
    // sunToSkyIlluminanceRatio)と「空照度/天頂輝度」(SkyIntegrate.hlslの積分値、
    // skyIlluminanceOverZenith)の積で、天頂輝度の単位のまま表せる
    const float sunIlluminance = sunToSkyIlluminanceRatio * skyIlluminanceOverZenith * zenithLuminance;

    // (b) 透過率と散乱光。厚みを持つ層(積雲)はスラブをレイマーチし、
    // 厚みゼロの層(巻雲)は従来どおり1枚の平面として扱う。
    //
    // 【厚みゼロの層は平面経路だけを通る】下の #if が無効なシェーダー、および
    // Thickness == 0 の層は else 側の平面経路だけを通る。散乱光の式は CloudInScatter として
    // 両経路で共有する
#if KURENAI_CLOUD_VOLUME
    if (layer.Thickness > 0.0f)
    {
        // --- ボリューム: 雲底から雲頂まで前から後ろへ積分する ---
        const float invSteps = 1.0f / float(kCumulusRaymarchSteps);

        // 【縦方向の影は勾配であって減光ではない】下の aboveTransmittance は
        // exp(-(1-hf) * k) で雲底ほど暗くなる係数だが、これをそのまま掛けるとスラブ内の平均が
        // (1 - exp(-k)) / k ≒ 0.57 になり、雲全体が平面レイヤーより暗くなってしまう
        // (実測: 雲画素の中央輝度が 167.5 → 151 まで落ちていた)。
        // 平均で割って1へ揃えることで、「雲底が暗く雲頂が明るい」という勾配だけを残し、
        // 雲全体の明るさは平面レイヤーと同じに保つ。kはhfに依らないのでここで1回だけ求める
        const float topShadowK = layer.Density * kCloudTopShadowScale * sunPathLengthScale;
        const float topShadowMean = (topShadowK > 1e-4f)
            ? ((1.0f - exp(-topShadowK)) / topShadowK)
            : 1.0f;
        const float topShadowNormalize = 1.0f / max(topShadowMean, 1e-4f);

        float accumTransmittance = 1.0f;
        float3 accumScatter = float3(0.0f, 0.0f, 0.0f);

        [loop]
        for (int marchStep = 0; marchStep < kCumulusRaymarchSteps; ++marchStep)
        {
            // 中点サンプリング。hf=0が雲底、hf=1が雲頂
            const float hf = (float(marchStep) + 0.5f) * invSteps;

            // このサンプルの高度における視線のXZ。**高さとともに横へずれるのが視差の源**で、
            // 仰角45度・厚み1,000mなら雲頂は雲底より1,000m(=ウェザーマップの1セル)横へ動く。
            // hf=0を代入すると平面経路の hitXZ と厳密に一致する
            const float2 sampleXZ = dir.xz * ((layer.Altitude + hf * layer.Thickness) / safeDirY);
            const float2 sampleNoiseXZ = sampleXZ * layer.UvScale + layer.ScrollOffset;

            // ウェザーマップ。ここが0なら3Dテクスチャを1枚も引かずに次のステップへ飛ぶ
            const float weatherN = CloudFbm(sampleNoiseXZ * layer.AnisotropicScale);
            const float weather = saturate(CloudRemap(weatherN, 1.0f - layer.Coverage, 1.0f));
            if (weather <= 0.0f)
            {
                continue;
            }

            const float sampleDensity = CloudSampleDensity(sampleNoiseXZ, hf, weather, layer);
            if (sampleDensity <= 0.0f)
            {
                continue;
            }

            // 【1/kCumulusRaymarchStepsを掛ける理由】こうしておくと全ステップの光学的深さの和が
            // 平面経路の density * Density * pathLengthScale と同じスケールになり、
            // CloudDensity(消散係数)の意味が平面経路と変わらない。判断B(被覆率→平均透過率)を
            // 層の厚みごとに再調整せずに済むのもこのため
            const float stepOpticalDepth = sampleDensity * layer.Density * pathLengthScale * invSteps;
            const float stepTransmittance = exp(-stepOpticalDepth);

            // 【雲底が暗く雲頂が明るい勾配】サンプルより上にまだ残っている雲の厚み(1-hf)を
            // 光学的深さとして解析的に扱う。太陽方向へ3次元にマーチすればより正確だが、
            // 立体感の主要因はこの縦方向の勾配であり、コストを1サンプルあたり0で済ませられる。
            // 横方向の自己影は上で求めた sunTransmittance(視線1本につき1回)がそのまま担う
            const float aboveTransmittance = exp(-(1.0f - hf) * topShadowK) * topShadowNormalize;
            const float3 stepInScatter = CloudInScatter(
                sunTransmittance * aboveTransmittance, phaseNormalized, layer, sunIlluminance,
                zenithLuminance);

            // 前から後ろへの合成。手前で既に遮られたぶん(accumTransmittance)だけ寄与する
            accumScatter += accumTransmittance * (1.0f - stepTransmittance) * stepInScatter;
            accumTransmittance *= stepTransmittance;

            // ほぼ不透明になったら以降のステップは絵に出ない
            if (accumTransmittance < 0.01f)
            {
                break;
            }
        }

        transmittance = accumTransmittance;
        scatteredLight = accumScatter;
    }
    else
#endif
    {
        // --- 平面 ---
        // ビアの法則。経路長はメートル、Density(消散係数)はCPU側UIで調整する無次元の強さ
        const float opticalDepth = density * layer.Density * pathLengthScale;
        transmittance = exp(-opticalDepth);
        // (1-transmittance)は視線の経路のうち実際に散乱へ回った分のスケール
        scatteredLight =
            CloudInScatter(sunTransmittance, phaseNormalized, layer, sunIlluminance, zenithLuminance)
            * (1.0f - transmittance);
    }

    // (e) 地平線際のフェード。kCloudMinDirYによる経路長クランプと合わせてのエイリアシング対策
    float fade = smoothstep(kCloudHorizonFadeEndY, kCloudHorizonFadeStartY, dir.y);

    // (f) 雲へ掛ける大気遠近。
    //
    // 【なぜ要るか】雲は深度を持たない「背景」として描かれるため、AerialPerspective.hlslの
    // 早期脱出(depth <= 0)に入りフォグを一切受けていなかった。しかし雲は無限遠ではなく
    // layer.Altitudeの有限距離にある層で、視線と雲底平面の交点までの斜距離は
    // Altitude/dir.y——仰角が下がるほど急速に伸びる(積雲1,500mなら仰角45度で2.1km、
    // 15度で5.8km)。既定の消散係数0.0004(視程10km)でも仰角45度で透過率0.65、15度で0.30に
    // なるはずのものが、素通しで最大コントラストのまま出ていた。結果、消散係数を上げると
    // 「地物は溶けたのに雲だけ剃刀のようにくっきり」という、霞ではなく地物だけが
    // 透けたように見える絵になっていた。
    //
    // 【なぜ(e)のフェードと同じ形で掛けるか】fadeは「この層の存在感を0〜1で薄める」係数で、
    //   透過率   = lerp(1, T, fade)   … 薄まるほど背後の空を遮らなくなる
    //   散乱光   = S * fade           … 薄まるほど自分の輝きが届かなくなる
    // という構造を既に持つ。フォグの効果もまったく同じ構造で表せる——雲までの透過率をfとすると、
    // 雲から目へ届く輝きはf倍に減り、雲が背後を遮る度合いも手前の大気光(airlight)で
    // 薄まってlerp(1, T, f)になる。しかも減った分を埋める大気光の等価輝度は、この関数の
    // 呼び出し元(SkyColor)が掛けているclearColor(晴天の空色)そのものなので、
    // 追加の項を持たずに済む。実際、f=0を代入するとT=1・S=0となり
    // SkyColorの合成式 clearColor * T + S は clearColor に一致する——
    // 「フォグで雲が完全に消えたら素の空色が見える」という正しい極限になる。
    //
    // 【FogEnabled=0なら1命令も走らせない】この分岐に入らなければfadeは(e)の値のままで、
    // フォグを持たない場合と浮動小数の最下位ビットまで一致する
    if (fog.Enabled > 0.5f)
    {
        // 視線と雲底平面の交点までの斜距離。|dir|=1なので Altitude/safeDirY がそのまま距離になる
        // (上の(d)節でhitXZを求めたときと同じ交点。safeDirYのクランプもそのまま効く)。
        // 雲底高度はカメラ基準の相対値なので、絶対高度は ViewerHeight + layer.Altitude
        const float3 rayStart = float3(0.0f, fog.ViewerHeight, 0.0f);
        const float3 rayEnd = rayStart + dir * (layer.Altitude / safeDirY);
        fade *= HeightFogTransmittance(rayStart, rayEnd, fog.Sigma0, fog.ScaleHeight, fog.RefHeight);
    }

    transmittance = lerp(1.0f, transmittance, fade);
    scatteredLight *= fade;
}

// ============================================================================
// 星空
//
// 視線方向を立方体の面へ射影してセル格子へ量子化し、1セルにつき星を1つ、セル内の
// 決定的な位置へ置く。テクスチャを使わないのは背景が画面解像度で解析評価される経路
// (DeferredLighting.hlslのSkyParams.y=1)に乗せるためで、こうすると星が拡大されず
// 常にシャープに出る。
//
// 【なぜSkyColorUpperUnitではなくSkyColorへ足すのか】SkyColorUpperUnitの結果は
// SkyIntegrate.hlslが積分して天頂輝度(=夜空の露出校正)を逆算する入力になっている
// (このファイルのSkyColorUpperUnit手前の【重要】コメント参照)。そちらへ星を混ぜると
// 校正値そのものが動き、星の有無で夜空全体の明るさが変わってしまう。
// 星は「校正済みの空の色へ後から足す発光体」として扱うのが正しい
// ============================================================================

// セル座標から決定的な擬似乱数を3つ作る
float3 StarHash3(float2 cell, float faceId)
{
    float3 p = float3(cell, faceId);
    p = frac(p * float3(0.1031f, 0.1030f, 0.0973f));
    p += dot(p, p.yzx + 33.33f);
    return frac((p.xxy + p.yzz) * p.zyx);
}

float3 EvaluateStarfield(float3 dir, SkyParameters params)
{
    // 【昼と無効時はここで抜ける】判断C(雲が無いときP4完了時点と画素まで一致する)と
    // 同じ考え方で、効かない条件では掛け算・足し算を1つも増やさない
    if (params.StarsIntensity <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    // 地平線際は大気の消散が効いて実際に星が見えなくなるので落とす。
    // ここで落としておくと、水平線より下へのフェード(SkyColorの後半)との境目も自然につながる
    const float horizonFade = saturate(dir.y * 6.0f);
    if (horizonFade <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    // 天球を立方体の6面へ射影する。球面座標(緯度経度)で切ると極で密度が跳ね上がるが、
    // 立方体面なら面内の歪みが高々√3倍に収まり、密度がおおむね一様になる
    const float3 a = abs(dir);
    float2 uv;
    float faceId;
    if (a.x >= a.y && a.x >= a.z)      { uv = dir.zy / a.x; faceId = dir.x > 0.0f ? 0.0f : 1.0f; }
    else if (a.y >= a.z)               { uv = dir.xz / a.y; faceId = dir.y > 0.0f ? 2.0f : 3.0f; }
    else                               { uv = dir.xy / a.z; faceId = dir.z > 0.0f ? 4.0f : 5.0f; }

    const float density = max(params.StarsDensity, 1.0f);
    const float2 scaledUv = uv * density;
    const float2 baseCell = floor(scaledUv);

    // 面のUVは[-1,1]で90度を張るので、1UVあたりおよそ0.785rad。
    // 星の見かけの半径が1画素を下回るとカメラを回したときにちらつくため、下限を設ける
    const float uvPerRadian = density / 0.7854f;
    const float minRadius = max(params.StarsPixelAngle * uvPerRadian * 1.2f, 0.03f);

    float3 result = float3(0.0f, 0.0f, 0.0f);

    // 隣接セルも見る。セル境界に近い星が片側からしか描かれないと、
    // 格子状の切れ目が空に浮き出てしまう
    for (int oy = -1; oy <= 1; ++oy)
    {
        for (int ox = -1; ox <= 1; ++ox)
        {
            const float2 cell = baseCell + float2(ox, oy);
            const float3 h = StarHash3(cell, faceId);

            // 【全セルに星を置かない】等級分布を作る前に間引く。1セル1個をそのまま全部
            // 描くと空が均一な砂目になり、星座のような粗密が出ない
            if (h.z > 0.55f)
            {
                continue;
            }

            // セル内の位置。端に寄りすぎると隣のセルの星と重なるので中央寄りへ詰める
            const float2 starPos = cell + 0.5f + (h.xy - 0.5f) * 0.7f;
            const float2 delta = scaledUv - starPos;
            const float dist = length(delta);

            // 等級分布。h.zを6乗して「暗い星が大多数、明るい星はごくわずか」にする。
            // 実際の星の等級分布も明るい星ほど指数的に少ない
            const float brightRandom = h.z / 0.55f;
            const float magnitude = pow(1.0f - brightRandom, 4.0f);

            // 明るい星ほど大きく見える(実際は目とレンズの滲みによる見かけの効果)
            const float radius = minRadius * (1.0f + magnitude * 1.5f);
            if (dist >= radius)
            {
                continue;
            }

            float falloff = saturate(1.0f - dist / radius);
            falloff = falloff * falloff;

            // 色温度。青白い星から橙色の星まで。h.xを使い回すと位置と色が相関するので
            // 別の成分(h.y)から作る
            const float3 warm = float3(1.00f, 0.80f, 0.62f);
            const float3 cool = float3(0.72f, 0.82f, 1.00f);
            float3 starColor = lerp(warm, cool, h.y);

            // またたき。既定は0で、その場合この行は結果を変えない
            if (params.StarsTwinkle > 0.0f)
            {
                const float phase = (h.x + h.y) * 6.2831853f;
                const float flicker = 0.5f + 0.5f * sin(params.StarsTime * 3.0f + phase);
                starColor *= lerp(1.0f, flicker, params.StarsTwinkle);
            }

            result += starColor * (falloff * magnitude);
        }
    }

    // 天頂輝度を基準にすることで、夜空の明るさが変わっても星との相対関係が保たれる
    return result * params.StarsIntensity * params.ZenithLuminance * horizonFade;
}

float3 SkyColor(float3 dir, SkyParameters params)
{
    if (dir.y >= kGroundFadeStartY)
    {
        // 星は雲より奥にあるので、雲で減光される前のここで足す。
        // StarsIntensity=0(昼・無効)のときEvaluateStarfieldは即座に0を返し、
        // 下の加算も分岐で飛ばすので、従来と画素まで一致する
        float3 clearColor = SkyColorUpper(dir, params);
        if (params.StarsIntensity > 0.0f)
        {
            clearColor += EvaluateStarfield(dir, params);
        }

        // (h) 早期脱出。積雲・巻雲どちらの被覆率も0、または地平線より下(dir.y<=0、(e)節)では
        // 雲の計算を一切行わずclearColorをそのまま返す。判断C(被覆率0のときは雲を
        // 持たない空と画素まで一致すること)の担保の1つめはここ——雲側の計算(EvaluateCloudLayer)は
        // 一度も呼ばれず、返す値もSkyColorUpperの結果そのままなので数値は変わりようがない
        if ((params.CloudCoverage <= 0.0f && params.CirrusCoverage <= 0.0f) || dir.y <= 0.0f)
        {
            return clearColor;
        }

        // 雲へ掛ける大気遠近。層に依らない値なのでここで1回だけ組み立て、両層へ渡す。
        // 【上の早期脱出より後に置く】判断Cの「雲が無いときは掛け算・足し算を1つも増やさない」に
        // 揃えるため。被覆率0の画素はここへ到達せず、この組み立て自体が行われない
        const CloudFogParams fog = MakeCloudFogParams(params);

        // 積雲(下層)。被覆率0でもここへ来る場合があるため(巻雲だけの空)、個別に早期脱出する。
        // transmittance=1.0/scatteredLight=0の初期値は「雲が無い」ことを表す中立元(下のclearColor*1+0と
        // 一致する値)であり、CloudCoverage<=0のときEvaluateCloudLayerを呼ばずこの初期値のまま使う
        float cumulusTransmittance = 1.0f;
        float3 cumulusScatter = float3(0.0f, 0.0f, 0.0f);
        if (params.CloudCoverage > 0.0f)
        {
            EvaluateCloudLayer(
                dir, MakeCumulusLayerParams(params), params.SunDirection, params.ZenithLuminance,
                params.SunToSkyIlluminanceRatio, params.SkyIlluminanceOverZenith, fog,
                cumulusTransmittance, cumulusScatter);
        }

        // 【判断Cの担保の2つめ】巻雲の被覆率が0のとき、EvaluateCloudLayer(巻雲側)を一度も呼ばず、
        // 積雲だけの式(clearColor * T_cumulus + S_cumulus)を
        // そのまま通す。掛け算・足し算を1つも増やさないことで、浮動小数の最下位ビットまで一致させる
        if (params.CirrusCoverage <= 0.0f)
        {
            return clearColor * cumulusTransmittance + cumulusScatter;
        }

        // 巻雲(上層)を評価する。巻雲は積雲より高い位置にあるため、巻雲から届く散乱光は
        // 手前(視点側)にある積雲でさらに減光される——これを表すのが下のcumulusTransmittanceを
        // 掛ける項。掛けないと積雲に隠れるはずの巻雲が透けて見えてしまう
        float cirrusTransmittance;
        float3 cirrusScatter;
        EvaluateCloudLayer(
            dir, MakeCirrusLayerParams(params), params.SunDirection, params.ZenithLuminance,
            params.SunToSkyIlluminanceRatio, params.SkyIlluminanceOverZenith, fog,
            cirrusTransmittance, cirrusScatter);

        // (g) 2層合成: 高い層(巻雲)から手前(積雲)へ。
        //   透過率 = T_cirrus * T_cumulus (両層を貫く視線の透過率なので積)
        //   散乱光 = S_cumulus + S_cirrus * T_cumulus (巻雲の光は積雲を透過して初めて届く)
        // lerpではなくこの形にするのは、雲の隙間からのぞく青空をそのまま残すため
        // (lerpだと被覆率で単純に混ぜてしまい、隙間の青空まで雲色へ寄ってしまう)。
        // 地平線より下(この関数の後続のelse分岐)には雲を一切掛けない
        const float transmittance = cirrusTransmittance * cumulusTransmittance;
        const float3 scatteredLight = cumulusScatter + cirrusScatter * cumulusTransmittance;
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
