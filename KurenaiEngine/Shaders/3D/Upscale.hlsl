// 超解像(空間アップスケール)。AMD FidelityFX Super Resolution 1.0 の
// EASU(Edge Adaptive Spatial Upsampling)と RCAS(Robust Contrast Adaptive Sharpening)の移植。
//
// 内部レンダー解像度で描いた最終LDR画像を、出力解像度へ拡大する。
// Presentパスのバイリニア拡大を置き換えるものであり、これが入ることで
// 「内部解像度を下げてフレーム時間を稼ぐ」という運用が実用になる。
//
// === なぜTonemapの後なのか ===
// EASUもRCASも「表示レンジの値」を前提に設計されている。EASUのエッジ検出は輝度差の絶対量を
// 見ており、RCASのリミッタは値域が[0,1]であること(下のPeakHi=1.0)を式に埋め込んでいる。
// HDRのSceneColorへ掛けると、プリ露出で数万倍になっている値に対してこれらの定数が意味を失い、
// 明るいエッジで極端なオーバーシュートが出る。Tonemap出力(ガンマ空間・[0,1])が正しい入力で、
// これはTonemap.hlslのシャープネスを最終出力側へ置いてあるのと同じ理由である。
//
// === なぜEASUとRCASを別パスに分けるのか ===
// RCASはEASUの結果の十字5タップを読む。1パスにまとめるとEASU出力を書きながら読むことになり、
// 同一リソースのSRV/UAV同時バインドになってしまう(Bloomがピラミッドをミップチェーン1枚では
// なくレベルごとの独立テクスチャで持っているのと同じ制約)。
//
// === 参照実装から変えた点 ===
// (1) 定数をAU4(uintビットキャスト)ではなくfloat4で渡す。参照実装がuintで持つのは
//     FP16パック経路(A_HALF)と共用するためで、このエンジンはSM5.0でも動く必要があるため
//     16bitパック経路を使わない。ならばfloatのまま持つほうがCPU側の構造体と素直に対応する。
// (2) RCASのリミッタの除算にゼロ除算ガードを入れた(理由はRcasLimiterRcp()のコメント)。
// (3) **スレッドの割り当てを、参照実装の「64スレッド/グループ + ARmp8x8による8x8のモートン風の
//     並べ替え + 1スレッド4画素」ではなく、このエンジンの慣例どおりの
//     [numthreads(8,8,1)] + SV_DispatchThreadID + 1スレッド1画素にした。**
//
//     参照実装が並べ替えを持つのは、EASUが1画素あたりGather4を12回撃つテクスチャフェッチ
//     律速のシェーダーで、隣接スレッドが重なった2x2ブロックを引くため、スレッド番号の並びが
//     そのままテクスチャキャッシュのヒット率になるから、という理屈である。
//
//     Intel UHD Graphics 620(Sponza / 1280x720出力 / 内部848x480 / DX11 / Release)で
//     3通りを実測した。**このGPUは連続稼働で絶対値が2〜3割ドリフトするため、
//     形を変えていないRCASパスとの比で見る**(各60サンプル超の中央値):
//
//       スレッドの割り当て                     EASU     RCAS    EASU/RCAS
//       64スレッド + ARmp8x8 + 1スレッド4画素   3.09ms   1.06ms    2.92
//       64スレッド + 行優先   + 1スレッド4画素   3.00ms   1.01ms    2.97
//       [numthreads(8,8,1)] + 1スレッド1画素    2.45ms   0.80ms    3.06
//
//     比で見ると3通りの差は5%以内で、**並べ替えの利得は測れなかった**。
//     利得が無い以上、エンジン内の他のコンピュートパス(Bloom・AutoExposure)と
//     読み方の揃った書き方を採る。別のGPUで有意差が出るなら戻す価値がある。
//     なお割り当てを変えても出力は変わらない(1画素あたりの計算式が同一のため)。
#include "Samplers.hlsli"

cbuffer UpscaleConstants : register(b1)
{
    // EASUの事前計算定数。CPU側のComputeEasuConstants()が入力/出力解像度から作る。
    // Con0.xy = 出力画素→入力画素のスケール、Con0.zw = その中心合わせのオフセット
    float4 EasuCon0;
    // Con1.xy = 入力テクセルサイズ、Con1.zw = 12タップの左上ブロックへのオフセット
    float4 EasuCon1;
    // Con2/Con3 = 残り3つのGather中心へのオフセット
    float4 EasuCon2;
    float4 EasuCon3;

    // 書き込み先(出力解像度)。EASU/RCASとも範囲外スレッドの早期リターンに使い、
    // RCASではさらに十字5タップの座標クランプにも使う
    uint2 OutputSize;
    // RCASのシャープネス。CPU側がexp2(-ストップ数)へ変換済みの線形値(1.0=最大)
    float RcasSharpnessScale;
    float UpscalePadding;
};

// EASU: Tonemapの出力(内部レンダー解像度)。RCAS: EASUの出力(出力解像度)
Texture2D SourceTexture : register(t0);
RWTexture2D<float4> DestTexture : register(u0);

// --- 参照実装(ffx_a.h)の近似ヘルパ ---

// 逆数のビット演算近似。速度のためだけではなく、**ゼロ除算のガードとして機能している**点が重要。
// EASUはlenX=max(|dc|,|cb|)の逆数を取るが、平坦な領域ではlenXがちょうど0になる。
// 正確なrcp(0)は+infになり、直後の abs(dirX)*inf が 0*inf = NaN を生む(dirXも同時に0になるため)。
// この近似は asuint(0.0)=0 から 0x7ef07ebb(約1.6e38)という有限の巨大値を返すので、
// 0を掛けた結果が素直に0になり、分岐なしで正しく縮退する
float ApproxRcpLow(float a)
{
    return asfloat(0x7ef07ebbu - asuint(a));
}

// 逆平方根のビット演算近似(いわゆる高速逆平方根の1定数版。ニュートン法の反復は行わない)
float ApproxRsqrtLow(float a)
{
    return asfloat(0x5f347d74u - (asuint(a) >> 1u));
}

// 中精度の逆数近似(ニュートン法1回)。RCASの最終正規化に使う。
// ここだけ精度を上げるのは、荒い近似だと分母のわずかな誤差が画面全体の明るさのズレとして
// 見えてしまうため(参照実装がAPrxMedRcpF1を指定しているのと同じ理由)
float ApproxRcpMed(float a)
{
    const float b = asfloat(0x7ef19fffu - asuint(a));
    return b * (-b * a + 2.0f);
}

float Min3(float a, float b, float c)
{
    return min(a, min(b, c));
}

float Max3(float a, float b, float c)
{
    return max(a, max(b, c));
}

float3 Min3(float3 a, float3 b, float3 c)
{
    return min(a, min(b, c));
}

float3 Max3(float3 a, float3 b, float3 c)
{
    return max(a, max(b, c));
}

// ============================================================================
// EASU
// ============================================================================

// 1タップぶんの重み付き加算。
// off  = 再構成位置からタップまでのオフセット(入力画素単位)
// dir  = 推定したエッジの方向(正規化済み)
// len2 = 方向に沿った異方性スケール
// lob  = 負のローブの強さ、clp = 距離^2の打ち切り点
void EasuTap(
    inout float3 accumColor, inout float accumWeight,
    float2 off, float2 dir, float2 len2, float lob, float clp, float3 tapColor)
{
    // オフセットをエッジ方向へ回転させ、異方性スケールを掛ける
    float2 v;
    v.x = (off.x * dir.x) + (off.y * dir.y);
    v.y = (off.x * -dir.y) + (off.y * dir.x);
    v *= len2;

    // 距離^2。窓の外へ出る角のタップがあるので打ち切る
    float d2 = min(v.x * v.x + v.y * v.y, clp);

    // sin()もrcp()もsqrt()も使わないLanczos2の近似:
    //   (25/16 * (2/5 * x^2 - 1)^2 - (25/16 - 1)) * (lob * x^2 - 1)^2
    //   |_____________________________________|   |__________________|
    //                    base                            window
    float wB = (2.0f / 5.0f) * d2 - 1.0f;
    float wA = lob * d2 - 1.0f;
    wB *= wB;
    wA *= wA;
    wB = (25.0f / 16.0f) * wB - (25.0f / 16.0f - 1.0f);
    const float w = wB * wA;

    accumColor += tapColor * w;
    accumWeight += w;
}

// 4つの近傍のうち1つぶんの、方向と「エッジらしさ」の累積。
// 十字5点(a=上, b=左, c=中央, d=右, e=下)の輝度から、+字の差分で勾配を取る。
// biS/biT/biU/biV はどの近傍かを表すフラグで、すべてコンパイル時定数なので分岐は消える
void EasuSet(
    inout float2 dir, inout float len,
    float2 pp, bool biS, bool biT, bool biU, bool biV,
    float lA, float lB, float lC, float lD, float lE)
{
    // バイリニア重み(s t / u v の並び)
    float w = 0.0f;
    if (biS) { w = (1.0f - pp.x) * (1.0f - pp.y); }
    if (biT) { w = pp.x * (1.0f - pp.y); }
    if (biU) { w = (1.0f - pp.x) * pp.y; }
    if (biV) { w = pp.x * pp.y; }

    // X方向。中央cの両側の差の絶対値の大きいほうで正規化することで、
    // 「勾配の反転(=エッジではなく細い線)」がlen=0へ滑らかに落ちる
    const float dc = lD - lC;
    const float cb = lC - lB;
    float lenX = ApproxRcpLow(max(abs(dc), abs(cb)));
    const float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;

    // Y方向も同じ
    const float ec = lE - lC;
    const float ca = lC - lA;
    float lenY = ApproxRcpLow(max(abs(ec), abs(ca)));
    const float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

// 出力画素ipの色を、入力画像の12タップから再構成する
float3 EasuFilter(uint2 ip)
{
    // --- 再構成位置。fpが整数部(左上の'f'タップ)、ppが小数部 ---
    float2 pp = (float2)ip * EasuCon0.xy + EasuCon0.zw;
    const float2 fp = floor(pp);
    pp -= fp;

    // --- 12タップのカーネル ---
    //     b c
    //   e f g h
    //   i j k l
    //     n o
    // Gather4を4回撃って集める。Gather4の戻り値の並びは
    //   .x=左下 .y=右下 .z=右上 .w=左上
    // で、下図の(0)〜(3)がそれぞれのGather中心になる。
    //       +---+---+
    //       |   |   |
    //       +--(0)--+
    //       | b | c |
    //   +---F---+---+---+
    //   | e | f | g | h |
    //   +--(1)--+--(2)--+
    //   | i | j | k | l |
    //   +---+---+---+---+
    //       | n | o |
    //       +--(3)--+
    //       |   |   |
    //       +---+---+
    //
    // 【ColorSamplerがClampであることが前提】画面端では再構成位置が入力画像の外へ出るため、
    // Gatherのアドレスモードがそのまま端の扱いになる。Wrapだと反対側の端が回り込んで
    // 画面の縁に偽のエッジが出る。スクリーン空間用のサンプラーセットにはWrapが1つも
    // 入らない設計になっているので構造的に安全(Samplers.hlsli冒頭の役割表を参照)
    const float2 p0 = fp * EasuCon1.xy + EasuCon1.zw;
    const float2 p1 = p0 + EasuCon2.xy;
    const float2 p2 = p0 + EasuCon2.zw;
    const float2 p3 = p0 + EasuCon3.xy;

    const float4 bczzR = SourceTexture.GatherRed(ColorSampler, p0);
    const float4 bczzG = SourceTexture.GatherGreen(ColorSampler, p0);
    const float4 bczzB = SourceTexture.GatherBlue(ColorSampler, p0);
    const float4 ijfeR = SourceTexture.GatherRed(ColorSampler, p1);
    const float4 ijfeG = SourceTexture.GatherGreen(ColorSampler, p1);
    const float4 ijfeB = SourceTexture.GatherBlue(ColorSampler, p1);
    const float4 klhgR = SourceTexture.GatherRed(ColorSampler, p2);
    const float4 klhgG = SourceTexture.GatherGreen(ColorSampler, p2);
    const float4 klhgB = SourceTexture.GatherBlue(ColorSampler, p2);
    const float4 zzonR = SourceTexture.GatherRed(ColorSampler, p3);
    const float4 zzonG = SourceTexture.GatherGreen(ColorSampler, p3);
    const float4 zzonB = SourceTexture.GatherBlue(ColorSampler, p3);

    // --- 輝度(の2倍)。FMA2回で済ませる近似で、係数の厳密さは方向推定に影響しない ---
    const float4 bczzL = bczzB * 0.5f + (bczzR * 0.5f + bczzG);
    const float4 ijfeL = ijfeB * 0.5f + (ijfeR * 0.5f + ijfeG);
    const float4 klhgL = klhgB * 0.5f + (klhgR * 0.5f + klhgG);
    const float4 zzonL = zzonB * 0.5f + (zzonR * 0.5f + zzonG);

    const float bL = bczzL.x;
    const float cL = bczzL.y;
    const float iL = ijfeL.x;
    const float jL = ijfeL.y;
    const float fL = ijfeL.z;
    const float eL = ijfeL.w;
    const float kL = klhgL.x;
    const float lL = klhgL.y;
    const float hL = klhgL.z;
    const float gL = klhgL.w;
    const float oL = zzonL.z;
    const float nL = zzonL.w;

    // --- 4近傍それぞれで方向と長さを求め、バイリニア重みで混ぜる ---
    float2 dir = float2(0.0f, 0.0f);
    float len = 0.0f;
    EasuSet(dir, len, pp, true,  false, false, false, bL, eL, fL, gL, jL);
    EasuSet(dir, len, pp, false, true,  false, false, cL, fL, gL, hL, kL);
    EasuSet(dir, len, pp, false, false, true,  false, fL, iL, jL, kL, nL);
    EasuSet(dir, len, pp, false, false, false, true,  gL, jL, kL, lL, oL);

    // --- 方向の正規化。ほぼ0のときは「エッジ無し」として軸方向へ倒す ---
    const float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    const bool zeroDir = dirR < (1.0f / 32768.0f);
    dirR = ApproxRsqrtLow(dirR);
    dirR = zeroDir ? 1.0f : dirR;
    dir.x = zeroDir ? 1.0f : dir.x;
    dir *= dirR;

    // lenは[0,2]で入ってくるので[0,1]へ直し、二乗して立ち上がりを鈍らせる
    len = len * 0.5f;
    len *= len;

    // カーネルを引き伸ばす(軸方向で1.0、斜め方向でsqrt(2))
    const float stretch = (dir.x * dir.x + dir.y * dir.y) * ApproxRcpLow(max(abs(dir.x), abs(dir.y)));
    // 回転後の異方性スケール。エッジが強いほど、x方向はstretchへ、y方向は1/2へ寄る
    const float2 len2 = float2(1.0f + (stretch - 1.0f) * len, 1.0f - 0.5f * len);
    // エッジの強さに応じて窓の広さを sqrt(2) 〜 2 の間で動かす
    const float lob = 0.5f + ((1.0f / 4.0f - 0.04f) - 0.5f) * len;
    const float clp = ApproxRcpLow(lob);

    // --- 最近傍4点(f,g,j,k)のmin/max。最後のデリンギングに使う ---
    const float3 fColor = float3(ijfeR.z, ijfeG.z, ijfeB.z);
    const float3 gColor = float3(klhgR.w, klhgG.w, klhgB.w);
    const float3 jColor = float3(ijfeR.y, ijfeG.y, ijfeB.y);
    const float3 kColor = float3(klhgR.x, klhgG.x, klhgB.x);
    const float3 min4 = min(Min3(fColor, gColor, jColor), kColor);
    const float3 max4 = max(Max3(fColor, gColor, jColor), kColor);

    // --- 12タップの加重平均 ---
    float3 accumColor = float3(0.0f, 0.0f, 0.0f);
    float accumWeight = 0.0f;
    EasuTap(accumColor, accumWeight, float2( 0.0f, -1.0f) - pp, dir, len2, lob, clp, float3(bczzR.x, bczzG.x, bczzB.x)); // b
    EasuTap(accumColor, accumWeight, float2( 1.0f, -1.0f) - pp, dir, len2, lob, clp, float3(bczzR.y, bczzG.y, bczzB.y)); // c
    EasuTap(accumColor, accumWeight, float2(-1.0f,  1.0f) - pp, dir, len2, lob, clp, float3(ijfeR.x, ijfeG.x, ijfeB.x)); // i
    EasuTap(accumColor, accumWeight, float2( 0.0f,  1.0f) - pp, dir, len2, lob, clp, jColor);                            // j
    EasuTap(accumColor, accumWeight, float2( 0.0f,  0.0f) - pp, dir, len2, lob, clp, fColor);                            // f
    EasuTap(accumColor, accumWeight, float2(-1.0f,  0.0f) - pp, dir, len2, lob, clp, float3(ijfeR.w, ijfeG.w, ijfeB.w)); // e
    EasuTap(accumColor, accumWeight, float2( 1.0f,  1.0f) - pp, dir, len2, lob, clp, kColor);                            // k
    EasuTap(accumColor, accumWeight, float2( 2.0f,  1.0f) - pp, dir, len2, lob, clp, float3(klhgR.y, klhgG.y, klhgB.y)); // l
    EasuTap(accumColor, accumWeight, float2( 2.0f,  0.0f) - pp, dir, len2, lob, clp, float3(klhgR.z, klhgG.z, klhgB.z)); // h
    EasuTap(accumColor, accumWeight, float2( 1.0f,  0.0f) - pp, dir, len2, lob, clp, gColor);                            // g
    EasuTap(accumColor, accumWeight, float2( 1.0f,  2.0f) - pp, dir, len2, lob, clp, float3(zzonR.z, zzonG.z, zzonB.z)); // o
    EasuTap(accumColor, accumWeight, float2( 0.0f,  2.0f) - pp, dir, len2, lob, clp, float3(zzonR.w, zzonG.w, zzonB.w)); // n

    // 正規化してから最近傍4点の範囲へクランプする(デリンギング)。
    // 重みの合計は負のローブのぶん理論上0を跨ぎうるので、分母は下限を切っておく
    const float3 resolved = accumColor / max(accumWeight, 1e-6f);
    return min(max4, max(min4, resolved));
}

[numthreads(8, 8, 1)]
void CSEASU(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= OutputSize.x || dispatchThreadID.y >= OutputSize.y)
    {
        return;
    }

    DestTexture[dispatchThreadID.xy] = float4(EasuFilter(dispatchThreadID.xy), 1.0f);
}

// ============================================================================
// RCAS
// ============================================================================

// RCASのリミッタが許す最大のローブ強度。これ以上シャープにすると
// 1画素幅の線でリンギング(オーバーシュートの縁取り)が見え始める
#define KURENAI_RCAS_LIMIT (0.25f - (1.0f / 16.0f))

// リミッタ専用の逆数。分母が0になる場合があるのでガードする。
// 参照実装は正確なrcp()をそのまま使っているが、
//   ・hitMin側の分母 4*max は、3x3が完全な黒(夜間や陰の内側)のとき0になる
//   ・hitMax側の分母 4*min-4 は、3x3が完全な白(白飛び)のとき0になる
// どちらも 0/0 → NaN を作りうる。NaNが出た後の min/max の挙動はGPU依存なので、
// たまたま無害に潰れることに頼らず符号を保ったまま下限を切る。
// signedLowerBoundは分母が取りうる符号(正なら正の微小値、負なら負の微小値)を渡す
float RcasLimiterRcp(float denominator, float signedLowerBound)
{
    // 分母の符号は式の形から一意に決まっているため、絶対値ではなく符号付きで丸める
    const float clamped = (signedLowerBound > 0.0f)
        ? max(denominator, signedLowerBound)
        : min(denominator, signedLowerBound);
    return 1.0f / clamped;
}

float3 RcasLoad(int2 pos)
{
    // Loadは範囲外で0を返してしまうため、端の画素を複製する形でクランプする
    const int2 clamped = clamp(pos, int2(0, 0), int2(OutputSize) - 1);
    return SourceTexture.Load(int3(clamped, 0)).rgb;
}

[numthreads(8, 8, 1)]
void CSRCAS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= OutputSize.x || dispatchThreadID.y >= OutputSize.y)
    {
        return;
    }

    // 十字の5タップだけを使う
    //     b
    //   d e f
    //     h
    const int2 sp = int2(dispatchThreadID.xy);
    const float3 b = RcasLoad(sp + int2( 0, -1));
    const float3 d = RcasLoad(sp + int2(-1,  0));
    const float3 e = RcasLoad(sp);
    const float3 f = RcasLoad(sp + int2( 1,  0));
    const float3 h = RcasLoad(sp + int2( 0,  1));

    // 輝度の2倍(EASUと同じ近似)
    const float bL = b.b * 0.5f + (b.r * 0.5f + b.g);
    const float dL = d.b * 0.5f + (d.r * 0.5f + d.g);
    const float eL = e.b * 0.5f + (e.r * 0.5f + e.g);
    const float fL = f.b * 0.5f + (f.r * 0.5f + f.g);
    const float hL = h.b * 0.5f + (h.r * 0.5f + h.g);

    // ノイズ検出。周囲4点の平均と中央の差を、リングの輝度レンジで正規化する。
    // 孤立した1画素(=ノイズ)ほど1に近づくので、そのぶんシャープ量を半分まで落とす。
    // TAAの後なので入力は比較的きれいだが、ディザとSSRの残りノイズが増幅されるのを防ぐ
    float nz = 0.25f * (bL + dL + fL + hL) - eL;
    const float lumaRange = Max3(Max3(bL, dL, eL), fL, hL) - Min3(Min3(bL, dL, eL), fL, hL);
    nz = saturate(abs(nz) * ApproxRcpMed(lumaRange));
    nz = -0.5f * nz + 1.0f;

    // リングの min / max
    const float3 mn4 = min(Min3(b, d, f), h);
    const float3 mx4 = max(Max3(b, d, f), h);

    // リミッタ。「シャープ化しても値域[0,1]をはみ出さない」ローブ強度の上限を色ごとに求める。
    // PeakLo=0 側から見た余裕がhitMin、PeakHi=1 側から見た余裕がhitMax
    const float3 hitMin = mn4 * float3(
        RcasLimiterRcp(4.0f * mx4.r,  1e-6f),
        RcasLimiterRcp(4.0f * mx4.g,  1e-6f),
        RcasLimiterRcp(4.0f * mx4.b,  1e-6f));
    const float3 hitMax = (1.0f - mx4) * float3(
        RcasLimiterRcp(4.0f * mn4.r - 4.0f, -1e-6f),
        RcasLimiterRcp(4.0f * mn4.g - 4.0f, -1e-6f),
        RcasLimiterRcp(4.0f * mn4.b - 4.0f, -1e-6f));
    const float3 lobeRGB = max(-hitMin, hitMax);

    // 3色でいちばん厳しい制約を採り、上限で切る。lobeは常に負の値になる(負のローブ=シャープ化)
    float lobe = max(-KURENAI_RCAS_LIMIT, min(Max3(lobeRGB.r, lobeRGB.g, lobeRGB.b), 0.0f))
               * RcasSharpnessScale;
    lobe *= nz;

    // 十字4点へlobe(負)、中央へ1.0を掛けて正規化する
    const float rcpL = ApproxRcpMed(4.0f * lobe + 1.0f);
    const float3 result = (lobe * (b + d + f + h) + e) * rcpL;

    // 入力は[0,1]のLDRなので、丸め誤差ぶんのはみ出しをここで確実に落としておく
    DestTexture[dispatchThreadID.xy] = float4(saturate(result), 1.0f);
}
