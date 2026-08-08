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
    // xyz=雲による空の明かりの変化(P18)。「雲込みの空の照度 ÷ 晴天の空の照度」をRGBで持つ。
    // 被覆率0で厳密に(1,1,1)。w=予備。詳細はSkyParameters::CloudSkyLightのコメント
    float4 CloudSkyLight;
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

    // --- 雲による空の明かりの変化(P18)。「雲込みの空の照度 ÷ 晴天の空の照度」のRGB ---
    // 【何のためにあるか】大気遠近のin-scatter(airlight)を照らしているのは、視線の先の
    // 空ではなく**その空間を照らしている光**、すなわち空全体の照度である。ところが
    // AerialPerspective.hlslはその照度をSkyColorUpper(=雲を通さない晴天の空)から作っており、
    // 被覆率1.0で空が灰色一色でも遠方の地物には青い散乱光が掛かり続けていた
    // (実測: 被覆率1.0での水平線際の空はB-R 42・輝度81だが、掛かっていたのは被覆率0の
    // B-R 86・輝度110。青みが2.0倍・明るさが1.35倍ずれていた)。
    //
    // 【なぜZenithLuminanceを暗くする形にしないか】天頂輝度は(a)雲の隙間から見える青空と
    // (b)雲そのものの明るさ(sunIlluminance = 比 × SkyIlluminanceOverZenith × 天頂輝度)の
    // 両方の基準になっている。ここを暗くすると隙間が二重に暗くなり、雲自体まで暗くなる
    // (KurenaiEngine3D.cppのm_ActiveCloudTransmittanceの代入元にある同じ指摘を参照)。
    // だから減光は天頂輝度に混ぜず、この独立した係数として持つ。
    //
    // 【スカラーではなくRGBである理由】曇天のairlightは暗いだけでなく**無彩色になる**。
    // スカラー倍では暗い青のままで、上の実測でいうと明るさの1.35倍しか埋まらない。
    //
    // 【方向依存が要らない理由】airlightを照らすのは半球全体なので、正しい量は半球平均で
    // ある。方向依存が効くのは太陽の直達光(薄明光線)だけで、このモデルはそれを持たない。
    //
    // 値はSkyIntegrate.hlslが求めてGPUSkyParameters::CloudSkyLightへ書く。
    // 被覆率0では厳密に(1,1,1)になり、掛けても浮動小数の最下位ビットまで変わらない
    float3 CloudSkyLight;

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
    float  CloudAltitude;      // 雲底の高度[m](**ワールドYの絶対高度**)。
                                // 【P17で意味が変わった】以前は「カメラのワールドY基準」の
                                // 相対高度で、層がカメラのYに追従していた(上空へ飛んでも雲の
                                // 上に出られず、模様もカメラに追従した)。P17でViewerPositionを
                                // 起点とするレイと層のスラブ交差を解く形になり、絶対高度になった
    float  CloudUvScale;       // ワールド1mあたりのノイズ空間の距離
    float  CloudDensity;       // 消散係数。大きいほど不透明で影が濃い
    float2 CloudScrollOffset;  // 風によるノイズ空間の移動量(CPU側でkCloudNoisePeriodの周期に
                                // wrap済み。KurenaiEngine3D.cppのm_CloudScrollOffset参照)
    float  CloudForwardG;      // Henyey-Greensteinの非対称パラメータ(前方散乱の強さ)
    float  CloudTypeBias;      // 雲の種類の偏り(C4)。0=層雲寄り / 0.5=中立 / 1=雄大積雲寄り。
    float  CloudThickness;     // 雲底から雲頂までの厚み[m](P13b)。0ならレイマーチせず
                                // 従来の厚みゼロの平面として扱う(巻雲はこちら)

    // --- 巻雲。積雲より高層にある2層目。CirrusCoverage <= 0 なら巻雲側の計算は
    // 一切行わない(判断C、SkyColor参照)。判断A(IBLキューブに雲を焼かない)・判断B
    // (平均透過率だけをベイク時に掛ける)は積雲とまったく同じ理由でこちらにも適用する
    // (Sky.hlsli冒頭の雲セクション、KurenaiEngine3D.cppのComputeCloudAverageTransmittance参照)。
    // 前方散乱の強さと自己影ステップ数はシェーダ内定数(kCirrusForwardG/kCirrusShadowSteps)
    // のため、ここにはフィールドを持たない(cbufferを増やす価値がないため) ---
    float  CirrusCoverage;     // 0=巻雲なし、1=全天が巻雲
    float  CirrusAltitude;     // 雲底の高度[m](**ワールドYの絶対高度**。積雲と同じ規約。P17参照)
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

    // --- レイマーチの開始位置をずらす量(C2)。[0,1) ---
    // 【なぜ要るか】全画素が同じ位置からマーチを始めると、ステップの切れ目が画面全体で
    // 揃った帯(スライス)として見える。画素ごとに1歩ぶん未満だけずらすと、その帯が
    // 高周波のディザへ変わり、はるかに目立たなくなる。
    // 【呼び出し側が入れる】各シェーダーがSV_Positionから CloudRaymarchDither() で作る。
    // ここで作れないのは、この構造体が画面座標を知らないため(SkyGenerate.hlslのように
    // 画面を持たない呼び出し側もある)。雲を評価しない経路では0でよい
    float  RaymarchJitter;

    // --- 視点のワールド座標(P17) ---
    // 【P17でfloatからfloat3へ広げた】以前はFogViewerHeightという名前でワールドYだけを
    // 持ち、用途も「雲底までの霞を評価するために絶対高度へ戻す」ことだけだった。雲層が
    // カメラ相対だったのでXZは要らなかったからである。P17で雲をワールド座標に固定し、
    // レイと層の交差を解くようになったため、レイの起点としてXZも要る。
    // SkyColor(dir, params)はこの位置を起点としたレイとして解釈する
    float3 ViewerPosition;

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
// 【P17でviewerPositionはfloat3になり、意味が「おおよその高さ」から「レイの起点」へ変わった】
// P17より前、この値は消散係数を高度で評価するためだけに使われており、数メートルのずれは
// 結果を0.1%も動かさなかった。P17で雲をワールド座標に固定してからは、これが雲との交差を
// 解くレイの起点そのものになる——**ずれるとその分だけ雲の模様がずれる**。
// したがって呼び出し側は「そのレイが実際に出る場所」を渡すこと:
//   - DeferredLighting.hlsl … カメラ位置(背景画素の視線はカメラから出る)
//   - SSR.hlsl             … カメラ位置。ただし水面の反射だけは起点が水面なので、
//                             SkyColorWithRayへ水面のワールド座標を明示的に渡して上書きする
//   - PlanarReflection.hlsl … CameraPositionは鏡映後のカメラ位置(yが負になる)。このシェーダーは
//                             SkyColorUpperしか呼ばずEvaluateCloudLayerへ到達しないため影響は
//                             無いが、SkyColorを呼ぶよう変えるなら鏡映前の位置を渡し直すこと
//   - SkyGenerate.hlsl     … 原点。判断A(IBLキューブに雲を焼かない)により被覆率0で呼ばれ、
//                             雲の計算自体が走らないため値は使われない
// レイマーチの開始位置をずらす量を画面座標から作る(C2)。Jimenezの
// interleaved gradient noise。**乱数ではなく低不一致列**なので、隣り合う画素が
// 別々の位相を取りつつ、4x4の範囲を見るとほぼ均等にばらける。
// 【なぜハッシュではなくこれか】素のハッシュだと白色雑音になり、ざらつきがそのまま
// 見えてしまう。IGNは細かい織り目状のディザになり、同じ分散でも目に付きにくい
// (SSAOがHash12+ブラーで均しているのと同じ狙いを、ブラー無しで達成する)
float CloudRaymarchDither(float2 pixelPosition)
{
    return frac(52.9829189f * frac(dot(pixelPosition, float2(0.06711056f, 0.00583715f))));
}

SkyParameters ApplyCloudFogParameters(SkyParameters params, float4 fogParams0, float3 viewerPosition)
{
    params.FogEnabled = fogParams0.w;
    params.FogSigma0 = fogParams0.x;
    params.FogScaleHeight = fogParams0.y;
    params.FogRefHeight = fogParams0.z;
    params.ViewerPosition = viewerPosition;

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
    // 雲による空の明かりの変化(P18)。大気遠近のin-scatterへ掛ける
    // (SkyParameters::CloudSkyLightのコメント参照)。被覆率0では(1,1,1)
    params.CloudSkyLight = data.CloudSkyLight.xyz;
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

// 【F2で未使用になった】ウェザーマップのfBmは単調減衰のオクターブ列から
// 帯域制限した重み表(kCloudFbmWeight)へ変わったため、この定数はどこからも読まれない。
// 帯の数と重みは CloudFbm のすぐ上を参照すること

// 光路長のクランプに使う下限(方向のyがこれを下回ったらこの値で頭打ちにする)。
//
// 【P17で用途が2つから1つへ減った】P17より前、この定数は(1)視線の光路長、(2)視線と雲底
// 平面の交点の位置、(3)太陽方向の光路長、の3つに使われていた。このうち(2)は「雲層がカメラに
// 固定されている」という誤ったモデルを地平線際で破綻させないための当て物で、P17でレイと層の
// スラブ交差を世界座標で解くようになったため不要になった(交差は|dir.y|→0でも発散しない。
// 層に当たらないか、当たるなら実際の経路長ぶんだけ濃くなる、というどちらかにしかならない)。
//
// 残る用途は「**厚みゼロの平面**として扱う層(巻雲)の光路長」と「太陽方向の光路長」の2つ。
// 厚みゼロのシートを斜めに貫く経路長は原理的に1/|dir.y|で発散するため、これはモデルに
// 内在する発散であってカメラ固定の副作用ではない。0.05は「約2.9度以上の仰角では実質
// クランプがかからず、それより下では経路長が最大20倍で頭打ちになる」という見た目からの調整値
static const float kCloudMinDirY = 0.05f;

// レイが層と平行とみなす|dir.y|のしきい値(P17)。これを下回ったらスラブ交差の
// t = (境界 - 起点.y)/dir.y が桁あふれするため、「層の中にいるなら最後まで、外なら当たらない」
// という別扱いに切り替える。1e-4は「1,000m進んで0.1mしか高度が変わらない」に相当し、
// スラブの厚み(既定1,000m)に対して十分に平行と言える
static const float kCloudParallelDirY = 1.0e-4f;

// マーチする距離の上限[m](P17)。地平線際で|dir.y|が小さいほどスラブ内の経路は伸び、
// 固定ステップ数のままでは1ステップがノイズ1セル(約2km)を超えてエイリアシングになる。
//
// 【なぜ30kmか】既定の視程(消散係数0.0002 → 約20km)では、雲底1,500mを見込む視線が
// この距離に達するのは仰角約2度で、そこでの霞の透過率は既に0.006——下の
// kCloudFogCutoffTransmittanceに掛かって打ち切られる。つまり**霞が有効な限りこの上限は
// 一度も効かない**。霞を切ったときだけ効く安全弁である
static const float kCloudMaxSpanMeters = 30000.0f;

// 霞による打ち切りのしきい値(P17)。視点からそのサンプルまでの霞の透過率がこれを
// 下回ったら、以降のサンプルは絵に出ないものとしてマーチを止める。
//
// 【これがP17より前の地平線フェードの置き換え】以前は kCloudHorizonFadeStartY=0.2
// (仰角約11.5度)より下の雲を見た目として消していたが、これは1/dir.yの発散を隠すための
// 対症療法で、**水面の反射を殺している当のもの**だった(水面すれすれの反射レイはこの領域へ
// 丸ごと入る)。正しい交差を解けば発散しないので、代わりに「遠くて霞に埋もれた雲は見えない」
// という物理そのもので打ち切る。0.02は「元の輝度の2%未満は8bitの1階調にも満たない」から
static const float kCloudFogCutoffTransmittance = 0.02f;

// 背景(地物に遮られない視線)へ渡すレイ長[m](P17)。無限の代わりに使う有限値。
// 大気の厚みに対して十分大きく、floatの精度に対して十分小さい値
static const float kCloudBackgroundRayDistance = 1.0e7f;

// 自己影のステップ数。**C1でXZ平面上の2D積分から3Dのレイマーチへ変わった**。
//
// 【何が問題だったか】C1より前、自己影は次の2つの近似の合成だった:
//   sunTransmittance … 太陽方向をXZ平面へ投影し、2Dのウェザーマップ(CloudFbm)を固定距離
//                      1,500m・5歩たどる。雲の縦構造も太陽の仰角も反映せず、3Dノイズを1回も引かない
//   aboveTransmittance … exp(-(1-hf)*k) の解析的な縦勾配。位置に依らない単調勾配で形を持たない
// 実測でこの2つが雲の明るさへ与える寄与は +1.2 と +3.6(雲の90%点と空の中央値の差54に対して)
// しかなく、**ほとんど形を作っていなかった**。参考写真の積雲が立体に見えるのは
// 「明るい雲頂・灰青の平らな雲底・房どうしの間の落ち影」によるもので、そのすべてが
// 3次元の自己影から出る。C1で両方を捨て、サンプル位置から太陽方向へ実際にマーチする形にした。
//
// ステップ数はシェーダ内定数(コストの主要なつまみ。kCumulusRaymarchStepsと同じ扱い)
static const int kCloudSunSteps = 6;
// 太陽マーチの1歩目の長さを、スラブの厚みに対する比で決める。厚みが変わっても
// 相対的な細かさが保たれる(kCloudSunStepGrowthで指数的に伸びるので、総距離は厚みの約1.6倍)
static const float kCloudSunFirstStepRatio = 1.0f / 12.0f;
// ステップ長を1歩ごとに何倍に伸ばすか。手前を細かく、遠くを粗く見るための等比
static const float kCloudSunStepGrowth = 1.6f;
// コーンオフセットの半径(進んだ距離に対する比)。**Schneiderのコーンサンプリング**で、
// サンプルを太陽方向のまわりへ広げ、近傍の雲も拾って影が硬くなりすぎないようにする。
//
// 【この値は見た目にほとんど効かない(実測)】0.35 / 0.20 / 0.10 / 0.00 と振って
// 雲画素のコントラスト比(トーンマップ前のシーン輝度の75%対25%)を測ったところ
// 1.30 / 1.30 / 1.31 / 1.31 と動かなかった。雲が暗い原因を探して真っ先に疑った項だが、
// **犯人ではなかった**(実際は多重散乱の段数不足。kCloudMsOctavesのコメント参照)。
// 0.35のまま残してあるのは、コーンの本来の役割(房の間に硬い筋が出るのを防ぐ)自体は
// 妥当な近似であり、0にする積極的な理由が無いため
static const float kCloudSunConeRadius = 0.35f;
// コーンの方向。半球上にばらけた6方向を固定で持つ(乱数を使うとフレーム間でちらつくため)。
// 長さは1に揃えず、後ろの方ほど大きく広がるようにしてある
static const float3 kCloudSunConeOffsets[6] =
{
    float3( 0.38f,  0.15f,  0.90f),
    float3(-0.72f,  0.30f,  0.62f),
    float3( 0.55f, -0.45f, -0.70f),
    float3(-0.30f, -0.62f,  0.72f),
    float3( 0.85f,  0.50f, -0.15f),
    float3( 0.00f,  0.00f,  0.00f)   // 最後の1歩はオフセットせず、遠方の平均的な減衰を素直に拾う
};

// 巻雲(P11)の前方散乱パラメータ。UI(CPU側)ではなくシェーダ内定数にしてある理由は
// SkyParameters::CirrusAnisotropyのコメント・KurenaiEngine3D.h側のコメント参照
// (cbufferを増やす価値がないため)。
// 【巻雲に自己影を持たせない理由】巻雲は光学的に薄く(CirrusDensityは積雲の1桁下)、
// 自己影がほとんど見た目に効かない。巻雲は厚みゼロの平面経路を通り、そちらは
// C1でも一切変えていないため sunTransmittance = 1.0 のまま(判断3)
static const float kCirrusForwardG = 0.3f;

// 【C1で kCloudSunExtinctionScale を撤去した】太陽側の光学的深さを視線側と違う単位で
// 積んでいたのを揃えるための当て物だった。C1では太陽側も視線側とまったく同じ
//   density * Density * (距離 / Thickness)
// という無次元量で積むため、単位を合わせる係数そのものが要らなくなった。

// 【P17で kCloudHorizonFadeStartY / kCloudHorizonFadeEndY を撤去した】
// 仰角11.5度以下の雲をフェードで消していた2定数は、1/dir.yの発散を隠すための対症療法であり、
// 同時に水面への雲の映り込みを殺していた(水面すれすれの反射レイは丸ごとこの領域に入る)。
// レイと層の交差を正しく解けば発散しないので、代わりに上の kCloudFogCutoffTransmittance
// (遠くて霞に埋もれた雲は見えない)で打ち切る。同じ物理で経路長とエイリアシングの
// 両方が同時に有界になる

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
// 【雲の明るさの基準を太陽照度へ変更】以前はzenithLuminance(青空の天頂輝度)に
// 0.25〜0.83の係数を掛けていたため、雲は原理的に青空より暗くしかならず、日向の積雲でも
// 曇り空のような灰色にしかならなかった(実測: 雲のない空の輝度125に対し雲がある所が130、
// 比1.04倍)。実際の日向の積雲は青空の3〜5倍明るい。これは雲を照らしているのが空ではなく
// 太陽だからなので、EvaluateCloudLayerを太陽照度基準に組み直した。
//
// 【C1で積雲側の SingleScatterScale / AmbientTermMin / AmbientTermMax を撤去した】
// これらは「多重散乱を自己影の透過率で下限〜上限に補間する」という、向きも深さも持たない
// スカラー近似だった。C1で下の多重散乱オクターブ和へ置き換えたため役目を終えた。
// **巻雲(平面経路)は引き続きこの3つを使う**ので、定数もフィールドも残してある(判断3)
static const float kCumulusAlbedo = 1.0f;

// 巻雲(P11)側の値。巻雲は自己影を持たない(sunTransmittanceが常に1.0)ため
// AmbientTermMin側は事実上使われない(lerp(Min,Max,1.0)=Max)が、式を1箇所に保つため
// フィールド自体はCloudLayerParamsに残し、Min=Maxとして無効化しておく。
// 単散乱強度(SingleScatterScale)を積雲よりやや強めにしてあるのは、巻雲は氷晶による
// 前方散乱が卓越し薄い縁が霞むように光る見た目を意図した調整値であり、実測値ではない
static const float kCirrusAlbedo = 1.0f;
static const float kCirrusSingleScatterScale = 0.5f;
static const float kCirrusAmbientTermMin = 0.4f;
static const float kCirrusAmbientTermMax = 0.4f;

// ============================================================================
// 多重散乱のオクターブ近似(C1)。Wrenninge (2013) / Hillaire (2016) の
// "energy-conserving multiple scattering approximation"
// ============================================================================
//
// 【なぜこの形か】雲が白く明るいのは、光が内部で何十回も散乱して出てくるためである。
// 単散乱だけを解くと厚い芯が真っ黒になり、逆に定数の環境項で埋めると平坦になる。
// オクターブ和は「k回目の散乱は、消散がaᵏ倍に弱まり、寄与がbᵏ倍に減り、位相がcᵏ倍だけ
// 等方に近づいた単散乱」とみなす近似で、次の3つを同時に満たす:
//   - 厚い芯ほど高次の項が支配的になり、明るく・彩度が低く・向きが鈍くなる
//   - 薄い縁は0次(単散乱)が支配的で、太陽方向へ鋭く光る(silver lining)
//   - **追加のテクスチャフェッチが0**。同じ sunOpticalDepth を使い回すだけ
//
// a と c は0.5。原論文でもこの付近が使われており、和が等比級数として収束するぶん
// エネルギーが発散しない。
//
// 【b を0.5から0.85へ、オクターブ数を3から5へ上げた根拠】どちらも参考写真と突き合わせて決めた。
//
// まず b。写真は 10%点208.2 / 中央227.5 / 90%点250.1 / 四分位21.8(画面のsRGB、
// 島を避けた左右の空の列、実現値3枚の中央値):
//   b=0.50 … 174.5 / 199.2 / 207.2 / 14.5
//   b=0.70 … 195.7 / 211.4 / 218.4 / 10.3
//   b=0.85 … 204.3 / 217.0 / 224.9 / 10.5   ← 採用。輝度の3指標で写真に最も近い
//
// 次にオクターブ数。**3段では厚い雲の内部で高次の項が足りず、下から見上げた雲底が
// 暗くなりすぎていた**。太陽照度の連鎖を実機から読み出すと
//   SunToSkyIlluminanceRatio 5.99 × SkyIlluminanceOverZenith 5.37 × 天頂輝度 0.095 = 3.06
// で、完全に照らされた雲の理論値は2.25になるはずなのに、実測の雲画素は95%点で0.85しか
// 出ていなかった。つまり光が雲を貫けていなかった。段数を振った実測(**トーンマップ前の
// シーン輝度**へ戻して測る。写真は 5%0.267 / 中央0.869 / 95%4.701 / 75%対25%比2.17):
//   3段 … 5%0.42 / 中央0.66 / 95%0.86 / 比1.30   灰色に沈む
//   5段 … 5%0.56 / 中央0.93 / 95%1.33 / 比1.49   ← 採用。中央値が写真に最も近い
//   8段 … 5%0.60 / 中央1.21 / 95%1.89 / 比1.76   写真を追い越して白く飛ぶ
//
// 【95%点(日向の雲頂)は5段でも写真に届かない】写真は撮影後に彩度・コントラストを
// 上げてあることが空の測定でも分かっている(このシーンのSkySaturationのコメント参照)。
// ここは写真の数字ではなく物理側の値を採る
static const int kCloudMsOctaves = 5;
static const float kCloudMsExtinctionFalloff = 0.5f;  // a: k段目の消散係数へ掛かる
static const float kCloudMsContribution = 0.85f;      // b: k段目の寄与
static const float kCloudMsEccentricityFalloff = 0.5f; // c: k段目の位相の異方性

// 2ローブのHenyey-Greenstein位相関数。前方(g1)と後方(g2)を混ぜる。
// 【なぜ単一ローブでは足りないか】積雲は前方散乱が卓越するが、実際には太陽を背にした側にも
// 弱い後方散乱のピークがある(だから順光でも雲は白く明るい)。前方だけだと順光の雲が
// 平坦な灰色になる。g2を負にすることで後方へ弱いローブを足す。
// 前方側のgは層ごとの CloudForwardG(UIつまみ)をそのまま使い、ここでは後方側と混合比だけ持つ
static const float kCloudPhaseBackwardG = -0.15f;
static const float kCloudPhaseBackwardWeight = 0.3f;

// 粉末効果(powder)の強さ。Schneiderの近似で、雲の縁が「密度が低いのに暗く見える」現象を出す。
// 【なぜ暗くなるのか】薄い縁では光が入ってすぐ抜けるため散乱回数が少なく、逆に内部では
// 多重散乱で光が溜まる。単散乱の式だけを見ると縁のほうが明るくなってしまうので、
// 1 - exp(-2τ) を掛けて縁を落とす。**太陽が視線の向こう側にあるとき(逆光)は縁が実際に
// 明るく光る**ので、順光側でだけ効かせる(下の cosAngle による補間)
static const float kCloudPowderScale = 2.0f;

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

// ウェザーマップ(雲の置き場を決める2次元の場)。**帯域制限**した多重ノイズ(F2)。
//
// 【なぜ単調減衰のfBmをやめたか】実際の積雲は対流セルとして組織化しており、
// セルの水平間隔は境界層の厚みの2〜5倍という**特徴スケール**を持つ。
// ところが単調減衰のfBmは自己相似で、**特徴スケールを持たない**。その結果、
// 雲の大きさに「これくらい」が生まれず、極小の雲が空一面に均一に散らばる絵になっていた。
//
// 【実測】40km四方を真上から見た雲の連結成分で「モード/中央値」(特徴スケールの有無)を測ると
//   単調減衰(旧)                    0.64  ← 特徴スケール無しの対照(0.64)と同じ
//   帯域制限(この式)                1.11  ← 特徴スケール有りの対照(1.12)に到達
// 雲の大きさのべき指数は 1.88 → 2.15 で、実際の積雲の1.7〜2.2に収まったまま。
//
// 【低周波を足すだけでは駄目】fBmは何オクターブ足しても自己相似のままで、モードは生まれない。
// **エネルギーを1つのスケールへ集める**必要がある。重みを山型にするだけなのでコストは同じ。
//
// 【周波数の制約】巻き戻しの周期は kCloudNoisePeriod * frequency 個の格子になるので、
// **これが整数でなければならない**(整数でないとタイル境界で格子が揃わず筋が出る)。
// 256 * {0.25, 0.5, 1, 2, 4} = {64, 128, 256, 512, 1024} で条件を満たす。
// 重みのピークは frequency 0.5 = 波長2セル。CellSize 1,400m のとき **2,800m**で、
// スラブ厚1,200mの2.33倍。物理が要求する2〜5倍に収まる。
//
// 【この重みを変えたら測り直すもの】kCloudWeatherLow/High/Max(場の1%/99%/99.9%点)と
// kCloudVolumeDensityNormalize。どちらも下にこの重みで測り直した値が入っている。
// 【大小2つのpopulationを同時に出す】(F2b) 実際の積雲の場は大きさがべき分布で、
// **対流セルとして組織化した大きな雲と、その隙間を埋める小さな雲が同時に存在する**。
// 重みを2組持ち、同じ帯の評価を使い回して2つの場を同時に作る(ノイズの評価は増えない)。
//   Large … ピーク frequency 0.5 = 2,800m。対流セルの組織化
//   Small … ピーク frequency 1.0 = 1,400m。セルの隙間を埋める小さなサーマル
// 実測(40km四方を真上から): 大のみでべき指数2.16、大+小で**1.99**。
// 実際の積雲の1.7〜2.2の真ん中へ寄り、雲の数も387→580へ増える。
//
// 【G→H1で峰を 0.72 → 0.45 → 0.30 へ二段階で緩めた】被覆率を上げると**格子模様が見えていた**ため。
// CloudValueNoise は正方格子の各点にハッシュを置いて補間しているので、単一の周波数だけを
// 見るとその格子の目がそのまま模様になる。普段は複数のオクターブが重なって隠れるが、
// F2で重みの0.72を frequency 0.5 の1本へ集めたことで露出した。
//
// 【測り方】40km四方の場を2次元FFTし、振幅を「軸に沿った十字帯(±15度)」と
// 「斜めの帯(30〜60度)」で比べた比を異方性とする。等方な場なら1.0になる:
//   峰0.72(F2)              異方性 1.72   モード/中央 1.01
//   峰0.45(G)               異方性 1.28   モード/中央 1.02  ← 格子は薄れたがまだ見えた
//   帯を3本に割る           異方性 1.37   モード/中央 0.29  ← モードを失う。不採用
//   単調減衰(F2前)          異方性 1.07   モード/中央 0.32
// **モードを測らずに峰だけ緩めてはいけない。** F2の成果はモードなので、そこは守る。
//
// 【H1で0.45から0.30へ】0.45でもユーザーには格子が見えていたため、峰の幅を刻み直した。
// 窓を3つ取って平均した(1つの実現値で決めない):
//   峰      異方性  モード/中央  べき指数
//   0.45     1.25      0.87      1.91
//   0.40     1.21      0.91      1.95
//   0.35     1.17      1.01      1.99
//   0.30     1.13      1.17      2.20  ← 採用。3条件をすべて満たす唯一の点
//   0.25     1.08      1.36      2.34  ← べき指数が実際の積雲の範囲(1.7〜2.2)を外れる
//
// 【却下した2案】どちらも**絵で落ちた**。数値だけで決めてはいけない:
//   ・ドメインワープ(UVを別のノイズで歪める)…異方性は1.19まで下がるが、
//     絵が大理石模様(渦と筋)になる。振幅を上げるほど悪化する
//   ・オクターブの回転(ピタゴラス数(3,4,5)でUVを回す)…異方性1.26でほとんど効かず、
//     しかも巻き戻しの周期を 256f/5 にする必要があるため
//     **世界の繰り返しが358kmから71.7kmへ縮む**(水平線際で反復が見える恐れ)
static const int kCloudFbmBands = 5;
static const float kCloudFbmFrequency[5] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
static const float kCloudFbmWeightLarge[5] = { 0.20f, 0.30f, 0.24f, 0.15f, 0.11f };
static const float kCloudFbmWeightSmall[5] = { 0.00f, 0.05f, 0.45f, 0.32f, 0.18f };

// 【H0: 帯ごとにUVの位相をずらす】これが無いと**ワールド原点に雲の穴が開く**。
//
// 【何が起きていたか】CloudHash12 は p=(0,0) で p3=(0,0,0) となり、
// frac((0+0)*0) = **0** を返す(退化点)。CloudValueNoise は格子の4隅をハッシュするので、
// 原点のセルに触れる場所は必ずこの0を混ぜることになる。さらに悪いことに、位相をずらさないと
// **5つの帯すべてが uv≒0 で同じ格子点(0,0)を参照する**ので、帯どうしが打ち消し合わずに
// 全部そろって0へ引っ張られる。fBmの「重ねれば平均へ寄る」という前提が原点付近だけ壊れる。
//
// 【実測(オフライン。カメラ位置 0,1.6,-600 の真上が見る ±0.7セルの窓)】
//   位相ずらし無し  largeN の平均 0.237 / P(largeN<0.3) 71.9%
//   位相ずらし有り  largeN の平均 0.529 / P(largeN<0.3)  0.0%
//   世界全体では    平均 0.498 で**どちらも同じ**(4.4% 対 4.2%)
// 実機でも weather = step(0.30, largeN) にして測ると真上の空の64.7%が
// largeN<0.30 だった(世界全体の予測は4.5%)。**局所を世界全体の統計で語っていた。**
//
// この穴は島とカメラのある場所そのものに開いていたので、
// 「真上の雲が少ない」「被覆率を1にしても埋まらない」の両方を単独で説明する。
//
// 【巻き戻しは壊れない】オフセットは定数なので、ワールドが kCloudNoisePeriod ずれたときの
// UVのずれは 256*frequency の整数倍のままで、周期の境界は揃ったままになる。
// 【値そのものに意味は無い】帯どうしが原点付近で別の格子点を見ればよいだけ。
// **分布は変わらないので、しきい値の定数も正規化係数も測り直す必要はない**
// (世界全体で 1%点 0.243 / 99%点 0.757 / 上端 0.880。現行の 0.241 / 0.758 / 0.881 と一致)
static const float2 kCloudFbmBandOffset[5] = {
    float2(23.7f, 91.3f), float2(57.1f, 13.9f), float2(11.3f, 77.7f),
    float2(89.1f, 41.3f), float2(5.9f, 63.1f)
};

// ============================================================================
// ウェザーマップのテクスチャ(H3)
// ============================================================================
//
// 【なぜ焼くのか】レイマーチの1歩あたりコストの**91%がこの2Dのfbm**だった(実機の実測。
// 同じ条件で6枚ずつ撮った中央値):
//   1歩 3.68ms / 48歩 5.64ms / 192歩 10.58ms  → 1歩あたり 0.0343ms
//   CloudFbmBands と CloudTypeAt を定数へ置き換えると  → 1歩あたり 0.0032ms
// フレーム全体5.64msのうちマーチは1.65ms(29%)で、残り3.99msは雲以外。
//
// 【空を飛ばす2段マーチは不採用】計画では「48歩のうち雲に当たるのは0.35歩、97.9%は空気」を
// 根拠に空間スキップを予定していたが、実測すると**同じ評価回数なら現状より悪かった**
// (仰角6度・基準18.8%に対し、2段マーチ51.0回→15.9% 対 現状47.5回→17.2%)。
// 空歩は無駄ではなく探索そのもので、さらに粗い均一マーチは薄い雲を過大に積分するため
// (40mの雲を1回引いて239m分の密度にする)、その過大評価が見落としを打ち消していた。
// 細かく積分すると打ち消しだけが消えて悪化する。**歩数を減らすのではなく1歩を安くする**のが正しい。
//
// 【中身】R=大きい雲のfbm / G=小さい雲のfbm / B=雲の種類の生ノイズ。どれも[0,1]。
// 被覆率によるしきい値化は引いたあとで行う(被覆率は実行時に変わるため焼き込めない)。
//
// 【解像度の根拠(オフラインで実測)】一番細かい帯は frequency 4.0 で格子の間隔が0.25セル=350m。
// 周期256セル=358kmを1枚で覆うので、weatherの誤差(平均/最大)は:
//   1024^2 (350m/テクセル)  0.0095 / 0.90   ← 使えない
//   2048^2 (175m/テクセル)  0.0028 / 0.32
//   4096^2 ( 88m/テクセル)  0.0009 / 0.095  ← 採用
// **ビット数は効かない。** 同じ解像度なら8bitと16bitの誤差はほぼ同じで(4096^2で
// 0.0011 対 0.0009)、誤差は空間の刻みだけで決まる。被覆率1でしきい値の幅が
// kCloudWeatherNarrowSpan=0.05まで狭まるので量子化が増幅されると考えたが、外れだった。
//
// 【手続き版を消してはいけない】ベイク(CloudNoiseGenerate.hlsl の CSGenerateWeather)が
// 下の *Procedural をそのまま呼んで焼く。**焼く側と引く側で式が分かれると必ずずれる。**
// またテクスチャを束ねていないシェーダー(SkyGenerate / AerialPerspective /
// PlanarReflection が通る巻雲の平面経路)は手続き版のまま走る
#ifdef KURENAI_CLOUD_WEATHER_REGISTER
// Samplers.hlsli をここでも取り込む。上の SkyView の #ifdef の中にしか include が無く、
// そちらを定義しない利用者では VolumeSampler が未宣言になるため(インクルードガード持ち)
#include "Samplers.hlsli"
Texture2D CloudWeatherNoiseTexture : register(KURENAI_CLOUD_WEATHER_REGISTER);
#define KURENAI_CLOUD_WEATHER_TEXTURE 1
#endif

// 2つの重みで同時に評価する。合成は**しきい値化のあと**で行う(CloudWeatherAt参照)
void CloudFbmBandsProcedural(float2 uv, out float large, out float small)
{
    float sumLarge = 0.0f;
    float sumSmall = 0.0f;
    float weightLarge = 0.0f;
    float weightSmall = 0.0f;
    [unroll]
    for (int band = 0; band < kCloudFbmBands; ++band)
    {
        const float frequency = kCloudFbmFrequency[band];
        // 周期も周波数と同じ倍率で伸ばす。格子が表すワールド範囲の周期性が帯ごとに
        // ずれると、継ぎ目の位置が揃わず周期性そのものが壊れるため
        // 【帯ごとの位相ずらし】kCloudFbmBandOffset のコメント参照。
        // これが無いとワールド原点で全帯が同じ格子点(0,0)を引き、ハッシュの退化で穴が開く
        const float n = CloudValueNoise(uv * frequency + kCloudFbmBandOffset[band],
                                        kCloudNoisePeriod * frequency);
        sumLarge += kCloudFbmWeightLarge[band] * n;
        sumSmall += kCloudFbmWeightSmall[band] * n;
        weightLarge += kCloudFbmWeightLarge[band];
        weightSmall += kCloudFbmWeightSmall[band];
    }
    // 各CloudValueNoiseは[0,1]を返すので、重みの和で割れば[0,1]に収まる
    large = sumLarge / weightLarge;
    small = sumSmall / weightSmall;
}

// 引く側の入口。テクスチャがあればフェッチ1回、無ければ手続きで評価する。
//
// 【uvをfrac()してはいけない】テクスチャはWrapで引く。ここでfrac()を掛けると
// 周期の境界でバイリニアのタップが端のテクセルに張り付き、一定間隔で継ぎ目が出る
// (VolumeSamplerのコメントと同じ理由。シェーダー側では消せない種類の継ぎ目)。
// 【巻き戻しは壊れない】CPUは風のスクロール量を kCloudNoisePeriod で fmod する。
// UVは uv / kCloudNoisePeriod なので、巻き戻しはUVがちょうど1周ぶんずれることに対応し、
// Wrapで引く限り同じテクセルへ戻る
void CloudFbmBands(float2 uv, out float large, out float small)
{
#ifdef KURENAI_CLOUD_WEATHER_TEXTURE
    const float2 baked = CloudWeatherNoiseTexture.SampleLevel(
        VolumeSampler, uv / kCloudNoisePeriod, 0.0f).rg;
    large = baked.r;
    small = baked.g;
#else
    CloudFbmBandsProcedural(uv, large, small);
#endif
}

// 1つだけ欲しい呼び出し側(巻雲の平面経路)のための包み。大きい側を返す
float CloudFbm(float2 uv)
{
    float large;
    float small;
    CloudFbmBands(uv, large, small);
    return large;
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
    float  Altitude;         // 雲底の高度[m](**ワールドYの絶対高度**。P17より前はカメラ相対だった)
    float  UvScale;          // ワールド1mあたりのノイズ空間の距離
    float  Density;          // 消散係数
    float2 ScrollOffset;
    float  ForwardG;
    float2 AnisotropicScale; // fBmのUVを異方的に伸ばす倍率(積雲は(1,1)、巻雲は筋状にする)
    // 【C1で ShadowSteps を外した】XZ平面上の2D積分で自己影を求めるステップ数だった。
    // ボリューム経路は3Dの太陽マーチ(kCloudSunSteps)へ移り、平面経路(巻雲)は
    // もともと0=自己影なしだったため、どの層も使わなくなった
    // 雲底から雲頂までの厚み[m](P13b)。**0なら従来の厚みゼロの平面として扱う**。
    // 巻雲は0を入れるため、巻雲が通るコードパスはP13b前と1命令も変わらない
    float  Thickness;
    // 雲の種類の偏り(C4)。ボリューム経路の縦プロファイルの選択に使う(平面経路は読まない)
    float  TypeBias;
    float  Albedo;           // 見かけのアルベド。両経路が使う
    // 単散乱強度・多重散乱の下限/上限。**C1以降は平面経路(巻雲)専用**。
    // ボリューム経路(積雲)は多重散乱のオクターブ和へ移ったため読まない
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
    layer.Thickness = params.CloudThickness; // 0でなければスラブをレイマーチする(P13b)
    layer.TypeBias = params.CloudTypeBias;
    layer.Albedo = kCumulusAlbedo;
    // 【C1以降ボリューム経路は読まない】平面経路専用のフィールド。積雲は多重散乱の
    // オクターブ和(CloudInScatterVolumetric)へ移ったため、ここは0で埋めておく。
    // 値を残すと「まだ効いている」と誤読されるため、あえて中立値にする
    layer.SingleScatterScale = 0.0f;
    layer.AmbientTermMin = 0.0f;
    layer.AmbientTermMax = 0.0f;
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
    // 平面経路は縦プロファイルを持たないため読まれない。中立値を入れておく
    layer.TypeBias = 0.5f;
    layer.AnisotropicScale = float2(params.CirrusAnisotropy, 1.0f); // U方向だけ伸ばして筋状にする
    layer.Albedo = kCirrusAlbedo;
    layer.SingleScatterScale = kCirrusSingleScatterScale;
    layer.AmbientTermMin = kCirrusAmbientTermMin;
    layer.AmbientTermMax = kCirrusAmbientTermMax;
    return layer;
}

// 雲(1層ぶん)の透過率と散乱光を求める。
// 【P17でレイベースになった】以前は「dir.y > 0を確認してから呼ぶこと」という前提があり、
// 呼び出し側(SkyColor)が dir.y <= 0 で早期脱出していた。P17でレイと層のスラブ交差を
// 世界座標で解くようになったため、その前提は要らなくなった——見上げても見下ろしても、
// 層の下・中・上のどこに視点があっても、同じ1本の式で解ける。層に当たらないレイは
// 交差が空になり、透過率1・散乱光0という中立元がそのまま返る。
// 【P11で層のパラメータを引数化】以前はSkyParametersから直接params.Cloud*を読んでいたが、
// 積雲・巻雲の式を2つに複製しないよう、層ごとの設定をCloudLayerParamsへまとめて渡す形にした。
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
// ウェザーマップ(Texture2D、KURENAI_CLOUD_WEATHER_REGISTER)も同じ作法だが、
// **こちらは定義しなくても手続きで評価する経路が残る**ので上の節で別に宣言している
// ============================================================================
#ifdef KURENAI_CLOUD_SHAPE_REGISTER
#include "Samplers.hlsli"
Texture3D CloudShapeNoiseTexture : register(KURENAI_CLOUD_SHAPE_REGISTER);
Texture3D CloudDetailNoiseTexture : register(KURENAI_CLOUD_DETAIL_REGISTER);
#define KURENAI_CLOUD_VOLUME 1
#endif

// レイマーチのステップ数の上限と、1歩の長さの下限[m](C2)。**コストの主要なつまみ**。
//
// 【C2でステップ数固定をやめた理由】C2より前は「スラブを常に12等分する」だったため、
// 1歩の長さが厚みに比例して伸びた(厚み400mで33m、1,500mなら125m)。ディテールノイズの
// 縦方向の周期は kCloudDetailVerticalPeriod = 67m なので、
// 厚みを上げるとステップが特徴と正面衝突し、**層状の縞(スライス)**になる。
// しかも開始位置が全画素で揃っていたため、縞が画面全体で揃った帯として出ていた(最悪の出方)。
//
// C2では1歩の長さをワールド空間の量として決める:
//   stepLength = max(スラブ内の経路長 / kCloudMaxRaymarchSteps, kCloudMinStepMeters)
// これで厚みを変えても1歩の長さが暴れない。上限のステップ数は経路が長いとき
// (地平線際)にコストが発散しないための歯止めで、そこでは1歩が伸びるが、
// もともと霞に埋もれて見えない領域なので実害が無い。
//
// 下限12mは「ディテールの縦周期67mを5〜6分割する」細かさ。これ以上細かくしても
// ノイズテクスチャが持っている情報より細かくならないので意味がない
//
// 【H3で48から384へ上げた】ウェザーマップを焼いて1歩が8倍安くなったため。
//
// 【48歩では何が落ちていたか】雲を止めて同じ場を撮り、仰角の帯ごとに雲の占有率を測った
// (48歩を2回撮った差=ノイズ下限は0.1ポイント以下):
//   仰角  0〜 4度   48歩 3.6% / 96歩 6.1% / 192歩 8.0% / 384歩 8.9%
//   仰角  4〜12度   48歩15.9% /     18.6% /     19.8% /     20.4%
//   仰角 12〜20度   48歩 2.9% /      3.3% /      3.4% /      3.5%
// 損失は**経路が長い低い仰角に集中する**。1歩は max(経路長/歩数, 12m) なので、
// 仰角2度では経路が30kmの上限に張り付いて1歩が625mになり、500m級の雲を跨いでしまう
// (オフラインで基準を均一24mに取ると、仰角2度で-23.8ポイント、3度で-12.1、
//  4度で-4.7、8度で-2.7、12度で-0.8、20度で+0.3)。
// 絵でも、384歩では水平線際に薄い雲の帯が現れ、雲の縁の階段状のがたつきが消える。
//
// 【GPU時間(実機。同じ条件で6枚ずつ撮った中央値)】
//   手続きのまま  48歩 5.64ms / 192歩 10.58ms / 384歩 19.11ms
//   焼いたあと    48歩 4.50ms / 192歩  5.93ms / 384歩  5.93ms
// 192歩と384歩で差が出ないのは、下限12mに当たって歩数を使い切らない視線が大半のため。
// **384歩でも現状(5.64ms)から+0.29msに収まる。**
// 【重くなったときは192へ落とす】上の実測どおり192でもこのシーンでは同じ時間で、
// 4〜12度の帯が19.3%(384は19.7%)まで戻る。最悪ケースの歩数が半分になる
static const int kCloudMaxRaymarchSteps = 384;
static const float kCloudMinStepMeters = 12.0f;

// 3Dノイズの水平・垂直の尺度。**水平は「何周するか」の回数、垂直はワールド距離[m]**。
//
// 【C7で厚みに比例させたが撤去した】水平のセルの広さも垂直の周期も厚みに比例させ、
// 厚みを「雲の大きさ」のつまみにしていたが、「厚みを上げても横幅が広がったように
// 見えない」という判断で外した。実測では実効セル幅は厚みに正確に比例していた
// (厚み600/1200/2400で493/997/1988m)ものの、同時に雲の背が高くなって空が埋まるため、
// 幅の変化がそちらに飲み込まれて見えなかった。
// **根本の問題は別にある**: 密度が2次元のウェザーマップの掛け算で決まるため、
// 雲の輪郭が高さによって変わらない(同じ形が積み上がるだけになる)。
//
// 【縦がスラブ内で何周するかが立体感を決める】密度がスラブ内で縦にほとんど変わらないと、
// 2次元のウェザーマップの輪郭が雲底から雲頂まで押し出された形になる。画面ではこれが
// **まっすぐな縦の側面**として見え、雲ではなく角柱・円筒に見える。実測でもこれが決定的で、
// 項を1つずつ止めた切り分けでは、ウェザーマップだけを残すと直線が全部残り、
// 3Dノイズだけを残すと直線が1本も出なかった。
//
// 【水平を回数で持つ理由: 巻き戻しを整数にするため】CPUは風のスクロール量を
// kCloudNoisePeriodでfmodして巻き戻す。そのとき3Dテクスチャの座標が整数ぶんずれていないと
// **巻き戻した瞬間に模様が飛ぶ**。水平の繰り返し数を整数にしておけば、セルの広さ(=厚み)を
// どう変えてもこの条件が自動的に満たされる。
// (C6で水平周期を1,500mと直接指定していたときは 256/1.5 = 170.67 で整数でなく、
//  この飛びが起きる状態だった。回数で持つ形にして構造的に断つ)
//
// 【値の根拠】厚み1,200m・巻雲オフ・波を止めた条件で水平4段×垂直3段を実測し、参考写真と
// 同じ物差し(縁の強さ・塊の数・面積の中央値・縦横の勾配比)で比べた。水平の値は
// 周期[m] = kCloudNoisePeriod / 回数 × セルの広さ(1,000m) で読み替えている:
//   水平4000 垂直4000(C5) … 縁0.65 塊 8 面積3644 比1.07  ← 巨大な塊に融合。円筒の正体
//   水平4000 垂直 400     … 縁0.97 塊14 面積2497 比1.28
//   水平2000 垂直 400     … 縁0.98 塊18 面積 220 比1.32
//   水平1500 垂直 450     … 縁0.99 塊22 面積  98 比1.32
//   水平1500 垂直 300     … 縁1.06 塊27 面積 107 比1.33  ← 採用(171回 / 4回)
//   水平1500 垂直 200     … 縁1.24 塊24 面積 115 比1.24  (筋状に痩せる)
//   水平1000 垂直 250     … 縁1.33 塊39 面積  76 比1.26  (刻まれすぎ)
//   参考写真               … 縁1.64 塊21 面積  99 比1.42
// なお「水平と垂直を等しくしたまま周期だけ縮める」(等方500m)も試したが、縦横の勾配比が
// 0.995と等方になり写真の1.42から離れた。空の雲は縦の変化の方が強いので等方ではない。
// 【D1で縦横比を参考写真に合わせた】以前は 水平1,497m × 垂直300m の **5:1に扁平**な塊で、
// それがスラブ内に4層積んでいた。すれすれの角度から見ると横縞の積層に見え、
// 「同じ形のスライスがただ積み上がっている」という見え方になっていた。
//
// 【指標】雲の**内側**(3×3がすべて雲の画素)での縦横の勾配比。平たい塊が縦に積むと
// 内部に横縞ができて |dI/dy| が |dI/dx| を上回る。丸い房なら等方に近づく。
// 水平を400mに固定して縦横比だけを振った実測:
//   1497×300 (5:1)   |dx|0.68 |dy|1.22 比1.80  ← 以前。横縞の積層
//    400×400 (1:1)   |dx|1.21 |dy|1.25 比1.04  (等方は行き過ぎ)
//    400×267 (1.5:1) |dx|1.22 |dy|1.51 比1.24  ← 採用。写真とほぼ一致
//    400×200 (2:1)   |dx|1.27 |dy|1.73 比1.37
//    400×133 (3:1)   |dx|1.33 |dy|1.80 比1.35
//   参考写真          |dx|1.71 |dy|2.08 比1.22
// 【内部のディテール量は写真に届いていない】比(1.24)は合ったが絶対量(1.22)は写真(1.71)より低い。
//
// 【この指標へ差し替えた理由】以前は地平線際の帯で「縁の強さ・塊の数・面積」を測って決めていたが、
// それは**雲と空の境界**を見る指標で、**雲の内側が層状か塊状か**を見ていなかった。
// 扁平なノイズは境界を細かくするので指標上は良く見え、実際の見た目は悪化していた。
//
// 【水平は繰り返し数、垂直はワールド距離】水平の繰り返し数は**整数でなければならない**。
// CPUがスクロール量をkCloudNoisePeriod(256セル)で巻き戻すので、整数でないと
// 巻き戻した瞬間にテクスチャ座標が半端にずれて模様が飛ぶ。
// 周期[m] = 256 ÷ 繰り返し数 × セル幅(1,000m)。171回 = 1,497m
//
// 【D2で粗くした理由】しきい値化にしたことで、形状ノイズが**輪郭そのもの**を決めるようになった。
// 掛け算だった頃は輪郭をウェザーマップ(1,000mセル)が決め、形状ノイズは内側を彫るだけだったので
// 400mでよかった。しきい値化では400mの細かさがそのままシルエットの細かさになり、砕けて見える。
// 4条件を実機で撮って内側の勾配を測った(縦横比は1.5:1に固定):
//   水平 400m  |dx|3.00 |dy|3.39 比1.13
//   水平 640m  |dx|2.43 |dy|2.86 比1.18
//   水平1000m  |dx|2.20 |dy|2.70 比1.23
//   水平1497m  |dx|2.02 |dy|2.49 比1.23  ← 採用。比は1000mと同じで、絶対量が写真に近い
//   参考写真   |dx|1.71 |dy|2.08 比1.22
// 【H1cで171→86・998→1984へ】被覆率を上げると**遠方まで一定間隔の繰り返し**が見えていた。
// 原因は形状テクスチャの中身で、CloudNoiseGenerate.hlsl の kShapeBaseCells が4だったため
// **128^3のタイル1枚に大きな特徴が4x4=16個しか無かった**。ワールドでは1セル524mで、
// タイルは 1400*256/171 = 2,096m ごとに繰り返す。特徴の語彙が少なすぎるので、
// タイルの継ぎ目をどう工夫しても「同じ大きさの塊が一定間隔」に見える。
//
// kShapeBaseCells を8へ上げると同時にここを半分にすることで、
// **雲の特徴の大きさは524mのまま**、タイルは4,167mごと・特徴は8x8=64個になる。
// (セル数だけ上げると特徴が小さくなり、周期だけ伸ばすと雲がのっぺりする。両方を同時に動かす)
// 縦のノイズ座標を作るときの基準になる厚み[m]。**[Cloud] Thickness とは独立**で、
// スラブが実際に何メートルでも、3Dノイズの縦は常にこの厚みぶんを通る
// (理由と実測は CloudSampleDensity の slabHeightMeters のコメント)。
// 値は下の縦周期・コントラスト・正規化係数をすべて測ったときの厚みに合わせてある。
// **ここを変えたら kCloudShapeContrastLow/High と kCloudVolumeDensityNormalize を測り直すこと**
static const float kCloudNoiseReferenceThickness = 1200.0f;

static const float kCloudShapeRepeats = 86.0f;
static const float kCloudShapeVerticalPeriod = 1984.0f;

// 【H1c: 形状テクスチャを2回引いて混ぜる】上のセル数だけでは遠方の繰り返しが残った。
// 水平線際は100km以上先まで見えるので、4,167mのタイルが25回以上並んで細かい規則模様になる。
//
// **86と61は互いに素**(86=2*43、61は素数)なので、両方が同時に巻き戻るのは256セルごと。
// つまり**合成した場は358kmでしか繰り返さない**。これで遠方の規則性が消えることを実機で確認した。
//
// 【順序が重要だった】セル数を上げる前に2回引きだけを試したときは効かなかった。
// 特徴が16種類しか無い状態では、2つのタップが**同じ16種類**を使うため
// 「並びの周期」を伸ばしても「語彙の少なさ」が残るため。**語彙を増やしてから並びを崩す。**
//
// 【縦周期は縦横比を保つ】1984 * 86/61 = 2797m。
// 【コスト】3Dテクスチャのフェッチが2回→3回になる
static const float kCloudShapeRepeatsB = 61.0f;
static const float kCloudShapeVerticalPeriodB = 2797.0f;
// 2つのタップが同じ場所を引かないようにずらす。値そのものに意味は無い
static const float3 kCloudShapeUvOffsetB = float3(0.37f, 0.71f, 0.23f);
static const float kCloudShapeMixB = 0.5f;
// ディテールノイズ。縁を削るための高周波成分なので形状の1/4の細かさにし、
// 縦横比(1.5:1)は形状に合わせる。683回 = 374m。
// 【縦周期の下限】レイマーチの1歩の下限が12mなので、40mを下回るとサンプルが足りず
// ざらつきに化ける。250mはその上
static const float kCloudDetailRepeats = 683.0f;
static const float kCloudDetailVerticalPeriod = 250.0f;
// ディテールで縁を削る強さ。0で削らない(形状そのまま)、大きいほど輪郭が房状に痩せる
static const float kCloudDetailErode = 0.35f;
// 【F4: 浸食を高さで変える】乾燥空気の巻き込み(エントレインメント)は物理では
// **雲頂で強く、雲底では起きない**。雲底は混合の面ではなく凝結が始まる高度そのもので、
// 境界層がよく混ざっているためどこでも同じ高さになり、輪郭は鋭く平らなまま残る。
// 一方で雲頂はサーマルが浮力を失って周囲へ吐き出される場所なので、房状にほつれる。
// 変更前の浸食は高さに依らず一様で、**平らであるべき雲底にも同じだけ穴を開けていた**。
//
// 【平均を1に保つ】lerp(a,b,hf)のhf∈[0,1]での平均は(a+b)/2なので、a+b=2に取れば
// スラブ内の浸食の総量は変わらない。「上で強く」であって「全体を強く」ではないことが
// これで保証される。
//
// 【0.0と2.0の根拠】オフラインでスラブを格子で埋め、**厚みのあるカラム**(雲のセルが
// スラブの30%以上あるカラム。雲の縁のカラムを混ぜると雲底の面ではなく輪郭を測ってしまう)
// の雲底/雲頂について、隣のカラムとの高さ差の平均[m]を測った。カラム占有率は案ごとに
// 被覆率を振り直して揃えてある:
//   浸食            雲底の粗さ  雲頂の粗さ  頂/底
//   底2.0 頂0.0(対照)   12.5 m     14.2 m    1.14  ← 符号を逆にすると逆へ動く
//   一様(変更前)        11.5 m     15.9 m    1.38
//   浸食なし            11.9 m     17.5 m    1.47  ← 一様な浸食は非対称を**減らしていた**
//   底0.3 頂1.7         10.3 m     16.6 m    1.61
//   底0.0 頂2.0         10.0 m     17.0 m    1.70  ← 採用
// **この2つを変えたら kCloudVolumeDensityNormalize を測り直すこと**
static const float kCloudErodeAtBase = 0.0f;
static const float kCloudErodeAtTop = 2.0f;
// 3Dの変調のスラブ内平均を**平面レイヤーだったときの値へ**揃える係数。
// **見た目の調整値ではなく実測から逆算した値。**
//
// 【揃える先が1ではない理由】(D2) 旧式は density = weather × (平均1へ揃えた3D変調) で、
// スラブ内の平均光学的深さは mean(weather) だった。これが平面レイヤーのときの値であり、
// CloudDensity(消散係数)と被覆率の意味はどちらもここから来ている。
// D2で weather がしきい値の中へ入ったので、**全体の平均を1へ揃えてはいけない**。
// そうすると被覆率を下げるほど残った雲が濃くなって相殺し、被覆率の意味が壊れる。
//
// 【測り方】CloudNoiseGenerate.hlslのベイクを128^3/32^3の格子とRGBA8の量子化まで含めて
// オフラインで再現し(**解析式の直接評価では駄目**。ディテールの最高オクターブは32^3に
// 24セルで激しくエイリアスするため実機と別物になる)、トライリニアで引いて
// スラブ内を一様に40万点サンプルした。再現が正しいことは、D2以前に記録済みの
// rawBase 0.784/0.055・base 0.455/0.329 を再現できる(得た値 0.780/0.054・0.443/0.327)ことで確認済み。
//
// 【被覆率にほぼ依らない】揃える先を mean(weather) にすると係数は被覆率0.15〜0.75で
// 3.59 / 3.51 / 3.48 / 3.50 / 3.56 / 3.64 / 3.82 とほぼ一定になり、定数として成立する:
//   被覆率0.30: mean(weather) 0.0485 ÷ mean(3D変調) 0.0139 = 3.501
// 【kCloudDetailErode・高さプロファイル・浸食・ノイズの生成方法のいずれかを変えたら測り直すこと】
// 【F2bで測り直した】CloudFbmを帯域制限へ、さらに大小2つのpopulationのmaxへ変えたため。
// 被覆率0.05〜0.35で 3.654 / 3.599 / 3.600 / 3.566 / 3.652 とほぼ一定で、
// 定数として成立している(帯域制限のみのときは3.587だった)
// 【F4で測り直した】浸食を高さで変えたため。同じオフライン再現・同じ被覆率
// (0.04/0.06/0.10/0.20)で変更前と変更後を測り、その比を上の3.610へ掛けた:
//   変更前(一様) 3.667 → 変更後(底0.0 頂2.0) 3.600  比 0.9817
//   3.610 x 0.9817 = 3.544
// 【比を使う理由】オフライン再現の絶対値には測る被覆率と乱数の種による揺れがあり、
// 3.610自体は別の被覆率の組で測った値である。差し替えるのは「浸食を変えたぶん」だけなので、
// 同じ条件で測った変更前後の比を掛けるのが正しい
// 【Gで測り直した】帯の峰を緩め、remapの上端を被覆率で動かすようにしたため。
// 同条件(被覆率0.04/0.06/0.10/0.20)で 3.600 → 3.837、3.544 x 3.837/3.600 = 3.777。
// 被覆率ごとに 3.806 / 3.784 / 3.874 / 3.868 とほぼ一定で、定数として成立している
// 【H1で測り直した】帯の峰を0.45→0.30へ緩めたため。運用点が0.17へ動いたので測る被覆率も
// そこへ合わせ(0.10/0.17/0.25/0.40)、同条件で 3.947 → 3.903、3.777 x 3.903/3.947 = 3.736
// 【H1cで測り直した】ベイクの基本セル数を8へ、形状テクスチャを2回引きへ変えたため。
// 同条件(被覆率0.10/0.16/0.25/0.40)で 3.959 → 4.121、3.777 x 4.121/3.959 = 3.931。
// 変更前として測った3.959は別経路で測った3.903とよく一致しており、測り方の妥当性の確認になっている
static const float kCloudVolumeDensityNormalize = 3.931f;
// 形状ノイズにコントラストを付ける範囲。**実測した分布から決めた値**で、
// remap(shape.r, WorleyFbm(gba) - 1, 1) の出力は平均0.784・標準偏差0.055とほとんど定数だった
// (下限が -0.6 付近になるため [0,1] が [0.375, 1] へ圧縮される)。このままだと3Dの形が
// ほとんど効かないので、実測の15%点(0.726)〜92%点(0.856)を[0,1]へ引き伸ばす。
// 変換後は平均0.455・標準偏差0.329となり、雲の芯と隙間がはっきり分かれる。
// 【この2つを変えたら上のkCloudVolumeDensityNormalizeを測り直すこと】
// 【H1cで測り直した】ベイクの基本セル数を4→8へ上げ、形状テクスチャを2回引いて混ぜたため。
// 混ぜると分散が下がる(rawBaseの標準偏差 0.054 → 0.039)ので、伸張範囲を測り直さないと
// 雲が薄くなる。取り直した結果 base の分布は変更前と一致する:
//   変更前(4セル・1回引き) base 平均0.454 標準偏差0.329
//   変更後(8セル・2回引き) base 平均0.455 標準偏差0.330
static const float kCloudShapeContrastLow = 0.746f;
static const float kCloudShapeContrastHigh = 0.840f;

// 【C4で縦プロファイルを「雲の種類」で切り替えるようにした】
// C4より前は全ての雲が同じ1つのプロファイル(平らな底・丸い頭)だったため、空一面に
// 同じ高さ・同じ大きさの塊が並ぶ層雲寄りの見た目になっていた。参考写真は「大きさの違う
// 積雲が散らばる」空なので、場所ごとに雲の背の高さが変わる必要がある。
//
// 種類は3つ。いずれも hf=0 が雲底、1 が雲頂(スラブの上端)で、**スラブの厚みは共通のまま
// 各種類が使う高さの範囲だけが違う**。これで1つの層に背の低い雲と高い雲が同居する。
//   層雲(0.0)     … スラブ下部に張り付いた薄いシート
//   積雲(0.5)     … 平らな底と丸い頭。スラブの半分ほどまで立ち上がる
//   雄大積雲(1.0) … スラブいっぱいに立ち上がる
// しきい値はいずれも見た目からの調整値で物理的な導出ではない
static const float kCloudProfileBaseSoftness = 0.10f;
static const float kCloudProfileStratusTop = 0.12f;
static const float kCloudProfileCumulusTop = 0.45f;
static const float kCloudProfileCongestusTop = 0.80f;

// 種類の場を引く周波数(被覆率の場に対する比)と位相のずらし。
// 【被覆率より低い周波数にする】種類が被覆率と同じ細かさで変わると、隣り合う塊ごとに
// 背の高さがばらばらになって「1つの雲」に見えなくなる。低くすることで、ひとかたまりの
// 雲は同じ種類を共有しつつ、離れた領域では種類が変わる。
// 【0.25という値】kCloudNoisePeriod(256)に掛けて64という整数になる必要がある。
// 周期をセル数の整数で持てないと、floorベースの巻き戻しがタイル境界で揃わず筋が出る
static const float kCloudTypeUvScale = 0.25f;
// 被覆率の場と相関しないように位相をずらす。値そのものに意味は無い
static const float2 kCloudTypeUvOffset = float2(137.0f, 71.0f);

// 【雲の大きさをまばらにする】(C7) 局所的な被覆率を「雲の種類」の場に結び付ける。
// 背の高い雲になる場所は被覆率も高くなって大きな塊にまとまり、背の低い場所は
// 被覆率が下がって小さくまばらになる。空一面が同じ大きさの塊で埋まるのを防ぐ。
//
// 【この結び付けが物理的に正しい向きである理由】積雲は対流のセルなので、深い上昇流ほど
// 水平方向のセルも大きい。雄大積雲が小さい塊で、層雲が巨大、という空は実在しない。
//
// 【追加コストがほぼ無い理由】CloudTypeAtは元々サンプルごとに呼んでいる(縦プロファイルの
// 選択に使う)。呼ぶ順をウェザーマップより前へ動かして使い回すだけで、新しいノイズは引かない。
//
// 【Coverageの意味を保つ】lerp(a,b,type)のtypeの平均は0.5なので、(a+b)/2 = 1になるよう
// 対称に取る。ただし被覆率からの整形(CloudRemap)は非線形なので平均の被覆率は厳密には
// 保存しない。**既定のCoverageは測り直すこと**
static const float kCloudCoverageAtStratus = 0.45f;
static const float kCloudCoverageAtCongestus = 1.55f;

// 【ウェザーマップを実測した分布へ合わせる】(D2)
// CloudFbm の生値(weatherN)を実機から読み出して測ったところ **平均0.500・標準偏差0.127**、
// 1%点0.243 / 99%点0.781 / 99.9%点0.839 だった。ところが以前の式は
//     weather = saturate(remap(weatherN, 1 - Coverage, 1.0))
// と**上端を1.0に固定**しており、weatherNが平均から4σ離れた1.0へ決して届かないため、
// weather が値域の下の方へ潰れていた(実測: 雲のある画素でも中央値0.134、95%点0.395)。
// これが次の3つをまとめて説明する:
//   ・密度をしきい値化する形が過去に崩壊した(base > 1-weather をほぼ満たせない)
//   ・kCloudVolumeDensityNormalize に4.315という大きな補正が要る
//   ・Coverageが過敏(0.25→0.30で空の占有率がほぼ倍になる)——正規分布の裾で動かしているため
// base 側には既に実測分布で引き伸ばす処理(kCloudShapeContrastLow/High)が入っており、
// ウェザーマップだけ入っていなかった。
//
// 【新しい式】被覆率でしきい値を1%点〜99%点の間で動かし、上端は99.9%点の少し上へ置く:
//   threshold = lerp(High, Low, localCoverage)
//   weather   = saturate(remap(weatherN, threshold, Max))
// 上端をLowではなくMaxにするのは、被覆率が小さいとき threshold が High に近づいて
// remapの分母が0になるのを避けるため(分母は最小でも Max - High = 0.119)。
// 【F2で測り直した】CloudFbmの重みを帯域制限へ変えたので場の分布が動いた
// (平均0.498 / 標準偏差0.159。旧: 標準偏差はもっと小さかった)。
// **CloudFbmの重みを変えたら、この3つを必ず測り直すこと。**
// 【G→H1で測り直した】kCloudFbmWeightLarge の峰を 0.72→0.45→0.30 と緩めたため。
// 峰を緩めるほど場は平均へ寄る(標準偏差 0.159 → 0.116 → 0.101。平均は常に0.498)。
// **帯ごとのUV位相(kCloudFbmBandOffset)を入れた状態で測ること。**
static const float kCloudWeatherLow = 0.269f;    // 実測1%点
static const float kCloudWeatherHigh = 0.727f;   // 実測99%点
static const float kCloudWeatherMax = 0.850f;    // 実測99.9%点の少し上。remapの上端

// 【G: 被覆率が1へ近づいたらremapの幅も詰める】
// 上の3つだけでは**被覆率1にしても空が埋まらない**。理由は remap の上端が固定だから:
//   weather = saturate(remap(weatherN, Low, Max))
// は場を[0,1]へ引き伸ばしているだけなので、被覆率1でも weather の平均は0.42にしかならない。
// 「被覆率1」が「しきい値が場の1%点にある」という意味にしかなっておらず、
// 「どこも濃い」という意味になっていなかった。
//
// ここで上端を下端へ寄せると、被覆率1で weather がほぼ全面1へ張り付く。
// 【実測(仰角ごとに視線を192本ずつ飛ばし、空が雲で覆われる割合)】
//   被覆率1.0        仰角5度  仰角20度  仰角45度  真上
//   変更前            100.0%    91.1%    83.3%   76.0%
//   上限だけ直す      100.0%    94.8%    89.6%   83.9%
//   上限+この幅詰め   100.0%   100.0%    97.9%   98.4%  ← 採用
// **真上が上限を直接映す方向である。** 雲層は厚み1,200mの水平スラブなので、
// 真上の視線はスラブを最短で抜け、投影された占有率が場の点の割合そのものになる。
// 水平線際は視線がスラブ内を13.8km進んで途中の雲を全部拾うため、上限が隠れて見えない。
// (F1を「不要」として一度計画から外したのは、水平線寄りの画角だけで測っていたため)
static const float kCloudWeatherNarrowSpan = 0.05f;

// 小さい雲のpopulation(F2b)。重みが違うので分布も違う(平均0.499 / 標準偏差0.125)。
// **kCloudFbmWeightSmall を変えたらこの3つも測り直すこと**
static const float kCloudSmallWeatherLow = 0.221f;
static const float kCloudSmallWeatherHigh = 0.778f;
static const float kCloudSmallWeatherMax = 0.909f;
// 小さい雲の被覆率を、大きい雲の被覆率の何倍にするか。
// 0で小さい雲が消える(=帯域制限だけの状態へ戻る)、1で同じ被覆率
static const float kCloudSmallCoverageScale = 0.6f;

// 【高さによる浸食】(D2) 上へ行くほどしきい値を上げ、雲の輪郭を内側へ縮める。
// hf の2乗にしてあるのは、雲底付近は平らなまま保ち、上半分で一気に細らせるため
// (積雲は底が平らで頭が丸い)。0で浸食なし=以前の押し出された角柱に戻る。
// **輪郭がどう縮むかは3Dノイズ任せ**なので、単なる相似縮小ではなく高度ごとに違う形になる。
//
// 【0.25の根拠】マーチ中のhfを定数に固定するとシルエットがその高度の水平断面になるので、
// 0.2/0.5/0.8で撮って断面の面積と重なり率(IoU)を測った:
//   浸食  hf0.2    hf0.5    hf0.8   0.8/0.2  IoU(0.2対0.8)
//   0.15  539816   441272   267911   49.6%    0.275
//   0.25  537160   401960   174809   32.5%    0.211  ← 採用
//   0.35  534201   363945   128081   24.0%    0.169
//   0.55  529651   309416    57424   10.8%    0.105
// **IoUだけで決めてはいけない。** 0.55はIoUが最も低いが、それは「上と下で形が違う」のではなく
// 雲頂がほぼ消滅したため。面積が単調に減りつつ雲頂が残る(3割)ところを取る。
// なお0.15〜0.55の差は等倍の絵では区別がつかない(画面に出るのは雲の側面の外郭で、
// 削っているのは雲頂だから)。だから絵ではなくこの構造で決めている
// 「被覆率1.0で全面曇り」を保証するための2定数。**これは物理からの意図的な逸脱**で、
// 実際の積雲は補償下降流のせいで雲量3〜5割で頭打ちになり、空を埋めることはない。
// つまみを端まで使えるようにするというアートの要求として入れてある。
//
// 【しきい値を持たせない】最初は被覆率0.85から smoothstep で立ち上げていたが、
// 「つまみのある一点から急に性質が変わる」形はやめる方針。代わりに localCoverage
// (=weather×プロファイル−高さ浸食。その場所に雲が立つ度合いそのもの)のべき乗で連続に効かせる。
//   ・雲が立たない場所(localCoverage=0)では厳密に0なので、青空はそのまま青空のまま残る
//   ・べきが効くので運用値では無視できる大きさにしかならない
//     (被覆率0.14ではlocalCoverageの最大が約0.2で、下限は 0.25*0.2^4 = 0.0004)
//   ・被覆率1.0ではプロファイルの頂点付近で0.25に達し、穴を埋める
// 0.25 と 4.0 は実機で確かめた値。真上(Pitch70)・厚み300・雲の種類を層雲へ振り切った
// 条件でも、わずかでも空が抜けた画素が0.00%になる。使い方は CloudSampleDensity の最後を参照
static const float kCloudOvercastDensityFloor = 0.25f;
static const float kCloudOvercastFalloff = 4.0f;
// 下限の濃淡の下側。1.0で濃淡なし(一様な板)、小さいほど遠方に明暗が残るが、
// 全面曇りの保証は **kCloudOvercastDensityFloor × これ** のほうで決まる
static const float kCloudOvercastFloorMin = 0.5f;

static const float kCloudHeightErosion = 0.25f;

// 高さによる密度の勾配。積雲は雲頂ほど凝結が進んで密度が高い。
// 【平均を1に保つ】lerp(a,b,hf)のhf∈[0,1]での平均は(a+b)/2なので、0.6と1.4にすることで
// 平均1になり、kCloudVolumeDensityNormalizeを動かさずに済む。
// 見た目としては「底が透けて頭が詰まっている」になり、平らな雲底が出る
static const float kCloudDensityGradientBottom = 0.6f;
static const float kCloudDensityGradientTop = 1.4f;

// 【C1で kCloudTopShadowScale を撤去した】「サンプルより上にどれだけ雲が残っているか」を
// exp(-(1-hf)*k) の解析的な縦勾配として扱う係数だった。位置に依らない単調勾配なので
// 形を持たず、実測でも雲の明るさへの寄与は +3.6(雲の90%点と空の中央値の差54に対して)
// しかなかった。C1で太陽方向への3Dマーチへ置き換え、縦の勾配も実際の雲の分布から出るようにした。
// あわせて「スラブ内平均を1に戻す」ための正規化(topShadowNormalize)も要らなくなった。

// その場所の雲の種類(0=層雲 / 0.5=積雲 / 1=雄大積雲)。C4。
// 【1オクターブの値ノイズで足りる理由】種類は低周波の場なので、fBmの高周波成分が要らない。
// この関数は視線マーチだけでなく太陽マーチのサンプルごとにも呼ばれるため、
// CloudFbm(4オクターブ=16ハッシュ)ではなく値ノイズ1回(4ハッシュ)にしてある
// ベイクが焼く生の値。typeBiasは実行時に変わるので**焼き込まない**
float CloudTypeNoiseProcedural(float2 noiseXZ)
{
    return CloudValueNoise(
        noiseXZ * kCloudTypeUvScale + kCloudTypeUvOffset, kCloudNoisePeriod * kCloudTypeUvScale);
}

float CloudTypeAt(float2 noiseXZ, float typeBias)
{
    // 【CloudFbmBandsとは別のUVで引く】上は noiseXZ * AnisotropicScale、こちらは noiseXZ。
    // 積雲は異方スケールが(1,1)なので同じ座標になるが、**式の上では別物なので分けて引く**。
    // まとめると異方スケールを持つ層で静かに間違う
#ifdef KURENAI_CLOUD_WEATHER_TEXTURE
    const float n = CloudWeatherNoiseTexture.SampleLevel(
        VolumeSampler, noiseXZ / kCloudNoisePeriod, 0.0f).b;
#else
    const float n = CloudTypeNoiseProcedural(noiseXZ);
#endif
    // 値ノイズは[0,1]でおおむね平均0.5。typeBias=0.5が中立で、
    // 下げると層雲寄り、上げると雄大積雲寄りへ空全体が寄る
    return saturate(n - 0.5f + typeBias);
}

// 積雲の高さプロファイル。hf=0が雲底、hf=1が雲頂。cloudTypeで3種を補間する(C4)
float CloudVerticalProfile(float hf, float cloudType)
{
    const float base = smoothstep(0.0f, kCloudProfileBaseSoftness, hf);
    const float stratus = base * (1.0f - smoothstep(kCloudProfileStratusTop, kCloudProfileStratusTop * 2.0f, hf));
    const float cumulus = base * (1.0f - smoothstep(kCloudProfileCumulusTop, 1.0f, hf));
    const float congestus = base * (1.0f - smoothstep(kCloudProfileCongestusTop, 1.0f, hf));

    // 0〜0.5は層雲→積雲、0.5〜1は積雲→雄大積雲。0.5でどちらの式も積雲になるので連続する
    return (cloudType < 0.5f)
        ? lerp(stratus, cumulus, saturate(cloudType * 2.0f))
        : lerp(cumulus, congestus, saturate((cloudType - 0.5f) * 2.0f));
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
// 【設計(D2で変わった): 3Dノイズが輪郭そのものを決める】
// weather と高さプロファイルは「3Dノイズに掛けるしきい値」として使い、密度に掛けない。
// 掛け算だと weather が輪郭を決める門になり、3Dノイズはその内側を彫るだけなので
// **輪郭が高さでほとんど変わらない**(同じ形が積み上がって見える)。しきい値にすれば
// 高さで輪郭そのものが変わる。
//
// 【この形は一度失敗しており、原因も分かっている】当初 saturate(CloudRemap(base, 1 - weather, 1))
// を試して密度が常に0へ落ちた。理由は2つあり、どちらもD2までに解消している:
//   ・base がほぼ定数だった(当時 平均0.781・標準偏差0.055)。C1のコントラスト伸張後は
//     平均0.455・標準偏差0.329
//   ・weather が値域の下へ潰れていた(remapの上端がweatherNの届かない1.0だった)。
//     D2で実測分布に合わせて直した(kCloudWeatherLow のコメント参照)
//
// なお kCloudVolumeDensityNormalize は「スラブ内の平均を1へ揃える」係数だが、
// **D2で合成の式が変わったので測り直しが必要**である。
// その場所のウェザーマップ(被覆率で整形済み)と雲の種類(C7)。
// 種類を先に求め、それで局所的な被覆率を上下させることで大きさをまばらにする
// (kCloudCoverageAtStratus のコメント参照)。cloudTypeは呼び出し側が
// CloudSampleDensityへそのまま渡し、二度引かないようにする
float CloudWeatherAt(float2 noiseXZ, CloudLayerParams layer, out float cloudType,
                     out float weatherLarge)
{
    // weatherLarge はしきい値化する**前**の大きい雲側のfBm。被覆率1.0では weather が
    // どこでも1へ張り付いて濃淡を失うので、曇り空にゆるい濃淡を残すためにこちらを外へ出す
    // (使い方は CloudSampleDensity の overcastFloor)
    weatherLarge = 0.0f;
    cloudType = CloudTypeAt(noiseXZ, layer.TypeBias);
    // 【G: Coverage=1でどこでも開ききるようにする】
    // 以前は saturate(Coverage * typeScale) だった。typeScale は層雲寄りで0.45なので、
    // **Coverage=1でも localCoverage が0.45で頭打ち**になり、空の半分近くが最後まで
    // 開かなかった(実測: Coverage=1でのlocalCoverageの最小値0.467)。
    // Coverage^2 の項を足すと、Coverage=1 で typeScale + (1 - typeScale) = 1 となって
    // どこでも開ききる一方、Coverage が小さいうちは Coverage^2 が無視できるので
    // 従来の挙動がそのまま残る(運用値0.04では 0.0180 → 0.0189。+5%)。
    // 【typeScale を捨てない理由】これはC7で入れた「背の高い雲の場所は大きな塊にまとまる」
    // という結び付きで、雲の大きさのばらつきの源。端だけを開けたいのであって
    // 途中の性質は変えたくない
    const float typeScale =
        lerp(kCloudCoverageAtStratus, kCloudCoverageAtCongestus, cloudType);
    const float localCoverage = saturate(
        layer.Coverage * typeScale + (layer.Coverage * layer.Coverage) * (1.0f - typeScale));
    // CloudRemapの分母は (1 - (1 - localCoverage)) = localCoverage なので、
    // 0のときは割らずに抜ける(この層に雲が無い場所)
    if (localCoverage <= 0.0f)
    {
        return 0.0f;
    }
    float largeN;
    float smallN;
    CloudFbmBands(noiseXZ * layer.AnisotropicScale, largeN, smallN);
    weatherLarge = saturate(CloudRemap(largeN, kCloudWeatherLow, kCloudWeatherHigh));

    // 実測分布に合わせたしきい値化(D2。定数側のコメントに根拠がある)。
    // localCoverage=0でしきい値は99%点(ほぼ雲なし)、1で1%点(ほぼ全面)
    // 【G】上端も被覆率で下端へ寄せる(kCloudWeatherNarrowSpan のコメント参照)。
    // 上端を固定にしていると、被覆率1でも weather は場を[0,1]へ引き伸ばしただけになり
    // 平均0.42にしかならない。max(..., threshold + 小さい値) は分母が0になるのを防ぐため
    const float largeThreshold = lerp(kCloudWeatherHigh, kCloudWeatherLow, localCoverage);
    const float largeTop = max(
        lerp(kCloudWeatherMax, kCloudWeatherLow + kCloudWeatherNarrowSpan, localCoverage),
        largeThreshold + 0.001f);
    const float large = saturate(CloudRemap(largeN, largeThreshold, largeTop));

    // 小さい雲(セルの隙間を埋める小さなサーマル)。被覆率は大きい雲に連動させる
    const float smallCoverage = saturate(localCoverage * kCloudSmallCoverageScale);
    const float smallThreshold =
        lerp(kCloudSmallWeatherHigh, kCloudSmallWeatherLow, smallCoverage);
    const float smallTop = max(
        lerp(kCloudSmallWeatherMax, kCloudSmallWeatherLow + kCloudWeatherNarrowSpan,
             smallCoverage),
        smallThreshold + 0.001f);
    const float small = saturate(CloudRemap(smallN, smallThreshold, smallTop));

    // 【重みを混ぜるのではなくmaxを取る理由】(F2b) 2組の重みを1つの場へ足し込むと、
    // populationが2つに分かれず「1つの場が2種類の細かさで波打つ」だけになる。
    // しきい値化は非線形なので、**しきい値化してから合成する**ことで初めて
    // 大きい雲と小さい雲という2つのpopulationになる
    return max(large, small);
}

// overcastFloorEnabled: 全面曇りの下限を効かせるなら1、切るなら0。
// 【太陽マーチでは必ず0を渡す】下限は「視線が層を貫いたときに必ず不透明にする」ための
// もので、雲の**明るさ**を決める自己影に混ぜてはいけない。混ぜていたときは被覆率1.0で
// 空全体が暗くなった(実測: Pitch20の空の明部90%点が全帯で16〜22下がり、
// **落ち幅が仰角によらずほぼ一定**だったことが視線側でなく太陽側だという証拠になった)
float CloudSampleDensity(float2 noiseXZ, float hf, float weather, float cloudType,
                         float weatherLarge, float overcastFloorEnabled,
                         CloudLayerParams layer)
{
    // 3Dテクスチャは Wrap で引くので範囲を気にせず掛けるだけでよい。
    // 【水平はセル単位の繰り返し数、垂直はワールド距離】noiseXZ はセル単位
    // (ワールド距離×UvScale)なので、「ノイズ周期256セルあたり何回繰り返すか」を掛ければ
    // テクスチャ座標になる。繰り返し数が整数なので、CPUがスクロール量を巻き戻しても
    // テクスチャ座標は整数ぶんしかずれず、模様が飛ばない。
    // 垂直はメートルで割る。hf(スラブ内の相対高さ)のままだと縦の周期が厚みそのものになり、
    // 厚みを上げるほど模様が縦へ引き伸びてしまう
    const float shapeUvScale = kCloudShapeRepeats / kCloudNoisePeriod;
    // 【縦の座標は実際の厚みではなく基準厚みで作る】ここを hf * layer.Thickness にすると、
    // **スラブが薄いほど3Dテクスチャの縦をわずかしか通らなくなる**。厚み1200では0.60周期
    // (1200/1984)を通るのに対し、厚み300では0.15周期しか通らないので、1本の柱の中で
    // base がほとんど変化せず、部分的な薄さが**柱まるごとの穴**に変わる。
    //
    // 【実測(Pitch20・被覆率1.0・青空 B-R>=90 の割合)】
    //   hf * layer.Thickness   厚み1200 0.5% / 厚み300 7.9%
    //   hf * 基準厚み          厚み1200 0.5% / 厚み300 0.1%
    // 厚み1200では置換が恒等式になるので両者が一致する(これが対照。ここが動いたら実験が壊れている)。
    //
    // 【なぜ基準厚みで固定してよいか】「縦がスラブ内で何周するかが立体感を決める」ことは
    // 下の kCloudShapeVerticalPeriod のコメントに実測で書いてある。その周期数が厚みで
    // 勝手に変わるのは意図に反する。さらに kCloudShapeContrastLow/High と
    // kCloudVolumeDensityNormalize は**厚み1200で測った値**なので、切り離して初めて
    // どの厚みでも較正が正しくなる。
    // 【C7で却下された案とは別物】C7は水平も垂直も厚みに比例させる案で、却下の理由は
    // 「厚みを上げても横幅が広がって見えない」こと。ここは縦だけの切り離し。
    //
    // 【プロファイルが雲を置く高さの幅でも割る】厚みを固定しても、**高さプロファイルが
    // 雲をスラブの一部へ閉じ込めると同じことが起きる**。層雲寄り(cloudType→0)の
    // プロファイルは hf > kCloudProfileStratusTop*2 = 0.24 で0になるので、雲が立つのは
    // スラブの下24%だけになり、そこを通る縦のノイズは0.145周期しかない。
    //
    // 【実測(Pitch20・被覆率1.0・青空 B-R>=90 の割合)】
    //   TypeBias         0.5    0.3    0.1
    //   割らない        0.5%   1.8%   5.6%
    //   支持幅で割る    0.5%   1.1%   0.2%
    // 高さプロファイルの雲の種類を0.5へ固定する対照では 0.8%/0.8%/0.7% となり、
    // **雲の種類による悪化はプロファイルが原因だと単独で確かめてある**。
    //
    // 【却下した2案(どちらも実測)】
    //   ・消散係数を上げる… 10→80で青空は 5.6%→10.1% と**増える**。穴は薄い雲ではなく
    //     密度がちょうど0の硬い穴なので、消散係数では埋まらない
    //   ・shapedのremapの上端を被覆率で狭める(旧計画のH2)… 0.1で5.6%→3.9%と半端で、
    //     しかも0.3では1.8%→2.2%と悪化する
    //
    // 【正規化係数を測り直さなくてよい】3Dテクスチャは周期的なので、同じ場をより長い
    // 範囲で引いても base の分布そのものは変わらない
    const float profileSupport =
        max(lerp(kCloudProfileStratusTop * 2.0f, 1.0f, saturate(cloudType * 2.0f)), 0.05f);
    const float slabHeightMeters = hf * kCloudNoiseReferenceThickness / profileSupport;
    // 【H1c: 2回引いて混ぜる】繰り返し数が互いに素なので、合成した場は358kmでしか
    // 繰り返さない(kCloudShapeRepeatsB のコメント参照)
    const float3 shapeUvw =
        float3(noiseXZ * shapeUvScale, slabHeightMeters / kCloudShapeVerticalPeriod);
    const float3 shapeUvwB =
        float3(noiseXZ * (kCloudShapeRepeatsB / kCloudNoisePeriod),
               slabHeightMeters / kCloudShapeVerticalPeriodB) + kCloudShapeUvOffsetB;
    const float4 shape = lerp(
        CloudShapeNoiseTexture.SampleLevel(VolumeSampler, shapeUvw, 0.0f),
        CloudShapeNoiseTexture.SampleLevel(VolumeSampler, shapeUvwB, 0.0f),
        kCloudShapeMixB);

    // Perlin-Worley(R)を、周波数を上げたWorley(GBA)を下限として引き伸ばす。
    // Rだけだと塊が丸すぎ、Worleyを重ねることで綿状の輪郭になる
    const float shapeFbm = CloudWorleyFbmFromChannels(shape.gba);
    const float rawBase = saturate(CloudRemap(shape.r, shapeFbm - 1.0f, 1.0f));
    // rawBase はほとんど定数(実測: 平均0.784・標準偏差0.055)なので、実測した分布の
    // 15%点〜92%点を[0,1]へ引き伸ばしてコントラストを付ける(定数のコメント参照)
    const float base = saturate(CloudRemap(rawBase, kCloudShapeContrastLow, kCloudShapeContrastHigh));

    // 【D2: 掛け算からしきい値化へ】以前は base に高さプロファイルとウェザーマップを
    // **掛けて**いた。掛け算だと weather が輪郭を決める門になり、3Dノイズはその内側を
    // 彫るだけなので、**輪郭が高さによってほとんど変わらない**(同じ形が積み上がる)。
    //
    // ここでは weather と高さを「3Dノイズに掛けるしきい値」にする。しきい値を上へ行くほど
    // 上げると輪郭が内側へ縮むが、**どこがどう縮むかは3Dノイズ任せ**なので、
    // 単なる相似縮小ではなく高度ごとに違う形になる。平らな底と房状の頭が出る。
    //
    // 【この形は過去に一度失敗している】そのときは weather が値域の下へ潰れていて
    // base が 1-weather を上回れず、密度が常に0へ落ちた。原因はウェザーマップのremapの
    // 上端が実際には届かない1.0だったことで、そこは kCloudWeatherLow のコメントのとおり直した。
    // いま base は平均0.455・標準偏差0.329、weather も[0,1]を使い切るので条件が違う。
    const float profile = CloudVerticalProfile(hf, cloudType);
    const float localCoverage =
        saturate(weather * profile - kCloudHeightErosion * hf * hf);
    if (localCoverage <= 0.0f)
    {
        // 雲底の直下・雲頂の直上と、浸食で消えた場所。ここで抜けると
        // ディテール(2枚目のテクスチャフェッチ)を丸ごと省ける
        return 0.0f;
    }
    // 【被覆率1.0は必ず全面曇りにする(アートの要求)】その場所に雲が立つ度合い
    // (localCoverage)のべき乗で密度に下限を与える。しきい値は持たせない
    // (kCloudOvercastDensityFloor のコメント参照)。
    //
    // 【なぜ形の側では保証できないか】残っていた穴は薄い雲ではなく**密度がちょうど0の硬い穴**
    // (形状ノイズが柱まるごと0に近い場所)で、実測でも消散係数を10→80に上げると青空は
    // 5.6%→10.1%と**増える**(穴が埋まらないまま縁だけ締まるため)。形の側を触る2案も測ったが
    // 届かなかった: 縦のノイズを支持幅で伸ばす案は TypeBias 0.5 で0.47%止まり、
    // shapedのremapの上端を狭める案(旧計画のH2)は0.4%止まりで TypeBias 0.3 では悪化した。
    //
    // 【localCoverageを使う利点】高さプロファイルが既に入っているので、雲底の下・雲頂の上は
    // 自動的に0になる。掛けなければ層が1枚の直方体になってしまう
    //
    // 【下限に濃淡を持たせる】一定値の下限を敷くと、**遠方だけ濃く見える**。
    // 下限は位置ごとに一定なので、その光学的深さは視線がスラブを通る距離に比例し、
    // 仰角2度では 1/sin = 28.7、仰角20度では 2.9 と**10倍の差**になる。しかも一様なので
    // 遠方は下限だけで埋まって明暗が消え、のっぺりした濃い塊になる
    // (実測: 被覆率1.0・Pitch20の空の明部90%点が 12〜20度で 240.0 → 219.3)。
    // そこで低周波の場で濃淡を付ける。使うのは**しきい値化する前の**大きい雲側のfBmで、
    // 穴を作っている形状ノイズとは独立なうえ、被覆率1.0でも濃淡を保っている
    // (weather 自体は1へ張り付いて使えない)。kCloudOvercastFloorMin が最も薄い場所の割合で、
    // 全面曇りの保証はこの最小値のほうで担保する
    const float overcastFloor =
        kCloudOvercastDensityFloor * pow(localCoverage, kCloudOvercastFalloff)
        * lerp(kCloudOvercastFloorMin, 1.0f, weatherLarge) * overcastFloorEnabled;
    // 高さによる密度の勾配(C4)。スラブ内の平均が1になるよう対称に取ってあるので、
    // 全体の光学的深さは変わらず「底が透けて頭が詰まる」形だけが加わる
    const float densityGradient =
        lerp(kCloudDensityGradientBottom, kCloudDensityGradientTop, hf);

    const float shaped = saturate(CloudRemap(base, 1.0f - localCoverage, 1.0f));
    if (shaped <= 0.0f)
    {
        // 【ここでディテールを引かずに済む】浸食は shaped=0 を0のままにするので、
        // 返す値は下限そのもの。下限を入れる前と同じく2枚目のテクスチャフェッチを省ける
        return overcastFloor * densityGradient * kCloudVolumeDensityNormalize;
    }

    // ディテールで縁だけを削る。密度が高い芯はほとんど削れず、薄い縁だけが房状に痩せる。
    // 座標の作り方は形状ノイズと同じ(水平はセル単位の繰り返し数、垂直はスラブ内の繰り返し数)
    const float detailUvScale = kCloudDetailRepeats / kCloudNoisePeriod;
    const float3 detailUvw =
        float3(noiseXZ * detailUvScale, slabHeightMeters / kCloudDetailVerticalPeriod);
    const float3 detail = CloudDetailNoiseTexture.SampleLevel(VolumeSampler, detailUvw, 0.0f).rgb;
    const float detailFbm = CloudWorleyFbmFromChannels(detail);
    // 【F4】浸食の強さを高さで変える(kCloudErodeAtBase のコメント参照)。
    // 【縁の項を足していない理由】計画では「浸食を(1 - shaped)にも比例させて芯を守る」と
    // していたが、remapの形が既にそれをしていた(実測: shaped>0.8 で削った後/削る前が0.997、
    // shaped<0.3 で0.258)。(1 - shaped)を掛けると逆に**縁の浸食が弱まる**(0.258→0.389)
    const float erodeStrength =
        kCloudDetailErode * lerp(kCloudErodeAtBase, kCloudErodeAtTop, hf);
    const float modulation = saturate(CloudRemap(shaped, detailFbm * erodeStrength, 1.0f));

    // 【構造は消えない】max なので、下限より濃い場所は元の密度のまま。下限が効くのは穴だけ。
    // 【weatherを掛けない】(D2) weather は上のしきい値の中へ入ったので、ここで掛けると二重になる
    return max(modulation, overcastFloor) * densityGradient * kCloudVolumeDensityNormalize;
}

// ワールド座標の1点の密度(C1)。太陽マーチが任意の位置を引けるように、
// hf / noiseXZ / weather をワールド座標から組み立てて上の CloudSampleDensity を呼ぶだけの薄い包み。
// スラブの外は0(層の外に雲は無い)
float CloudSampleDensityWorld(float3 worldPos, CloudLayerParams layer)
{
    const float hf = (worldPos.y - layer.Altitude) / layer.Thickness;
    if (hf < 0.0f || hf > 1.0f)
    {
        return 0.0f;
    }
    const float2 noiseXZ = worldPos.xz * layer.UvScale + layer.ScrollOffset;
    float cloudType;
    float weatherLarge;
    const float weather = CloudWeatherAt(noiseXZ, layer, cloudType, weatherLarge);
    if (weather <= 0.0f)
    {
        return 0.0f;
    }
    return CloudSampleDensity(noiseXZ, hf, weather, cloudType, weatherLarge, 0.0f, layer);
}

// サンプル位置から太陽方向への光学的深さ(C1)。**立体感の本体**。
//
// 【単位を視線側と厳密に揃える】視線側の1ステップは
//   density * Density * (stepLength / Thickness)
// という無次元量で積んでいる。太陽側も同じ Thickness を基準長として
//   Σ(density * stepLength) * Density / Thickness
// で積むことで、CloudDensity(消散係数)が視線方向と太陽方向で同じ意味を持つ。
// P13b〜P17で必要だった単位合わせの当て物(kCloudSunExtinctionScale)はこれで不要になった。
//
// 【ステップ長を指数的に伸ばす】太陽光を遮るのは手前の雲がほとんどなので、近くを細かく、
// 遠くを粗く見る。6段・初項が厚みの1/12・公比1.6で、総距離は厚みの約1.6倍になる。
//
// 【コーンオフセット】サンプルを太陽方向のまわりへ広げる(Schneider)。真っ直ぐ辿ると
// 房と房の間に硬い筋が出るが、広げることで近傍の雲も拾い、実際の雲の影らしい柔らかさになる
float CloudSunOpticalDepth(float3 samplePos, float3 sunDirection, CloudLayerParams layer)
{
    float stepLength = layer.Thickness * kCloudSunFirstStepRatio;
    float travelled = 0.0f;
    float densitySum = 0.0f;

    [unroll]
    for (int i = 0; i < kCloudSunSteps; ++i)
    {
        // 中点サンプリング。コーンの広がりは進んだ距離に比例させる
        const float3 center = samplePos + sunDirection * (travelled + stepLength * 0.5f);
        const float3 offset = kCloudSunConeOffsets[i] * (travelled * kCloudSunConeRadius);
        densitySum += CloudSampleDensityWorld(center + offset, layer) * stepLength;

        travelled += stepLength;
        stepLength *= kCloudSunStepGrowth;
    }

    return densitySum * layer.Density / layer.Thickness;
}

// 2ローブのHenyey-Greenstein位相関数(C1)。等方散乱を1とした相対値で返す。
// 【なぜ等方基準へ直すか】素の位相関数は立体角で積分すると1になるよう正規化されており、
// 値は1/(4π)≒0.08のオーダーになる。等方散乱を1とした相対値へ直してから重みを掛けないと、
// 単散乱の寄与が多重散乱の項に対して2桁小さくなり、太陽側の縁が光る効果が見えなくなる
float CloudHenyeyGreenstein(float cosAngle, float g)
{
    const float g2 = g * g;
    const float denom = pow(max(1.0f + g2 - 2.0f * g * cosAngle, 1e-4f), 1.5f);
    // (1-g²)/(4π·denom) が素の値。4πを掛けて等方基準へ直す
    return (1.0f - g2) / denom;
}

float CloudDualLobePhase(float cosAngle, float forwardG)
{
    return lerp(
        CloudHenyeyGreenstein(cosAngle, forwardG),
        CloudHenyeyGreenstein(cosAngle, kCloudPhaseBackwardG),
        kCloudPhaseBackwardWeight);
}

// ボリューム経路の1サンプルの散乱光(C1)。多重散乱をオクターブ和で近似する。
//
// 【平面経路(巻雲)の CloudInScatter とは別関数にしてある】判断3。巻雲は式も値も
// C1前と1命令も変わらないため、「積雲の被覆率0・巻雲のみならC1着手前と厳密一致」が担保される。
//
//   sunOpticalDepth … 上の CloudSunOpticalDepth の戻り値(無次元)
//   sampleDensity   … このサンプルの密度(powderに使う)。
//                     【1ステップの光学的深さではなく密度を使う理由】光学的深さはステップ長に
//                     比例するため、ステップ数を変えるとpowderの効き方まで変わってしまう。
//                     密度はサンプリングの粗さに依らない量なので、C2でステップ長を可変にしても
//                     見た目が動かない
float3 CloudInScatterVolumetric(
    float sunOpticalDepth, float sampleDensity, float cosAngle, CloudLayerParams layer,
    float sunIlluminance, float zenithLuminance)
{
    // --- 多重散乱のオクターブ和 ---
    // k段目は「消散が aᵏ 倍、寄与が bᵏ 倍、位相の異方性が cᵏ 倍」の単散乱とみなす。
    // 厚い芯ほど高次が支配的になり、明るく・向きが鈍くなる
    // --- 粉末効果(powder) ---
    // 薄い縁は散乱回数が少ないぶん実際には暗く見える。ただし**逆光側では逆に明るく光る**ので、
    // 太陽が視線の向こう側にあるとき(cosAngleが1に近い)は効かせない
    const float powder = 1.0f - exp(-sampleDensity * kCloudPowderScale);
    const float powderWeight = saturate(0.5f - 0.5f * cosAngle); // 順光で1、逆光で0
    const float powderTerm = lerp(1.0f, powder, powderWeight);

    float attenuation = 1.0f;   // aᵏ
    float contribution = 1.0f;  // bᵏ
    float eccentricity = 1.0f;  // cᵏ
    float scatter = 0.0f;
    [unroll]
    for (int k = 0; k < kCloudMsOctaves; ++k)
    {
        const float sunTransmittance = exp(-sunOpticalDepth * attenuation);
        const float phase = CloudDualLobePhase(cosAngle, layer.ForwardG * eccentricity);

        // 【powderは低次のオクターブにだけ掛ける】powderは「境界付近では多重散乱が
        // 溜まっていない」ことを表す近似なので、多重散乱そのものを表す高次の項へ掛けるのは
        // 二重計上になる。実測でも全体へ掛けると雲が一様に暗くなり、
        // 雲の中の明暗の幅(四分位範囲)が写真の21.8に対して10前後まで潰れていた。
        // 0次(単散乱)には全量、最高次には掛けない、という線形の重みにする
        const float octaveFade = float(k) / float(max(kCloudMsOctaves - 1, 1));
        scatter += contribution * sunTransmittance * phase * lerp(powderTerm, 1.0f, octaveFade);

        attenuation *= kCloudMsExtinctionFalloff;
        contribution *= kCloudMsContribution;
        eccentricity *= kCloudMsEccentricityFalloff;
    }

    // 日陰側(自己影で太陽光が届かない雲底)は空の光だけで照らされる。この項だけは
    // 天頂輝度基準で残す(kCloudSkyAmbientTermのコメント参照)。1/πはランバート面の輝度換算
    return layer.Albedo
        * (scatter * sunIlluminance / kCloudPI + kCloudSkyAmbientTerm * zenithLuminance);
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
    // 【P17でViewerHeightを外した】以前は「視点のワールドY」を持ち、雲底までの霞を
    // 1回だけ評価するのに使っていた。P17でEvaluateCloudLayerがレイの起点(rayOrigin)を
    // 引数で受け取り、霞をサンプルごとに評価するようになったため不要になった
};

CloudFogParams MakeCloudFogParams(SkyParameters params)
{
    CloudFogParams fog;
    fog.Enabled = params.FogEnabled;
    fog.Sigma0 = params.FogSigma0;
    fog.ScaleHeight = params.FogScaleHeight;
    fog.RefHeight = params.FogRefHeight;
    return fog;
}

void EvaluateCloudLayer(
    float3 rayOrigin, float3 dir, float maxDistance, float jitter,
    CloudLayerParams layer, float3 sunDirection, float zenithLuminance,
    float sunToSkyIlluminanceRatio, float skyIlluminanceOverZenith, CloudFogParams fog,
    out float transmittance, out float3 scatteredLight, out float fogInFront)
{
    // 層に当たらないレイが返す中立元。SkyColorの合成式 clearColor * T + S へ入れると
    // clearColor そのものになる(=この層は視線に何の影響も与えない)
    transmittance = 1.0f;
    scatteredLight = float3(0.0f, 0.0f, 0.0f);
    // 【視点からこの層の雲に当たるまでの霞の透過率(P18b)】1.0は「手前に霞が無い」の意味で、
    // 層に当たらなかった場合・霞が無効な場合の中立元でもある。呼び出し側(SkyColorWithRay)が
    // 「雲の手前の霞そのものの色」を作るために使う。詳しくはSkyColorWithRayの該当箇所
    fogInFront = 1.0f;

    // ================= (a) レイと層の交差(P17) =================
    // 層は**世界座標**のスラブ [Altitude, Altitude + Thickness]。厚みゼロの層(巻雲)だけは
    // 平面 y = Altitude として別に解く。
    //
    // 【なぜこの形にしたか】P17より前は「カメラの真上 Altitude[m] にある無限平面」を仮定し、
    // 交点を dir.xz * (Altitude / dir.y) で求めていた。層がカメラのYにもXZにも追従するため、
    //   - 上空へ飛んでも雲の上に出られない(高度差が常に一定)
    //   - 水平に動いても雲が頭上を通り過ぎない(模様がカメラに追従する)
    //   - 見下ろすと雲が消える(dir.y <= 0 で早期脱出していた)
    //   - 水面に雲が映らない(反射レイはほぼ水平で、1/dir.y を隠すためのフェード領域へ丸ごと入る)
    // という4つがすべてこの1点から派生していた。スラブ交差なら視点が層の下・中・上の
    // どこにあっても、見上げても見下ろしても、同じ1本の式で解ける
    const float slabBase = layer.Altitude;
    const float slabTop = layer.Altitude + max(layer.Thickness, 0.0f);

    float tEnter;
    float tExit;
    if (layer.Thickness <= 0.0f)
    {
        // 厚みゼロの平面(巻雲)。1点で交わる。ほぼ平行なレイは交わらないものとして扱う
        if (abs(dir.y) < kCloudParallelDirY) { return; }
        tEnter = (slabBase - rayOrigin.y) / dir.y;
        // レイの後ろ側(t<0)、またはレイの届く範囲より先なら当たらない
        if (tEnter < 0.0f || tEnter > maxDistance) { return; }
        tExit = tEnter;
    }
    else if (abs(dir.y) < kCloudParallelDirY)
    {
        // レイが層とほぼ平行。t = (境界 - 起点.y)/dir.y が桁あふれするため別扱いにする。
        // 層の中にいるなら最後まで層の中、外にいるなら永遠に当たらない
        if (rayOrigin.y < slabBase || rayOrigin.y > slabTop) { return; }
        tEnter = 0.0f;
        tExit = maxDistance;
    }
    else
    {
        // スラブ交差。上下どちらの境界に先に当たるかはdir.yの符号で決まるのでmin/maxで揃える。
        // tEnterを0で下限クランプするのは、視点が層の中や上にある場合に
        // 「レイの後ろ側の交点」を拾わないため
        const float t0 = (slabBase - rayOrigin.y) / dir.y;
        const float t1 = (slabTop - rayOrigin.y) / dir.y;
        tEnter = max(min(t0, t1), 0.0f);
        tExit = min(max(t0, t1), maxDistance);
        if (tExit <= tEnter) { return; }
    }

    // マーチする距離の上限。霞が有効な限り下の打ち切りが先に効くので一度も掛からない
    // (kCloudMaxSpanMetersのコメント参照)。霞を切ったときだけ効く安全弁
    tExit = min(tExit, tEnter + kCloudMaxSpanMeters);

    // ================= (b) 層の入口までの霞(P17の判断3) =================
    // 入口で既に見えないなら、この層は丸ごと省ける
    const float3 enterPos = rayOrigin + dir * tEnter;
    float enterFog = 1.0f;
    if (fog.Enabled > 0.5f)
    {
        enterFog =
            HeightFogTransmittance(rayOrigin, enterPos, fog.Sigma0, fog.ScaleHeight, fog.RefHeight);
        if (enterFog < kCloudFogCutoffTransmittance) { return; }
    }

    // 自己影のマーチと平面経路が使うノイズ空間の位置。**層への入口**を基準にする。
    // 視点が層より下にある通常の構図では、これは雲底平面との交点そのものであり、
    // P17より前の hitXZ と厳密に一致する(書き直しの等価性の根拠の1つ)
    const float2 anchorUv = enterPos.xz * layer.UvScale + layer.ScrollOffset;

    // 【C1で自己影のXZ2D積分を撤去した】ここには「雲底のUVから太陽方向へXZ平面上だけを
    // 5歩たどる」ブロックがあった。雲の縦構造も太陽の仰角も反映せず、3Dノイズを1回も引かない
    // 近似で、実測でも雲の明るさへの寄与は +1.2 しかなかった。ボリューム経路はマーチの
    // サンプルごとに CloudSunOpticalDepth(3Dの太陽マーチ)を呼ぶ形へ移した。
    // **平面経路(巻雲)は自己影を持たない**(C1前も kCirrusShadowSteps=0 で常に1.0だった)ので、
    // 下の平面分岐は sunTransmittance=1.0 のままで C1 前と厳密に一致する(判断3)
    const float sunTransmittance = 1.0f;

    // 視線と太陽のなす角。両経路が使う(ボリューム経路は CloudInScatterVolumetric へ渡し、
    // 平面経路は下で単一ローブの位相関数を組み立てる)
    const float cosAngle = dot(dir, sunDirection);

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
        // --- ボリューム(P13b): 層に入ってから出るまでを前から後ろへ積分する ---
        // (P17でレイに沿った等間隔マーチに、C2で1歩の長さがワールド空間の量になった)
        //
        // 【C2】1歩の長さを「経路長 ÷ 上限ステップ数」と「下限12m」の大きいほうにする。
        // 厚みを変えても1歩が暴れず、経路が長いとき(地平線際)だけ1歩が伸びてコストが有界になる
        const float span = tExit - tEnter;
        const float stepLength = max(span / float(kCloudMaxRaymarchSteps), kCloudMinStepMeters);

        // 【1ステップの光学的深さを stepLength/Thickness で測る理由】(P17)
        // スラブを完全に貫くレイでは全ステップの和が
        //   density * Density * (経路長 / Thickness) = density * Density / |dir.y|
        // となり、**P17より前の 1/dir.y の式と厳密に一致する**。したがって
        // CloudDensity(消散係数)の意味も判断B(被覆率→平均透過率、
        // kCloudOvercastTransmittance)も再調整せずに済む。
        // 貫かない場合(層の中から始まる・地物で打ち切られる)は、実際に通った長さのぶんだけ
        // 薄くなる——これがそのまま正しい振る舞いになる
        const float stepDepthScale = stepLength / layer.Thickness;

        // 【霞は1歩ごとに掛けず、雲に入った位置の霞を最後に1回だけ掛ける】
        // 以前は毎歩 lerp(1, stepTransmittance, sampleFog) を掛けていたが、これは
        // **カメラからその点までの同じ霞を雲の奥行きぶん複利で効かせる**式になっていた。
        // そのため打ち切りの条件しだいで絵が両極端に振れた:
        //   cloudTransmittance で打ち切る … 複利が数歩で止まり返り値が高いまま残る。
        //     消散係数を上げると「不透明な雲なのに背後の青空が2割見える」
        //   返り値で打ち切る … 複利が最後まで効く。被覆率1.0の水平線際が真っ黒に近づく
        //     (実測: 出荷カメラの仰角0〜4度の輝度中央値が 219.3 → 148.0)
        // どちらも同じ式の裏表なので、複利そのものを断つ。雲が層を遮る割合は
        // cloudTransmittance で、霞はその遮りがどれだけ見えるかを決めるだけ、と分ける
        float cloudTransmittance = 1.0f;
        // 雲に最初に当たった位置の霞。遠方の雲ほど小さくなり、層が背後の空を遮らなくなる
        float fogAtCloud = 1.0f;
        bool hasCloud = false;
        float3 accumScatter = float3(0.0f, 0.0f, 0.0f);

        // 【C2: 開始位置を画素ごとにずらす】1歩ぶん未満のずれを入れることで、
        // 全画素で揃っていたステップの切れ目(=スライスの縞)が高周波のディザへ変わる。
        // jitterは[0,1)なので、ずれは常に1歩の内側に収まり、経路の総和は変わらない
        const float jitterOffset = jitter * stepLength;

        [loop]
        for (int marchStep = 0; marchStep < kCloudMaxRaymarchSteps; ++marchStep)
        {
            // レイに沿った中点サンプリング。**サンプル位置がワールド座標で決まるのがP17の要**で、
            // 高さとともに横へずれるのが視差の源になる(仰角45度・厚み1,000mなら雲頂は雲底より
            // 1,000m=ウェザーマップの1セルぶん横へ動く)
            const float t = tEnter + jitterOffset + (float(marchStep) + 0.5f) * stepLength;
            // 1歩が下限12mで頭打ちになっている場合、上限ステップ数より手前で層を抜ける
            if (t > tExit)
            {
                break;
            }
            const float3 samplePos = rayOrigin + dir * t;
            // スラブ内の高さ(0=雲底、1=雲頂)
            const float hf = saturate((samplePos.y - slabBase) / layer.Thickness);

            // 【霞をサンプルごとに評価する】(P17の判断3) P17より前は「雲底までの透過率」を
            // 最後にまとめて掛けていたが、見下ろす視点では Altitude/dir.y が負になり破綻した。
            // サンプルのワールド座標は既に求まっているので追加コストはexp1回ぶん。
            // 累積した霞が閾値を下回ったら、以降のサンプルは絵に出ないので打ち切る——
            // これがP17より前の地平線フェードの置き換えになる
            float sampleFog = 1.0f;
            if (fog.Enabled > 0.5f)
            {
                sampleFog = HeightFogTransmittance(
                    rayOrigin, samplePos, fog.Sigma0, fog.ScaleHeight, fog.RefHeight);
                if (sampleFog < kCloudFogCutoffTransmittance)
                {
                    break;
                }
            }

            const float2 sampleNoiseXZ = samplePos.xz * layer.UvScale + layer.ScrollOffset;

            // ウェザーマップ。ここが0なら3Dテクスチャを1枚も引かずに次のステップへ飛ぶ。
            // 被覆率は場所ごとに雲の種類で上下する(C7。大きさをまばらにする)
            float cloudType;
            float weatherLarge;
            const float weather =
                CloudWeatherAt(sampleNoiseXZ, layer, cloudType, weatherLarge);
            if (weather <= 0.0f)
            {
                continue;
            }

            const float sampleDensity =
                CloudSampleDensity(sampleNoiseXZ, hf, weather, cloudType, weatherLarge,
                                   1.0f, layer);
            if (sampleDensity <= 0.0f)
            {
                continue;
            }

            const float stepOpticalDepth = sampleDensity * layer.Density * stepDepthScale;
            const float stepTransmittance = exp(-stepOpticalDepth);
            if (!hasCloud)
            {
                fogAtCloud = sampleFog;
                hasCloud = true;
            }

            // 【C1: ここが立体感の本体】このサンプルから太陽方向へ実際に3Dマーチして
            // 光学的深さを求め、多重散乱のオクターブ和で散乱光にする。
            // C1より前はここが「XZ平面上の2D積分 × 解析的な縦勾配」という2つの近似の積で、
            // 実測の寄与は合わせて +4.8 しかなかった(雲と空の差54に対して)
            const float sunOpticalDepth = CloudSunOpticalDepth(samplePos, sunDirection, layer);
            const float3 stepInScatter = CloudInScatterVolumetric(
                sunOpticalDepth, sampleDensity, cosAngle, layer, sunIlluminance, zenithLuminance);

            // 前から後ろへの合成。手前の雲で既に遮られたぶん(cloudTransmittance)だけ寄与し、
            // さらに視点までの霞(sampleFog)で減る
            accumScatter += cloudTransmittance * (1.0f - stepTransmittance) * stepInScatter * sampleFog;
            cloudTransmittance *= stepTransmittance;

            // ほぼ不透明になったら以降のステップは絵に出ない。
            // 霞を最後に1回だけ掛ける形にしたので、ここで見る量と返り値が同じ意味になり、
            // 早い打ち切りでも返り値が高いまま残る不整合が構造的に起きない
            if (cloudTransmittance < 0.01f)
            {
                break;
            }
        }

        // 霞で薄まったぶん、雲は背後の空を遮らなくなる。霞が無ければ cloudTransmittance と一致する
        transmittance = lerp(1.0f, cloudTransmittance, fogAtCloud);
        scatteredLight = accumScatter;
        // 雲に当たったときだけ、その位置までの霞を呼び出し側へ渡す(P18b)。
        // 当たらなかった場合は初期値の1.0のまま=手前に霞が無い扱いになる
        if (hasCloud)
        {
            fogInFront = fogAtCloud;
        }
    }
    else
#endif
    {
        // --- 平面(P5〜P12と同一。巻雲がここを通る) ---
        // (c) 雲の密度。fBmの出力を被覆率で塊に整形する。
        // AnisotropicScaleはここでUVへ掛ける(積雲は(1,1)なので無変化、巻雲はU方向だけ伸びて筋状になる)
        const float n = CloudFbm(anchorUv * layer.AnisotropicScale);

        // 【D2でボリューム側を直した欠陥が、こちらに残っていた】以前は
        //     density = saturate(remap(n, 1 - Coverage, 1.0))
        // と**上端を1.0に固定**していた。CloudFbmの実測分布は平均0.500・標準偏差0.132、
        // 99.9%点でも0.839で、1.0へは決して届かない。そのため
        //  ・被覆率1でも密度が0.68止まりで「全天が巻雲」にならない
        //  ・被覆率の効きが崖になる。しきい値 1-Coverage が分布の中央(0.5)を跨ぐ前後で
        //    急に空が埋まる。実測でも被覆率0.3→0.5で空の埋まり具合が12.9%→19.9%と跳ねた
        // 定数の根拠は kCloudWeatherLow のコメント(同じ CloudFbm を測ったもの)。
        // 異方スケールでUVを伸ばしても値の分布は変わらないので、そのまま使える。
        //
        // 【Coverage=0がここへ来ないこと】判断C(被覆率0で雲を追加する前と画素まで一致)は
        // SkyColor側の早期脱出が担保する。この式自体は Coverage=0 でしきい値が99%点になるだけで
        // 0にはならないが、そこへは到達しない
        const float threshold = lerp(kCloudWeatherHigh, kCloudWeatherLow, layer.Coverage);
        const float density = saturate(CloudRemap(n, threshold, kCloudWeatherMax));

        // (d) 光路長。厚みゼロのシートを浅い角度で貫くほど経路が伸びるため 1/|dir.y| に
        // 比例させる。**この発散はモデルに内在するもの**(厚みゼロの面には交差の解が1点しか
        // 無く、そこを何メートル通ったかという情報が無い)なので、kCloudMinDirYのクランプは
        // P17後もここに残る。P17より前と違うのは絶対値を取る点だけで、これは見下ろす視線でも
        // 経路長が正になるようにするため
        const float pathLengthScale = 1.0f / max(abs(dir.y), kCloudMinDirY);

        // Henyey-Greenstein位相関数(単一ローブ)。cosAngle=1(dirが太陽方向と一致=太陽を直視する
        // 向き)で前方散乱が最大になり、半逆光で雲の縁が光る効果が出る(layer.ForwardGが強さ)。
        // 【C1でこの分岐の中へ移した】ボリューム経路が2ローブの位相関数へ移り、この式を
        // 使わなくなったため。**式そのものはC1前と1文字も変えていない**(判断3)。
        // 位相関数は立体角で積分すると1になるよう正規化されているため素の値は1/(4π)≒0.08の
        // オーダーになる。等方散乱を1とした相対値へ直してから重みを掛けないと、単散乱の寄与が
        // 多重散乱の下限項に対して2桁小さくなり、太陽側の縁が光る効果が見えなくなる
        const float g = layer.ForwardG;
        const float g2 = g * g;
        const float phaseDenom = pow(max(1.0f + g2 - 2.0f * g * cosAngle, 1e-4f), 1.5f);
        const float phase = (1.0f - g2) / (4.0f * kCloudPI * phaseDenom);
        const float phaseNormalized = phase * 4.0f * kCloudPI;

        // ビアの法則。経路長はメートル、Density(消散係数)はCPU側UIで調整する無次元の強さ
        const float planeTransmittance = exp(-density * layer.Density * pathLengthScale);

        // 視点から平面までの霞(P17)。単一サンプルなので入口の値がそのまま使える。
        // 【この形はP17より前と厳密に同じ】以前は fade = 霞の透過率 として
        //   transmittance = lerp(1, T, fade) / scatteredLight = S * fade
        // を最後に適用していた。ここではそれを展開して書いているだけで、
        // 巻雲の描画結果は(地平線フェードを外した点を除いて)変わらない
        transmittance = lerp(1.0f, planeTransmittance, enterFog);
        // (1-planeTransmittance)は視線の経路のうち実際に散乱へ回った分のスケール
        scatteredLight =
            CloudInScatter(sunTransmittance, phaseNormalized, layer, sunIlluminance, zenithLuminance)
            * (1.0f - planeTransmittance) * enterFog;
        // 平面経路は交差の早期脱出を通り抜けた時点で必ず当たっているので、入口の霞をそのまま渡す(P18b)
        fogInFront = enterFog;
    }

    // 【(e)の地平線フェードと(f)の一括フォグはP17で無くなった】
    //
    // (e) 地平線際のフェード(仰角11.5度以下で雲を消す)は 1/dir.y の発散を隠すための
    // 対症療法で、同時に水面への雲の映り込みを殺していた。(a)でレイと層の交差を正しく
    // 解くようになったので発散せず、フェードそのものが不要になった。
    //
    // (f) 雲へ掛ける大気遠近(P12)は、「雲底までの透過率を最後にまとめて掛ける」という
    // 割り切りだった。見下ろす視点では雲底までの距離 Altitude/dir.y が負になって破綻するため、
    // P17でマーチのサンプルごとの評価((b)と上のループ内)へ移した。**なぜ要るかという理由**は
    // P12から変わらない——雲は深度を持たない背景として描かれAerialPerspective.hlslの
    // 早期脱出(depth <= 0)に入るため、ここで掛けないと「地物は溶けたのに雲だけ剃刀のように
    // くっきり」という絵になる。
    //
    // 減った分を埋める大気光(airlight)の等価輝度が、この関数の呼び出し元(SkyColor)の
    // clearColor(晴天の空色)そのものである、という構造もP12から変わらない。霞の透過率f=0を
    // 代入すると透過率1・散乱光0になり、合成式 clearColor * T + S が clearColor に一致する
    // ——「霞で雲が完全に消えたら素の空色が見える」という正しい極限になる
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

// 雲の無い素の空の色。地平線より上はSkyColorUpper、下はプラトー色から接地色へのフェード。
// 【P17でSkyColorから括り出した】雲の合成を「地平線より上のif」の中から外へ出すため
// (見下ろす視線にも雲が掛かるようにする)、基色を決める部分を独立させた。中身も呼び出し順も
// P17より前と同じなので、雲が無い画素の値は1ビットも変わらない
float3 SkyClearColor(float3 dir, SkyParameters params)
{
    if (dir.y >= kGroundFadeStartY)
    {
        return SkyColorUpper(dir, params);
    }

    // 水平線より下: プラトー色(kGroundFadeStartYの高さへ射影した方向の空色)から接地色へフェード
    float3 plateauDir = dir;
    plateauDir.y = kGroundFadeStartY;
    plateauDir = normalize(plateauDir);
    const float3 plateauColor = SkyColorUpper(plateauDir, params);

    const float3 groundColor = params.ZenithLuminance * params.GroundTint;
    const float groundT = saturate((dir.y - kGroundFadeStartY) / (kGroundFadeEndY - kGroundFadeStartY));
    return lerp(plateauColor, groundColor, groundT);
}

// 雲の手前にある霞の色を、晴天の空色から曇天の空色へ直す補正項(P18b)。
//
// 【何が起きていたか】雲層の合成は clearColor * T + S で、TはEvaluateCloudLayerが
//   T = lerp(1, T_cloud, fog)
// として返す。これを展開すると
//   clearColor * fog * T_cloud   ← 雲の隙間から見える空(正しい)
// + clearColor * (1 - fog)       ← **雲の手前の霞そのもの**
// + S
// となり、2項目の霞が晴天の空色で塗られていた。曇天では霞は無彩色に近くなるはずなので、
// 被覆率を上げるほど「灰色であるべき雲に青が差す」という形で出る。
// 実測(被覆率1.0・出荷カメラ): 霞を運用値5e-5から1e-7へ実質切ると、
// 空の帯のB-Rの中央値が 32.0 → 2.0(中の帯)・6.0 → 0.0(上の帯)まで落ちた。
// **霞を完全に切った対照で症状が消える**ので、原因は霞の色で確定している。
//
// 【直し方】正しい霞の色は「その空間を照らしている光」であり、それは大気遠近の
// in-scatterとまったく同じ量である(AerialPerspective.hlslを参照)。すなわち
// clearColor * CloudSkyLight。差分だけを足す形にすると
//   clearColor * (CloudSkyLight - 1) * (1 - fog)
// になる。CloudSkyLight = 1(被覆率0、または雲の減光が無い)のとき厳密に0なので、
// 雲を持たないシーンでは1命令も効かない。
//
// 【なぜEvaluateCloudLayerの中でやらないか】層ごとに足すと、2層とも当たったときに
// 視点から手前の層までの区間を二重に計上する。呼び出し側で1回だけ足せばその心配が無い
float3 CloudAirlightCorrection(float3 clearColor, float fogInFront, SkyParameters params)
{
    return clearColor * (params.CloudSkyLight - 1.0f) * (1.0f - fogInFront);
}

// 起点と長さを持つ1本のレイに沿って空と雲を評価する(P17)。
//   rayOrigin   … レイの起点(ワールド)。背景画素ならカメラ位置、水面の反射なら水面の位置
//   dir         … 正規化済みの向き
//   maxDistance … レイの長さ[m]。地物に遮られる場合はそこまでの距離、背景なら
//                 kCloudBackgroundRayDistance
//
// 【なぜ起点が要るのか】P17より前のSkyColor(dir, params)は方向しか受け取らず、雲層を
// 「カメラの真上にある無限平面」として扱っていた。そのため水面の反射レイも起点がカメラ扱いに
// なり、**水面に雲が映らない直接の原因**になっていた(EvaluateCloudLayerの(a)節に4つの症状を
// まとめてある)
float3 SkyColorWithRay(float3 rayOrigin, float3 dir, float maxDistance, SkyParameters params)
{
    float3 clearColor = SkyClearColor(dir, params);

    // 星は雲より奥にあるので、雲で減光される前のここで足す。
    // StarsIntensity=0(昼・無効)のときEvaluateStarfieldは即座に0を返し、
    // この加算自体も分岐で飛ばすので、星を持たない場合と画素まで一致する。
    // 水平線より下はEvaluateStarfield側のhorizonFadeが0になるため、ここで別扱いしなくてよい
    if (params.StarsIntensity > 0.0f)
    {
        clearColor += EvaluateStarfield(dir, params);
    }

    // (h) 早期脱出。積雲・巻雲どちらの被覆率も0なら雲の計算を一切行わずclearColorをそのまま返す。
    // 判断C(被覆率0のときP4完了時点=雲を追加する前と画素まで一致すること)の担保の1つめはここ
    // ——雲側の計算(EvaluateCloudLayer)は一度も呼ばれず、返す値もSkyClearColorの結果そのまま
    // なので数値は変わりようがない。
    // 【P17で dir.y <= 0 の早期脱出を外した】地平線より下を見る視線にも雲を評価させるため
    // (上空から雲を見下ろせるようにするのが目的)。地上のカメラが見下ろす通常の構図では、
    // 雲層は視点より上にあるのでスラブ交差が空になり、雲は掛からないまま——
    // つまり結果は変わらず、変わるのは「視点が雲より上にあるとき」だけになる
    if (params.CloudCoverage <= 0.0f && params.CirrusCoverage <= 0.0f)
    {
        return clearColor;
    }

    // 雲へ掛ける大気遠近(P12)。層に依らない値なのでここで1回だけ組み立て、両層へ渡す。
    // 【上の早期脱出より後に置く】判断Cの「雲が無いときは掛け算・足し算を1つも増やさない」に
    // 揃えるため。被覆率0の画素はここへ到達せず、この組み立て自体が行われない
    const CloudFogParams fog = MakeCloudFogParams(params);

    // 積雲(下層、P5)。被覆率0でもここへ来る場合があるため(巻雲だけの空)、個別に早期脱出する。
    // transmittance=1.0/scatteredLight=0の初期値は「雲が無い」ことを表す中立元(下のclearColor*1+0と
    // 一致する値)であり、CloudCoverage<=0のときEvaluateCloudLayerを呼ばずこの初期値のまま使う
    float cumulusTransmittance = 1.0f;
    float3 cumulusScatter = float3(0.0f, 0.0f, 0.0f);
    float cumulusFogInFront = 1.0f;
    if (params.CloudCoverage > 0.0f)
    {
        EvaluateCloudLayer(
            rayOrigin, dir, maxDistance, params.RaymarchJitter, MakeCumulusLayerParams(params), params.SunDirection,
            params.ZenithLuminance, params.SunToSkyIlluminanceRatio, params.SkyIlluminanceOverZenith,
            fog, cumulusTransmittance, cumulusScatter, cumulusFogInFront);
    }

    // 【判断Cの担保の2つめ】巻雲の被覆率が0のとき、EvaluateCloudLayer(巻雲側)を一度も呼ばず、
    // 積雲だけだったP11着手時点(HEAD)と完全に同一の式(clearColor * T_cumulus + S_cumulus)を
    // そのまま通す。掛け算・足し算を1つも増やさないことで、浮動小数の最下位ビットまで一致させる
    if (params.CirrusCoverage <= 0.0f)
    {
        return clearColor * cumulusTransmittance + cumulusScatter
             + CloudAirlightCorrection(clearColor, cumulusFogInFront, params);
    }

    // 巻雲(上層、P11)を評価する。巻雲は積雲より高い位置にあるため、巻雲から届く散乱光は
    // 手前(視点側)にある積雲でさらに減光される——これを表すのが下のcumulusTransmittanceを
    // 掛ける項。掛けないと積雲に隠れるはずの巻雲が透けて見えてしまう。
    // 【視点が積雲と巻雲の間にある場合】積雲は視線に掛からずスラブ交差が空になるため
    // cumulusTransmittance=1となり、この項は自動的に無効になる(場合分けは要らない)
    float cirrusTransmittance;
    float3 cirrusScatter;
    float cirrusFogInFront;
    EvaluateCloudLayer(
        rayOrigin, dir, maxDistance, params.RaymarchJitter, MakeCirrusLayerParams(params), params.SunDirection,
        params.ZenithLuminance, params.SunToSkyIlluminanceRatio, params.SkyIlluminanceOverZenith,
        fog, cirrusTransmittance, cirrusScatter, cirrusFogInFront);

    // (g) 2層合成: 高い層(巻雲)から手前(積雲)へ。
    //   透過率 = T_cirrus * T_cumulus (両層を貫く視線の透過率なので積)
    //   散乱光 = S_cumulus + S_cirrus * T_cumulus (巻雲の光は積雲を透過して初めて届く)
    // lerpではなくこの形にするのは、雲の隙間からのぞく青空をそのまま残すため
    // (lerpだと被覆率で単純に混ぜてしまい、隙間の青空まで雲色へ寄ってしまう)
    const float transmittance = cirrusTransmittance * cumulusTransmittance;
    const float3 scatteredLight = cumulusScatter + cirrusScatter * cumulusTransmittance;
    // 【霞の色は手前の層のものを使う】(P18b) 積雲は巻雲より低く、視線に当たっていれば必ず手前にある。
    // 当たっていない(cumulusFogInFront == 1.0)ときだけ巻雲側の霞を見る。
    // 霞が無効なときは両方1.0になり、補正は0になる
    const float fogInFront =
        (cumulusFogInFront < 1.0f) ? cumulusFogInFront : cirrusFogInFront;
    return clearColor * transmittance + scatteredLight
         + CloudAirlightCorrection(clearColor, fogInFront, params);
}

// 背景(地物に遮られない視線)用の薄いラッパー。起点は視点、レイ長は実質無限。
// 呼び出し側(DeferredLighting.hlsl / SkyGenerate.hlsl)の記述をP17より前のまま保つためにある
float3 SkyColor(float3 dir, SkyParameters params)
{
    return SkyColorWithRay(params.ViewerPosition, dir, kCloudBackgroundRayDistance, params);
}

#endif // KURENAI_SKY_HLSLI
