// MegaLights のデノイザ。時間累積 + エッジ停止付き à-trous(SVGF; Schied 2017 の構成)。
//
// 【何を落とすのか】確率的サンプリングは1画素1本の影レイで推定するので、
// 半影や遮蔽の縁では可視率が0か1のどちらかしか引けず、1枚の絵では強いノイズになる。
// 時空間再利用(段階3・4)は**リザーバ**を混ぜて実効サンプル数を増やすが、
// それでも残る裾をここで落とす。**両者は別物で、混同しないこと** ――
// あちらは「どの灯を選ぶか」を改善し、こちらは「出た色」を空間・時間へならす。
//
// 【TAAと二重に掛けない】このエンジンは後段にTAAを持つ。TAAはYCoCgの近傍分散でクリップ
// するので、ノイズを「正当な信号の広がり」と解釈して履歴を毎フレーム棄却する ――
// ノイズもAAも両方失う。だからノイズはTAAへ渡す前にここで落とす。
// 逆にここで長く累積しすぎるとTAAのゴーストと重なって二重に尾を引くので、
// 時間累積の上限は手法ごとに持つ(既定は手法2で32・手法3で64。EngineDefaults.h)。
// さらに残差が大きい画素だけ上限を短く落とすアンチラグを持つ(AntiLagCap。既定は無効)。
//
// 【アルベド復調】フィルタの前に「その画素の反射率」で割り、後で掛け戻す。
// 割らずにぼかすと、明るい面と暗い面の境界で色が滲む(テクスチャの模様が影へ漏れる)。
// 係数はG-Bufferから両側で同一に再計算するので保存は要らない。
// **拡散と鏡面を1本で復調するのは近似**で、強い鏡面ハイライトでは復調しきれない。
// 分けるには出力を2枚に増やす必要があるため、まず1本で出して残る破綻を見てから判断する。
//
// レイを撃たないので3バリアントすべてでコンパイルされる。
#include "NormalEncoding.hlsli"
#include "Samplers.hlsli"
// 前フレームの幾何(MegaLightsHistoryGuide)。時間再利用が毎フレーム書いているものを
// 履歴の妥当性判定に使う。リソースは宣言していないヘッダなので取り込んでも束縛は増えない
#include "MegaLightsCommon.hlsli"

#include "MathConstants.hlsli"

#include "ShaderInterop/FrameConstants.hlsli"

cbuffer MegaLightsDenoiseConstants : register(b1)
{
    // x=出力幅, y=出力高, z=履歴が使えるか(0なら履歴を読まない), w=à-trousの段(0起点)
    uint4 Params0;
    // x=à-trousのステップ幅(1,2,4,8...), y=時間累積の上限フレーム数,
    // z=輝度のエッジ停止の強さ, w=法線のエッジ停止の指数
    float4 Params1;
    // x=深度のエッジ停止の強さ, y=ファイアフライのクランプ強さ(0で無効),
    // z=前フレームの幾何(履歴ガイド)が使えるか(0なら現フレームのG-Bufferで代用),
    // w=履歴の色の再サンプリング(0=バイリニア / 1=Catmull-Rom)
    float4 Params2;
    // x=履歴の妥当性の判定タップ数(0=最近傍1タップ(従来) / 1=バイリニア2x2の4タップ),
    // y=アンチラグの相対変化の smoothstep 下端 t0, z=同 上端 t1,
    // w=アンチラグの短い EMA の長さ[フレーム](0で無効)
    float4 Params3;
};

// 前フレームの幾何。時間再利用(MegaLightsTemporal)が毎フレーム全画素へ書いている。
// 【使わないフレームでも必ずバインドする】DX12は宣言したリソースが未バインドだと壊れる
StructuredBuffer<MegaLightsHistoryGuide> HistoryGuide : register(t0);

Texture2D NormalTexture : register(t1);
Texture2D DepthTexture : register(t2);
Texture2D AlbedoTexture : register(t3);
Texture2D MaterialTexture : register(t4);
// G-Bufferパスが書いたモーションベクター(UV単位)。TAAと同じものを同じ引き方で使う
Texture2D VelocityTexture : register(t5);
// 入力。時間累積では MegaLights の生出力、à-trous では前段の出力
Texture2D InputTexture : register(t6);
// 前フレームの累積結果(rgb=復調済みの色, a=これまでに累積したフレーム数)
Texture2D HistoryTexture : register(t7);
// モーメント(x=輝度の1次, y=2次, z=履歴の長さ, w=輝度の分散)。
// 【.w の分散は à-trous が段ごとにフィルタして次段へ渡す】時間累積が最初の値を作り、
// 各段が重みの二乗で畳んで書き戻す(本家SVGFの構成)
Texture2D HistoryMomentsTexture : register(t8);

RWTexture2D<float4> OutputTexture : register(u0);
RWTexture2D<float4> OutputMomentsTexture : register(u1);
// 時間累積(CSTemporalAccum)が「次フレームの履歴」として書く先。
// 【u0/u1と別に要る】u0/u1(ping)はà-trousが上書きしていくので、
// 翌フレームが読み戻す履歴は独立したバッファに残さなければならない。
// C++側は最初からここへ履歴バッファを束縛していたが、シェーダが宣言しておらず
// **時間累積が一度も履歴を書けていなかった**(historyValidの幾何判定は現フレームの
// G-Bufferを見るので通ってしまい、長さだけがゴミ値のまま毎フレーム1へ戻っていた)。
// à-trous / Remodulate では書かない(C++はダミーとして自分の出力を重ねて束縛する)
RWTexture2D<float4> HistoryOutTexture : register(u2);
RWTexture2D<float4> HistoryMomentsOutTexture : register(u3);

// 履歴を採用する条件。時空間再利用(MegaLightsTemporal/Spatial)と同じ3つを同じしきい値で。
// **深度はView空間の線形値で比べること**(Reverse-Zの生値で比べてはいけない)
static const float kMaxRelativeDepthDiff = 0.05f;
static const float kMinNormalDot = 0.9f;
static const float kMaxMaterialDiff = 0.1f;

// 復調に使う反射率の下限。0で割ると黒い面で発散する
static const float kMinDemodulation = 0.05f;

float Luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

#include "ShaderInterop/Common.hlsli"

// その画素の「反射率」。フィルタの前にこれで割り、後で掛け戻す。
// **時間累積側とà-trous側で必ず同じ式を使うこと** ―― ずれると掛け戻したときに色が変わる
float3 DemodulationFactor(float2 uv)
{
    const float3 albedo = AlbedoTexture.SampleLevel(ColorSampler, uv, 0).rgb;
    const float metallic = MaterialTexture.SampleLevel(DataSampler, uv, 0).r;
    // 金属は拡散を持たず反射色がアルベドになる。誘電体は拡散アルベド + 4%の鏡面。
    // 拡散と鏡面を分けない近似(冒頭のコメント参照)
    const float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    const float3 factor = lerp(albedo, float3(0.0f, 0.0f, 0.0f), metallic) + f0;
    return max(factor, float3(kMinDemodulation, kMinDemodulation, kMinDemodulation));
}

float TileViewZ(float2 uv, float depth)
{
    return mul(float4(ReconstructWorldPos(uv, depth), 1.0f), View).z;
}

// ---------------------------------------------------------------------------
// 段1: 時間累積。速度ベクトルで再投影し、指数移動平均で混ぜる
// ---------------------------------------------------------------------------
// 履歴の色を引く。**ここのフィルタが、移動中の鮮鋭さを決めている。**
//
// 【なぜ Catmull-Rom を選べるようにしたか】バイリニアで引くと、毎フレーム
// 「補間した結果をまた補間する」ことになり、ぼけが累積する。TAA はまさにこの理由で
// Catmull-Rom を使っており、TAA.hlsl の SampleHistoryCatmullRom にそう書いてある。
// ところがこのデノイザは SampleLevel(バイリニア)のままで、**同じリポジトリの中で
// 非対称になっていた**。
//
// 実測(BistroExteriorNight / Strafe経路 / 2560x1440 / 分母は参照実装 rays=64)。
// S = 候補自身の高周波エネルギー ÷ 真値のそれ。1未満はなまっていることを意味する:
//     累積上限 2フレーム  S=0.987     16フレーム S=0.553
//     累積上限 4フレーム  S=0.744     64フレーム S=0.516
// **累積を伸ばすほど単調に鮮鋭さが失われる。** a-trous の段数ではほとんど動かない
// (0段でも 0.516)ので、なまりを作っているのは空間フィルタではなくこの再サンプリング。
//
// 【モーメントには使わない】Catmull-Rom は負のローブを持つ。モーメントの z 成分は
// 履歴長、xy は輝度の1次・2次モーメントで、負へ振れると分散が負になったり
// 履歴長が壊れたりする。モーメントはバイリニアのままにすること
float3 SampleHistoryColorCatmullRom(float2 uv, uint2 outputSize)
{
    const float2 texelSize = 1.0f / float2(outputSize);
    const float2 samplePos = uv * float2(outputSize);
    const float2 texPos1 = floor(samplePos - 0.5f) + 0.5f;
    const float2 f = samplePos - texPos1;

    // Catmull-Rom(B=0, C=0.5)の重み。TAA.hlsl と同じ式にしてあること
    const float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    const float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    const float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    const float2 w3 = f * f * (-0.5f + 0.5f * f);

    // w1とw2を1タップのバイリニアへまとめ、16タップを5タップへ削る定番の形
    const float2 w12 = w1 + w2;
    const float2 offset12 = w2 / max(w12, 1e-5f);

    const float2 texPos0 = (texPos1 - 1.0f) * texelSize;
    const float2 texPos3 = (texPos1 + 2.0f) * texelSize;
    const float2 texPos12 = (texPos1 + offset12) * texelSize;

    float3 result = float3(0.0f, 0.0f, 0.0f);
    result += HistoryTexture.SampleLevel(ColorSampler, float2(texPos12.x, texPos0.y), 0).rgb * w12.x * w0.y;
    result += HistoryTexture.SampleLevel(ColorSampler, float2(texPos0.x, texPos12.y), 0).rgb * w0.x * w12.y;
    result += HistoryTexture.SampleLevel(ColorSampler, float2(texPos12.x, texPos12.y), 0).rgb * w12.x * w12.y;
    result += HistoryTexture.SampleLevel(ColorSampler, float2(texPos3.x, texPos12.y), 0).rgb * w3.x * w12.y;
    result += HistoryTexture.SampleLevel(ColorSampler, float2(texPos12.x, texPos3.y), 0).rgb * w12.x * w3.y;

    // 【重みの和で割る ―― 5タップ化で落とした角4タップのぶんを戻す】
    // 16タップの重みの和は 1 だが、角(w0*w0, w0*w3, w3*w0, w3*w3)は正の小さい値で、落とすと
    // 和が 1 − (w0.x + w3.x)(w0.y + w3.y) になる(f=0.5 で 0.984)。1回の損失は最大 1.6% でも、
    // ここは上限64の帰還ループなので定常値が (1/64) / (1 − (63/64)·g) まで沈む ――
    // 61.7o.8 で「原因未確認」だった総和比の暗化(0.927 → 0.896)はこの取りこぼしで説明がつく。
    // TAA.hlsl は実効累積が約10フレームなので同じ取りこぼしが目に見えなかった
    const float weightSum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    result /= max(weightSum, 1e-5f);

    // 【近傍クランプ】負のローブのリンギングは近傍の外へ飛び出す成分なので、
    // バイリニアの2x2近傍の min/max へクランプして止める(RELAX/NRD が同じ位置で同じことを
    // している)。飛び出していない鋭さはそのまま残る。
    // 以前ここに「クランプしても総和比が 0.901 までしか戻らず暗化はリンギングではない」と
    // 書いていたが、その暗化の正体は上の重みの取りこぼしだった。正規化したあとの実測は
    // 総和比 0.9375 / |相対誤差|中央 0.0514 / S 0.579 / 誤差>0 49.3% で、バイリニア
    // (0.9273 / 0.0600 / 0.480 / 49.2%)をすべての指標で上回る(61.7q)
    const float2 texelSize2 = texelSize;
    const float3 c00 = HistoryTexture.SampleLevel(ColorSampler, (texPos1 + float2(-0.0f, -0.0f)) * texelSize2, 0).rgb;
    const float3 c10 = HistoryTexture.SampleLevel(ColorSampler, (texPos1 + float2(1.0f, 0.0f)) * texelSize2, 0).rgb;
    const float3 c01 = HistoryTexture.SampleLevel(ColorSampler, (texPos1 + float2(0.0f, 1.0f)) * texelSize2, 0).rgb;
    const float3 c11 = HistoryTexture.SampleLevel(ColorSampler, (texPos1 + float2(1.0f, 1.0f)) * texelSize2, 0).rgb;
    const float3 lo = min(min(c00, c10), min(c01, c11));
    const float3 hi = max(max(c00, c10), max(c01, c11));

    // 負の色を履歴へ入れると次フレーム以降も残り続けるので、最後に0で止める
    return max(clamp(result, lo, hi), 0.0f);
}

// 7x7 の輝度の (平均, 二乗平均)。タップの復調は中心の係数で代用する
// (反射率は7x7の窓では大きく変わらない)。
// 【計算の順序を変えないこと】短履歴の分散フォールバックはこの結果をそのまま使う。
// 足す順・割る順を変えると OFF 時のバイト同一が崩れる
float2 SpatialLuminanceMoments(uint2 pixel, uint2 outputSize, float3 demod)
{
    const float invDemodLum = 1.0f / max(Luminance(demod), 1e-6f);
    float sm1 = 0.0f;
    float sm2 = 0.0f;
    float count = 0.0f;
    [unroll]
    for (int dy = -3; dy <= 3; ++dy)
    {
        [unroll]
        for (int dx = -3; dx <= 3; ++dx)
        {
            const int2 p = clamp(int2(pixel) + int2(dx, dy), int2(0, 0), int2(outputSize) - 1);
            const float l = Luminance(InputTexture.Load(int3(p, 0)).rgb) * invDemodLum;
            sm1 += l;
            sm2 += l * l;
            count += 1.0f;
        }
    }
    sm1 /= count;
    sm2 /= count;
    return float2(sm1, sm2);
}

// アンチラグが発火したときの累積上限。newLength < 4 の空間分散フォールバックの境目の
// 1つ上に置き、発火画素を a-trous に強く混ぜさせる
static const float kAntiLagMinFrames = 4.0f;

// 残差駆動のアンチラグ。「現フレームの7x7平均を数フレームならした値」と「履歴の7x7平均」の
// **相対変化**で変化を検出し、累積上限を落とす。
//
// 【ここへ来るまでに2つの設計を測って落とした。どちらも同じ地雷を別の形で踏んだ】
// MegaLights の生標本は裾が重く(1/p の重み)、相対 std が画素ごとに 0.3〜1.0 と6倍ばらつき、
// しかも**右へ歪んでいる**(中央値 < 平均)。BistroExteriorNight / 手法3 / 静止と消灯での実測:
//
//  (1) 平均の差を画素の時間std(sigT)で割る … σ換算係数を振っても両立しない
//        σ    静止の誤発火   消灯の発火
//        0.25  29.2%         97.8%
//        1.0    0.98%        73.6%
//        2.0    0.086%       34.2%
//      消灯の信号(相対変化 1.0)と静止のノイズ(相対 0.1 程度)は10倍離れているのに、
//      sigT で割ると画素ごとの sigT のばらつきに埋もれる。**正規化の物差しが違っていた**
//  (2) タップごとの符号検定(生 < 履歴 を数える)… 静止で 30.8% 誤発火
//      分布が右に歪んでいるので、静止でも約75%のタップが「生 < 平均」になる。
//      符号検定は**分布の歪みで構造的に偏る**。ノイズではなく設計の誤り
//
// 【今の形】平均どうしを比べる(どちらも E[l] の不偏推定なので歪みで偏らない)。
// 生の7x7平均はファイアフライ1個で動くので、**数フレームの短い EMA でならしてから**比べる。
// ならした値は HistoryOutTexture.a に**負の値**で持ち回る(.a は読み手が無い枠。
// 0 と OFF の 1.0 は「短い履歴なし」の番兵になり、OFF の書き込みは変えないので
// バイト同一が保てる)。発火の遅れは EMA の長さぶん(既定4なら2〜3フレーム)で、
// 64フレームの残光に比べれば無視できる。
//
// Params3.y = t0, Params3.z = t1(相対変化 |fast − hist| / max(fast, hist) に対する
// smoothstep の両端)、Params3.w = 短い EMA の長さ[フレーム](0 で無効)。
// 初期値は EngineDefaults.h(静止と消灯の両方を満たす領域を掃引で決める)
float AntiLagCap(uint2 pixel, float2 historyUv, float maxFrames, out float fire, out float fastMu)
{
    const float2 texelSize = 1.0f / float2(Params0.xy);
    // タップの復調は**タップごと**に行う(SpatialLuminanceMoments の「中心の係数で代用」は
    // ここでは使えない)。HistoryTexture は画素ごとに復調された色なので、現フレーム側も
    // 画素ごとに復調しないと、7x7 の窓にアルベドの縁が入るだけで両者の平均が恒常的に
    // 食い違う。中心代用のときの静止での誤発火(k>0.5)は 2.95%、うち誤発火画素の
    // 7x7 内の復調係数の変動係数は中央値 0.67(発火しない画素は 0.03)で、
    // タップごとに直すと 0.17% へ落ちた(24 フレームの生入力からのオフライン再現、
    // 実機の 3.06% を再現した上で比較。BistroExteriorNight / 手法3 / 静止)
    float sumC = 0.0f;
    float sumH = 0.0f;
    [unroll]
    for (int dy = -3; dy <= 3; ++dy)
    {
        [unroll]
        for (int dx = -3; dx <= 3; ++dx)
        {
            const int2 p = clamp(int2(pixel) + int2(dx, dy), int2(0, 0), int2(Params0.xy) - 1);
            const float2 pUv = (float2(p) + 0.5f) * texelSize;
            const float tapDemodLum = max(Luminance(DemodulationFactor(pUv)), 1e-6f);
            sumC += Luminance(InputTexture.Load(int3(p, 0)).rgb) / tapDemodLum;
            const float2 tapUv = historyUv + float2(dx, dy) * texelSize;
            sumH += Luminance(HistoryTexture.SampleLevel(ColorSampler, tapUv, 0).rgb);
        }
    }
    const float muC = sumC / 49.0f;
    const float muH = sumH / 49.0f;

    // 前フレームの「ならした7x7平均」。負なら持っている、0以上(背景の0 / OFF の 1.0)なら無い
    const float fastPrevRaw = HistoryTexture.SampleLevel(ColorSampler, historyUv, 0).a;
    const float fastPrev = (fastPrevRaw < 0.0f) ? -fastPrevRaw : muC;
    const float fastFrames = max(Params3.w, 1.0f);
    fastMu = lerp(fastPrev, muC, 1.0f / fastFrames);

    const float relDiff = abs(fastMu - muH) / max(max(fastMu, muH), 1e-6f);
    fire = smoothstep(Params3.y, Params3.z, relDiff);
    return lerp(maxFrames, kAntiLagMinFrames, fire);
}

// 履歴の1タップが「今の画素と同じ面か」を判定する。しきい値は従来と同一
// (kMaxRelativeDepthDiff / kMinNormalDot / kMaxMaterialDiff)。**緩める方向へは一切動かさない。**
bool HistoryTapValid(int2 tapPixel, uint2 outputSize, float viewZ, float3 N, float2 material)
{
    float hViewZ;
    float3 hN;
    float2 hMaterial;
    bool hValid;

    const int2 clamped = clamp(tapPixel, int2(0, 0), int2(outputSize) - 1);
    if (Params2.z != 0.0f)
    {
        const MegaLightsHistoryGuide guide = HistoryGuide[clamped.y * outputSize.x + clamped.x];
        hViewZ = guide.ViewZ;
        hN = OctDecode(MegaLightsUnpackNormalOct(guide.NormalOct));
        MegaLightsUnpackMaterial(guide.Material, hMaterial.x, hMaterial.y);
        hValid = (guide.ViewZ != 0.0f);
    }
    else
    {
        // ガイドが無いときは従来どおり現フレームのG-Bufferで代用する。
        // 【ここも4タップに揃える】片方だけ1タップのままにすると、
        // 「ガイドの有無で挙動が変わる」条件がもう1つ増えて切り分けが利かなくなる
        const float2 tapUv = (float2(clamped) + 0.5f) / float2(outputSize);
        const float hDepth = DepthTexture.SampleLevel(DataSampler, tapUv, 0).r;
        hViewZ = (hDepth > 0.0f) ? TileViewZ(tapUv, hDepth) : 0.0f;
        hN = OctDecode(NormalTexture.SampleLevel(DataSampler, tapUv, 0).xy);
        hMaterial = MaterialTexture.SampleLevel(DataSampler, tapUv, 0).rg;
        hValid = (hDepth > 0.0f);
    }

    if (!hValid)
    {
        return false;
    }
    return abs(hViewZ - viewZ) <= kMaxRelativeDepthDiff * max(abs(viewZ), 1e-3f) &&
           dot(N, hN) >= kMinNormalDot &&
           abs(hMaterial.r - material.r) <= kMaxMaterialDiff &&
           abs(hMaterial.g - material.g) <= kMaxMaterialDiff;
}

[numthreads(8, 8, 1)]
void CSTemporalAccum(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    const uint2 outputSize = Params0.xy;
    if (pixel.x >= outputSize.x || pixel.y >= outputSize.y)
    {
        return;
    }

    const float2 uv = (float2(pixel) + 0.5f) / float2(outputSize);
    const float depth = DepthTexture.SampleLevel(DataSampler, uv, 0).r;
    const float3 raw = InputTexture.Load(int3(pixel, 0)).rgb;

    if (depth <= 0.0f)
    {
        // 背景。【必ず書くこと】RHIにUAVのクリアが無く、書かずにreturnすると前フレームが残る
        OutputTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        OutputMomentsTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        HistoryOutTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        HistoryMomentsOutTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // --- 復調してから混ぜる ---
    const float3 demod = DemodulationFactor(uv);
    float3 current = raw / demod;

    // --- ファイアフライの抑制(近傍クランプ) ---
    // 【何を切っているのか】RISの 1/p の重みは裾の重い分布を作る。たまたま小さい確率で
    // 引かれた灯は大きな W で割り戻され、桁違いに明るい1画素になる。これが画面上で
    // 最も目につく「白い粒」で、履歴が無い状態(カメラを動かした直後)で顕著になる
    // (実測は docs/ImplementationDetail.md 61.7g.4)。
    // 【上側だけ切る】暗い側は触らない。黒く沈んだ画素は影の縁の黒い斑点の原因になり、
    // そちらは別の仕組み(可視性込みの目標関数とBlockedLightsキャッシュ)で潰してある。
    // 【色相は保つ】輝度の比で全成分を縮める。成分ごとに切ると色が転ぶ。
    // 【平均+k・標準偏差では切れない】一度そう書いて外した。履歴が無い場所では近傍も
    // 同じだけノイジーで、**標準偏差がファイアフライ自身に押し上げられる**ため上限が
    // 外れ値の上に来る。基準には外れ値に強い量が要る。
    // 【採ったやり方】5x5の平均を取り、その8倍を超えるタップを外してもう一度平均を取る
    // (1回の刈り込み平均)。上限はその k 倍。平均は25画素で薄まるので、3%が外れ値でも
    // 基準はほとんど動かない。
    // 【バイアスが入る】切ったぶんのエネルギーは戻らないので、強さは
    // 「総和比を落とさない範囲でどこまで切れるか」で決める(EngineDefaults.h の根拠を参照)
    if (Params2.y > 0.0f)
    {
        const float invDemodLum = 1.0f / max(Luminance(demod), 1e-6f);
        float tapLum[25];
        float rawMean = 0.0f;
        [unroll]
        for (int cy = -2; cy <= 2; ++cy)
        {
            [unroll]
            for (int cx = -2; cx <= 2; ++cx)
            {
                const int2 cp = clamp(int2(pixel) + int2(cx, cy), int2(0, 0), int2(outputSize) - 1);
                const float l = Luminance(InputTexture.Load(int3(cp, 0)).rgb) * invDemodLum;
                tapLum[(cy + 2) * 5 + (cx + 2)] = l;
                rawMean += l;
            }
        }
        rawMean /= 25.0f;

        // 刈り込み平均。極端なタップを外してから取り直す
        const float trimLimit = rawMean * 8.0f;
        float trimmedSum = 0.0f;
        float trimmedCount = 0.0f;
        [unroll]
        for (uint t = 0u; t < 25u; ++t)
        {
            if (tapLum[t] <= trimLimit)
            {
                trimmedSum += tapLum[t];
                trimmedCount += 1.0f;
            }
        }
        const float robustMean = (trimmedCount > 0.0f) ? (trimmedSum / trimmedCount) : rawMean;

        const float limit = robustMean * Params2.y;
        const float curLum = Luminance(current);
        if (curLum > limit && curLum > 1e-6f)
        {
            current *= limit / curLum;
        }
    }

    const float lum = Luminance(current);

    const float3 N = OctDecode(NormalTexture.SampleLevel(DataSampler, uv, 0).xy);
    const float viewZ = TileViewZ(uv, depth);
    const float2 material = MaterialTexture.SampleLevel(DataSampler, uv, 0).rg;

    // --- 再投影。TAAとまったく同じ引き方(historyUv = uv - velocity) ---
    // 【引き方は同じでも、引くフィルタが違っていた】下の SampleHistoryColor を参照
    const float2 velocity = VelocityTexture.SampleLevel(DataSampler, uv, 0).rg;
    const float2 historyUv = uv - velocity;

    float3 historyColor = float3(0.0f, 0.0f, 0.0f);
    float2 historyMoments = float2(0.0f, 0.0f);
    float historyLength = 0.0f;
    bool historyValid = false;

    if (Params0.z != 0u && all(historyUv >= 0.0f) && all(historyUv <= 1.0f) && Params3.x != 0.0f)
    {
        // --- 4タップ判定 ---
        // 履歴の**色**はバイリニアで2x2を混ぜているのに、その4タップが妥当かどうかを
        // 最近傍1点でしか見ていなかった。帰結は2つとも実害で、
        //   (1) 1点だけがシルエットの向こう側だと履歴全体を棄却する(本当は妥当なのに捨てる)
        //   (2) 1点が通れば残り3タップが別の面でも 3/4 の重みで色が入る
        // ここでは4点それぞれを同じしきい値で判定し、**通ったタップだけを
        // バイリニア重みで加重平均する**。時間再利用(MegaLightsTemporal)は
        // 元から2x2を走査しており、デノイザだけが片肺だった。
        //
        // 【Catmull-Rom は「4タップ全部が通ったときだけ」併用する】部分採用があるとハードウェアの
        // 補間に載せられないので、1つでも棄却されたタップがあれば通ったタップの加重平均(下)を使う。
        // 全部通ったなら 2x2 の芯は同じ面なので Catmull-Rom(近傍クランプ付き)で引く ――
        // 4タップ判定の「別の面を混ぜない」と Catmull-Rom の「なまりを累積させない」を両取りする。
        // 4タップ判定が無効のときの Catmull-Rom は下の else 側(従来どおり)
        const float2 historyPixelF = historyUv * float2(outputSize) - 0.5f;
        const float2 baseF = floor(historyPixelF);
        const float2 frac2 = historyPixelF - baseF;
        const int2 baseI = int2(baseF);

        const float tapWeights[4] = {
            (1.0f - frac2.x) * (1.0f - frac2.y),
            frac2.x * (1.0f - frac2.y),
            (1.0f - frac2.x) * frac2.y,
            frac2.x * frac2.y,
        };
        const int2 tapOffsets[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };

        float weightSum = 0.0f;
        float3 colorSum = float3(0.0f, 0.0f, 0.0f);
        float2 momentSum = float2(0.0f, 0.0f);
        // 【履歴長は加重平均ではなく通ったタップの最小値を採る】平均だと、片方だけ長い履歴を
        // 持つタップに引きずられて α が過小になり、別の面の色が長く残る。保守側へ倒す
        float minLength = 1e30f;
        // 妥当性で落としたタップが1つでもあるか(Catmull-Rom へ切り替えてよいかの判定)
        bool anyRejected = false;

        [unroll]
        for (uint tap = 0u; tap < 4u; ++tap)
        {
            const int2 tapPixel = baseI + tapOffsets[tap];
            if (tapWeights[tap] <= 0.0f)
            {
                continue;
            }
            if (!HistoryTapValid(tapPixel, outputSize, viewZ, N, material))
            {
                anyRejected = true;
                continue;
            }
            const int2 clamped = clamp(tapPixel, int2(0, 0), int2(outputSize) - 1);
            const float4 h = HistoryTexture.Load(int3(clamped, 0));
            const float4 hm = HistoryMomentsTexture.Load(int3(clamped, 0));
            colorSum += h.rgb * tapWeights[tap];
            momentSum += hm.xy * tapWeights[tap];
            minLength = min(minLength, hm.z);
            weightSum += tapWeights[tap];
        }

        if (weightSum > 1e-5f)
        {
            // 全タップが通ったときだけ Catmull-Rom(色のみ。モーメントは負のローブを避けてバイリニア)
            historyColor = (Params2.w != 0.0f && !anyRejected)
                               ? SampleHistoryColorCatmullRom(historyUv, outputSize)
                               : colorSum / weightSum;
            historyMoments = momentSum / weightSum;
            historyLength = minLength;
            historyValid = true;
        }
    }
    else if (Params0.z != 0u && all(historyUv >= 0.0f) && all(historyUv <= 1.0f))
    {
        // --- 再投影先の幾何を引く ---
        // 【前フレームの幾何そのものを見る】以前は現フレームのG-Bufferを再投影先で
        // 引いて代用していた。これは**動く細い形状で必ず失敗する** ―― 椅子の桟が
        // 毎フレーム数画素動くと、historyUv が指す現フレームの位置にはもう桟がなく
        // 背後の壁が写っているので、深度が桁違いに食い違って履歴が棄却される。
        // 棄却された画素は1サンプルの推定値がそのまま出るため、**動かしたときだけ
        // 細い形状に白い粒が乗る**という形で現れていた。
        // 時間再利用が前フレームの法線・線形深度・材質を全画素ぶん書いているので、それを読む。
        // 時間再利用を切っている場合はガイドが更新されないので、従来どおり代用する
        float hViewZ;
        float3 hN;
        float2 hMaterial;
        bool hValid;
        if (Params2.z != 0.0f)
        {
            const int2 historyPixel =
                clamp(int2(historyUv * float2(outputSize)), int2(0, 0), int2(outputSize) - 1);
            const MegaLightsHistoryGuide guide =
                HistoryGuide[historyPixel.y * outputSize.x + historyPixel.x];
            hViewZ = guide.ViewZ;
            hN = OctDecode(MegaLightsUnpackNormalOct(guide.NormalOct));
            MegaLightsUnpackMaterial(guide.Material, hMaterial.x, hMaterial.y);
            // ガイドは背景を ViewZ=0 で表す
            hValid = (guide.ViewZ != 0.0f);
        }
        else
        {
            const float hDepth = DepthTexture.SampleLevel(DataSampler, historyUv, 0).r;
            hViewZ = (hDepth > 0.0f) ? TileViewZ(historyUv, hDepth) : 0.0f;
            hN = OctDecode(NormalTexture.SampleLevel(DataSampler, historyUv, 0).xy);
            hMaterial = MaterialTexture.SampleLevel(DataSampler, historyUv, 0).rg;
            hValid = (hDepth > 0.0f);
        }

        if (hValid)
        {
            if (abs(hViewZ - viewZ) <= kMaxRelativeDepthDiff * max(abs(viewZ), 1e-3f) &&
                dot(N, hN) >= kMinNormalDot &&
                abs(hMaterial.r - material.r) <= kMaxMaterialDiff &&
                abs(hMaterial.g - material.g) <= kMaxMaterialDiff)
            {
                // 色だけ再サンプリングのフィルタを選べる。モーメントは必ずバイリニア
                // (負のローブで履歴長と分散が壊れるため。SampleHistoryColorCatmullRom を参照)
                historyColor = (Params2.w != 0.0f)
                                   ? SampleHistoryColorCatmullRom(historyUv, outputSize)
                                   : HistoryTexture.SampleLevel(ColorSampler, historyUv, 0).rgb;
                const float4 hm = HistoryMomentsTexture.SampleLevel(ColorSampler, historyUv, 0);
                historyMoments = hm.xy;
                historyLength = hm.z;
                historyValid = true;
            }
        }
    }

    // --- 混ぜる ---
    // α = 1/min(履歴の長さ+1, 上限)。上限で止めるのは、止めないと動く物に追従できなくなるため。
    // 上限をTAAより短くするのは冒頭の「二重に掛けない」の通り。
    //
    // 【上限は一律ではない(アンチラグ)】従来の newLength は「その画素の信号が変化したか」を
    // 一切見ていなかった。だから遅れは上限が全画素へ一律に決めており、
    // 灯を消しても (1-1/上限)^t で尾を引く(上限64なら10%まで2.5秒。61.7j.6)。
    // 残差が大きい画素だけ上限を cap まで落とすことで、静穏な画素は長く累積してノイズを
    // 下げたまま、変化した画素だけ短く追従させる。式と罠は下の AntiLagCap を参照
    const float maxFrames = max(Params1.y, 1.0f);
    const bool antiLagOn = (Params3.w > 0.0f);
    float cap = maxFrames;
    float fire = 0.0f;
    // ならした7x7平均。HistoryOutTexture.a へ負の値で持ち回る(番兵の規約は AntiLagCap)
    float fastMu = 0.0f;
    bool fastMuValid = false;
    if (antiLagOn && historyValid)
    {
        cap = AntiLagCap(pixel, historyUv, maxFrames, fire, fastMu);
        fastMuValid = true;
    }
    const float newLength = historyValid ? min(historyLength + 1.0f, cap) : 1.0f;
    const float alpha = 1.0f / newLength;

    const float3 blended = historyValid ? lerp(historyColor, current, alpha) : current;
    const float2 moments = historyValid
                               ? lerp(historyMoments, float2(lum, lum * lum), alpha)
                               : float2(lum, lum * lum);

    // --- 分散を作る。ここで1回だけ作り、à-trousは段ごとにこれを畳んで次段へ渡す ---
    // 【なぜここへ移したのか】以前はà-trousが毎段この場で時間分散を計算し直していた。
    // 段を重ねると入力は既に平滑化されているのに分散だけ生のままなので、
    // 輝度の門番の分母(σ・√分散)が過大になり、**後段ほど重みが1へ寄って
    // ただのぼかしになる**。実測でも3段以上は誤差が悪化した(2段0.0394 → 5段0.0461)
    float variance = max(moments.y - moments.x * moments.x, 0.0f);
    if (newLength < 4.0f)
    {
        // 【履歴が短い画素は時間分散を信用しない】その場の7x7で代用する。
        // これをやらないと、遮蔽が外れた直後(disocclusion)の画素が
        // 「分散0 = 信用できる」と誤判定され、ノイズがそのまま残る。
        // アンチラグが発火して cap が 4 未満へ落ちた画素もここへ来る ―― 発火画素は
        // ノイズが多いので a-trous に強く混ぜさせるのは望ましい結合。
        // 【ここでだけ計算する】OFF のときは従来と同じ場所・同じ順序で計算させ、
        // 「OFF なら変更前とバイト同一」を保つ(ホイストすると丸めの順序が変わる)
        const float2 spatialMoments = SpatialLuminanceMoments(pixel, outputSize, demod);
        variance = max(spatialMoments.y - spatialMoments.x * spatialMoments.x, 0.0f);
    }

    OutputTexture[pixel] = float4(blended, 1.0f);
    OutputMomentsTexture[pixel] = float4(moments, newLength, variance);
    // 翌フレームの履歴。pingはà-trousが上書きするので独立に残す。
    // .wは翌フレームの時間累積では読まない(分散はそのフレームで作り直す)。
    // 【アンチラグ有効時は .w に発火の度合い(0〜1)を写す】読み手が無い枠なので、
    // 新しいテクスチャを足さずに -dumptex MegaLightsDenoiseMoments で発火マップが取れる。
    // OFF のときは従来どおり分散(=ping と同じ値)を書き、バイト同一を保つ
    // .a は読み手が無い枠。アンチラグが走ったフレームだけ「ならした7x7平均」を負の値で置く。
    // それ以外(OFF・履歴無効)は従来どおり 1.0 を書き、バイト同一を保つ
    HistoryOutTexture[pixel] = float4(blended, fastMuValid ? -fastMu : 1.0f);
    HistoryMomentsOutTexture[pixel] = float4(moments, newLength, antiLagOn ? fire : variance);
}

// ---------------------------------------------------------------------------
// 段2: エッジ停止付き à-trous。段ごとにステップ幅を倍にして広い範囲をならす
// ---------------------------------------------------------------------------
static const float kAtrousKernel[3] = { 3.0f / 8.0f, 1.0f / 4.0f, 1.0f / 16.0f };
// 門番に渡す分散を平滑化するための3x3ガウス(中心 / 辺 / 角 = 4:2:1 の分離型)
static const float kGaussian3x3[2] = { 2.0f / 4.0f, 1.0f / 4.0f };

[numthreads(8, 8, 1)]
void CSAtrous(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    const uint2 outputSize = Params0.xy;
    if (pixel.x >= outputSize.x || pixel.y >= outputSize.y)
    {
        return;
    }

    const float2 uv = (float2(pixel) + 0.5f) / float2(outputSize);
    const float depth = DepthTexture.SampleLevel(DataSampler, uv, 0).r;
    const float4 center = InputTexture.Load(int3(pixel, 0));

    if (depth <= 0.0f)
    {
        OutputTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        OutputMomentsTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float3 N = OctDecode(NormalTexture.SampleLevel(DataSampler, uv, 0).xy);
    const float viewZ = TileViewZ(uv, depth);
    const float4 centerMoments = HistoryMomentsTexture.Load(int3(pixel, 0));

    // 輝度の分散。時間累積が作り、前段までのà-trousが畳んできた値をそのまま受け取る
    const float variance = max(centerMoments.w, 0.0f);

    // 【門番に使う分散は3x3で平滑化する(本家SVGF)】分散そのものが1画素ぶんの推定で
    // ノイジーなので、生の値で門番を作ると「たまたま分散が小さく出た画素」だけが
    // 近傍と混ざらず粒として残る。重み計算に使うのは平滑化した値、
    // 次段へ渡すのは下で重みの二乗で畳んだ値で、役割が違うことに注意
    float varSmooth = 0.0f;
    float varWeightSum = 0.0f;
    [unroll]
    for (int vy = -1; vy <= 1; ++vy)
    {
        [unroll]
        for (int vx = -1; vx <= 1; ++vx)
        {
            const int2 vp = clamp(int2(pixel) + int2(vx, vy), int2(0, 0), int2(outputSize) - 1);
            const float g = kGaussian3x3[abs(vx)] * kGaussian3x3[abs(vy)];
            varSmooth += g * max(HistoryMomentsTexture.Load(int3(vp, 0)).w, 0.0f);
            varWeightSum += g;
        }
    }
    varSmooth = (varWeightSum > 1e-6f) ? (varSmooth / varWeightSum) : variance;

    const float lumStdDev = sqrt(varSmooth) + 1e-6f;
    const float centerLum = Luminance(center.rgb);

    const int step = (int)max(Params1.x, 1.0f);
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float weightSum = 0.0f;
    // 分散は重みの二乗で畳む。Var[Σw_i x_i / Σw_i] = Σw_i^2 Var[x_i] / (Σw_i)^2
    float varSum = 0.0f;

    [unroll]
    for (int dy = -2; dy <= 2; ++dy)
    {
        [unroll]
        for (int dx = -2; dx <= 2; ++dx)
        {
            // 【画面外は折り返す】同じシェーダ内で境界の扱いが2種類あると将来混乱する。
            // 上の空間分散のフォールバックと同じ clamp に揃えてある。
            // なお打ち切っても採用したタップだけで正規化するので、どちらでも
            // 系統的な偏りにはならない(縁を30px切って測っても損失は減らなかった)
            const int2 p = clamp(int2(pixel) + int2(dx, dy) * step, int2(0, 0), int2(outputSize) - 1);
            const float2 puv = (float2(p) + 0.5f) / float2(outputSize);
            const float pDepth = DepthTexture.SampleLevel(DataSampler, puv, 0).r;
            if (pDepth <= 0.0f)
            {
                continue;
            }

            const float3 tap = InputTexture.Load(int3(p, 0)).rgb;

            // --- エッジ停止 ---
            // 深度: 別の面へ滲ませない。視線に対して斜めの面でも切れないよう相対差で見る
            const float pViewZ = TileViewZ(puv, pDepth);
            const float wZ = exp(-abs(pViewZ - viewZ) / (Params2.x * max(abs(viewZ), 1e-3f) + 1e-6f));
            // 法線: 角を丸めない
            const float3 pN = OctDecode(NormalTexture.SampleLevel(DataSampler, puv, 0).xy);
            const float wN = pow(saturate(dot(N, pN)), Params1.w);
            // 輝度: 分散で正規化する。ノイズなら分散が大きいので広く混ぜ、
            // 本物の明暗差なら分散が小さいので混ぜない ―― これがSVGFの要。
            //
            // 【この重みは原理的にエネルギーを減らす】中心から遠い値ほど弾くので、
            // 1/p の重みが作る裾の重い分布では**明るいタップのほうが強く弾かれる**。
            // 結果としてフィルタ後の平均が暗い側へ寄る。実測(ManyLightsTest / N=256)で
            // 段を重ねるごとに総和が -1.1% / -2.5% / -5.8% と積み上がった。
            // 実装ミスではなくSVGF系の既知の性質で、σ(Params1.z)を大きくすると混ぜる範囲が
            // 広がるぶん損失も増える。**平均を動かさないことを優先するなら段数を減らす。**
            // 根拠と実測は docs/ImplementationDetail.md 61.7d
            // 【中心の輝度が厳密に0の画素は、輝度の門番を外して近傍から埋める】
            // 確率的サンプリングでは「届く灯を一度も引き当てられない画素」が影の縁に
            // 黒い斑点として残る(可視レイで殺され続けるか、目標関数が遮蔽された灯に
            // 支配される画素)。輝度の門番は黒を「本物のエッジ」として守ってしまうので、
            // 中心が0のときだけ無効化する。真に照らされない画素は近傍も0なので安全
            // (0どうしの平均は0のまま)
            const float wL = (centerLum <= 0.0f)
                                 ? 1.0f
                                 : exp(-abs(centerLum - Luminance(tap)) / (Params1.z * lumStdDev));

            const float kernel = kAtrousKernel[abs(dx)] * kAtrousKernel[abs(dy)];
            const float w = kernel * wZ * wN * wL;
            sum += tap * w;
            weightSum += w;
            varSum += w * w * max(HistoryMomentsTexture.Load(int3(p, 0)).w, 0.0f);
        }
    }

    // 【0除算のガード】全部の近傍が弾かれたら中心をそのまま返す
    const float3 filtered = (weightSum > 1e-6f) ? (sum / weightSum) : center.rgb;
    const float filteredVariance =
        (weightSum > 1e-6f) ? (varSum / (weightSum * weightSum)) : variance;
    OutputTexture[pixel] = float4(filtered, center.a);
    // xyz(1次・2次モーメントと履歴の長さ)はそのまま、wだけ畳んだ分散に差し替えて次段へ渡す。
    // これで段が進むほど分散が小さくなり、輝度の門番が効き続ける
    OutputMomentsTexture[pixel] = float4(centerMoments.xyz, filteredVariance);
}

// ---------------------------------------------------------------------------
// 段3: 復調を戻して最終出力にする
// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSRemodulate(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    const uint2 outputSize = Params0.xy;
    if (pixel.x >= outputSize.x || pixel.y >= outputSize.y)
    {
        return;
    }
    const float2 uv = (float2(pixel) + 0.5f) / float2(outputSize);
    const float depth = DepthTexture.SampleLevel(DataSampler, uv, 0).r;
    if (depth <= 0.0f)
    {
        OutputTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    // 【復調に使ったのと同じ式で掛け戻す】ずれると色が変わる
    const float3 demod = DemodulationFactor(uv);
    OutputTexture[pixel] = float4(InputTexture.Load(int3(pixel, 0)).rgb * demod, 1.0f);
}
