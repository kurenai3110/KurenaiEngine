// ブルーム。Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare"
// (SIGGRAPH 2014) 方式の、ピラミッドを使ったプログレッシブなダウンサンプル/アップサンプル。
//
// ダウンサンプルは13タップ(4隅の2x2ボックス+中央)、アップサンプルは3x3テントフィルタで
// 1段上のレベルへ加算合成する。大きな半径のガウシアンを1回掛けるのに比べ、
// 小さいフィルタをレベル間で繰り返すほうが少ないタップ数で広く滑らかな裾を作れる。
//
// === 最初のダウンサンプルだけKaris平均を掛ける理由 ===
// SSR/SSILのノイズや鏡面ハイライトのように「面積は1画素でも輝度が極端に高い」点が入力にあると、
// ダウンサンプルで平均されたあとも大きな値として残り、カメラが動くたびにその画素が出入りして
// 激しくちらつく(ファイアフライ)。Karis平均は各タップを輝度の逆数 1/(1+luma) で重み付けして
// から平均するもので、極端に明るいタップの寄与を抑えてこのちらつきを消す。
// 2段目以降は既に平均済みで極端な値が残っていないため、素直な平均でよい。
//
// === ピラミッドを「ミップチェーン1枚」ではなくレベルごとの独立テクスチャで持つ理由 ===
// ミップチェーン1枚だと、あるミップをUAVで書きながら別のミップをSRVで読む形になり、
// 同一リソースのSRV/UAV同時バインド(サブリソースの重なり)になってしまう。
// Hi-Zはこれを避けるため読み書きともUAVで行っているが、その手が使えるのはR32_Floatが
// 型付きUAV読み出しを保証されている数少ないフォーマットだからで、
// ブルームで使うR11G11B10_FloatにはTypedUAVLoadAdditionalFormatsが必要になり移植性を欠く。
// レベルごとに独立したテクスチャにすれば読み書きが別リソースになり、この問題が両方とも消える
#include "Samplers.hlsli"

cbuffer BloomConstants : register(b1)
{
    // タップのオフセット計算に使う「読み出し元」のサイズ。
    // ダウンサンプル時はSourceTexture、アップサンプル時はLowerTextureのサイズを入れる
    uint2 SrcSize;
    // 書き込み先のサイズ
    uint2 DstSize;

    // しきい値とソフトニー。物理的にはブルームはレンズ内部の散乱なので全輝度に掛かるのが正しく、
    // 既定値は十分低くしてある(thresholdless寄り)。アート制御用に上げられるようにだけしてある
    float Threshold;
    float SoftKnee;
    // 1.0ならこのディスパッチが最初のダウンサンプル(Karis平均としきい値を適用する)
    float ApplyKarisAndThreshold;
    // 1.0=自動露出を使う、0.0=手動。Tonemap.hlslのUseAutoExposureと同じ意味
    float UseAutoExposure;

    // CPU側でライト強度へ事前乗算済みのEV100(プリ露出)
    float PreExposureEV100;
    // 手動露出時に掛ける倍率(Tonemap.hlslのExposureScaleと同じ値)
    float ExposureScale;
    float2 BloomPadding;
};

// ダウンサンプル: 読み出し元(1段上の解像度、または最初はSceneColor)
// アップサンプル: 同じレベルのダウンサンプル結果
Texture2D SourceTexture : register(t0);
// アップサンプル時のみ使う、1段下(低解像度側)の累積結果
Texture2D LowerTexture : register(t1);
// AutoExposure.hlslが書いた露出(texel(0,0)=EV100)。最初のダウンサンプルでしきい値を
// 適用する前に露出を反映させるために読む(ResolveExposureScale()のコメント参照)
Texture2D<float> ExposureTexture : register(t2);
RWTexture2D<float4> DestTexture : register(u0);

// SceneColorはプリ露出済み(EV100=15なら約1/39000倍)なので、そのままだとしきい値1.0が
// 事実上「何も通さない」設定になってしまう。しきい値を「表示上の白」を基準にした直感的な
// 値のままにするため、ピラミッドの入力段で露出を先に反映しておく。
// 露出はTonemap.hlslと同じ式(プリ露出EVと自動露出EVの差)で求める
float ResolveExposureScale()
{
    if (UseAutoExposure > 0.0f)
    {
        return exp2(PreExposureEV100 - ExposureTexture.Load(int3(0, 0, 0)));
    }
    // 手動露出でも1.0ではない。プリ露出EV100は時刻連動で変動するため、
    // CPU側が 2^(実効EV100 - 設定EV100) を入れてくる
    return ExposureScale;
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

// Karis平均の重み。輝度が高いタップほど軽くする
float KarisWeight(float3 color)
{
    return 1.0f / (1.0f + Luminance(color));
}

// ソフトニー付きのしきい値(Jimenez 2014)。Thresholdを境に急に切るとエッジが目立つため、
// SoftKneeの幅で二次関数的に立ち上げる
float3 ApplyThreshold(float3 color)
{
    const float brightness = max(color.r, max(color.g, color.b));
    const float knee = Threshold * SoftKnee;
    float soft = brightness - Threshold + knee;
    soft = clamp(soft, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * knee + 1e-6f);
    const float contribution = max(soft, brightness - Threshold) / max(brightness, 1e-6f);
    return color * contribution;
}

// --- ダウンサンプル: 13タップ ---
// 中央寄りの4点(間隔1テクセル)と外側の9点(間隔2テクセル)を5つの2x2ブロックにまとめて合成する
[numthreads(8, 8, 1)]
void CSDownsample(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= DstSize.x || dispatchThreadID.y >= DstSize.y)
    {
        return;
    }

    const float2 srcTexelSize = 1.0f / (float2)SrcSize;
    // 書き込み先テクセルの中心に対応する、読み出し元でのUV
    const float2 uv = ((float2)dispatchThreadID.xy + 0.5f) / (float2)DstSize;

    const float2 o1 = srcTexelSize;        // 1テクセル
    const float2 o2 = srcTexelSize * 2.0f; // 2テクセル

    const float3 a = SourceTexture.SampleLevel(ColorSampler, uv + float2(-o2.x,  o2.y), 0.0f).rgb;
    const float3 b = SourceTexture.SampleLevel(ColorSampler, uv + float2( 0.0f,  o2.y), 0.0f).rgb;
    const float3 c = SourceTexture.SampleLevel(ColorSampler, uv + float2( o2.x,  o2.y), 0.0f).rgb;
    const float3 d = SourceTexture.SampleLevel(ColorSampler, uv + float2(-o2.x,  0.0f), 0.0f).rgb;
    const float3 e = SourceTexture.SampleLevel(ColorSampler, uv,                        0.0f).rgb;
    const float3 f = SourceTexture.SampleLevel(ColorSampler, uv + float2( o2.x,  0.0f), 0.0f).rgb;
    const float3 g = SourceTexture.SampleLevel(ColorSampler, uv + float2(-o2.x, -o2.y), 0.0f).rgb;
    const float3 h = SourceTexture.SampleLevel(ColorSampler, uv + float2( 0.0f, -o2.y), 0.0f).rgb;
    const float3 i = SourceTexture.SampleLevel(ColorSampler, uv + float2( o2.x, -o2.y), 0.0f).rgb;
    const float3 j = SourceTexture.SampleLevel(ColorSampler, uv + float2(-o1.x,  o1.y), 0.0f).rgb;
    const float3 k = SourceTexture.SampleLevel(ColorSampler, uv + float2( o1.x,  o1.y), 0.0f).rgb;
    const float3 l = SourceTexture.SampleLevel(ColorSampler, uv + float2(-o1.x, -o1.y), 0.0f).rgb;
    const float3 m = SourceTexture.SampleLevel(ColorSampler, uv + float2( o1.x, -o1.y), 0.0f).rgb;

    // 5つの2x2ブロックへ分けて平均する(COD:AWのグルーピング)
    const float3 block0 = (a + b + d + e) * 0.25f; // 左上
    const float3 block1 = (b + c + e + f) * 0.25f; // 右上
    const float3 block2 = (d + e + g + h) * 0.25f; // 左下
    const float3 block3 = (e + f + h + i) * 0.25f; // 右下
    const float3 block4 = (j + k + l + m) * 0.25f; // 中央

    float3 result;
    if (ApplyKarisAndThreshold > 0.0f)
    {
        // 最初のダウンサンプルだけ、ブロックごとにKaris平均で重み付けしてファイアフライを抑える。
        // 中央ブロックの重みが最も大きい(0.5)のは元の13タップフィルタの重み配分に合わせるため
        const float w0 = KarisWeight(block0) * 0.125f;
        const float w1 = KarisWeight(block1) * 0.125f;
        const float w2 = KarisWeight(block2) * 0.125f;
        const float w3 = KarisWeight(block3) * 0.125f;
        const float w4 = KarisWeight(block4) * 0.5f;
        const float totalWeight = w0 + w1 + w2 + w3 + w4;
        result = (block0 * w0 + block1 * w1 + block2 * w2 + block3 * w3 + block4 * w4) / max(totalWeight, 1e-6f);
        // しきい値の前に露出を反映する。以降のピラミッドとTonemapでの合成はすべて
        // この「露出適用後」のスケールで揃う(Tonemap.hlslは露出を掛けたあとにブルームを混ぜる)
        result *= ResolveExposureScale();
        result = ApplyThreshold(result);
    }
    else
    {
        result = block0 * 0.125f + block1 * 0.125f + block2 * 0.125f + block3 * 0.125f + block4 * 0.5f;
    }

    DestTexture[dispatchThreadID.xy] = float4(max(result, 0.0f), 1.0f);
}

// --- アップサンプル: 1段下の累積を3x3テントで広げ、同じレベルのダウンサンプル結果へ加算する ---
[numthreads(8, 8, 1)]
void CSUpsample(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= DstSize.x || dispatchThreadID.y >= DstSize.y)
    {
        return;
    }

    // タップ間隔は読み出し元(1段下=低解像度側)のテクセルサイズで決める
    const float2 lowerTexelSize = 1.0f / (float2)SrcSize;
    const float2 uv = ((float2)dispatchThreadID.xy + 0.5f) / (float2)DstSize;
    const float2 o = lowerTexelSize;

    float3 sum = float3(0.0f, 0.0f, 0.0f);
    sum += LowerTexture.SampleLevel(ColorSampler, uv + float2(-o.x,  o.y), 0.0f).rgb * 1.0f;
    sum += LowerTexture.SampleLevel(ColorSampler, uv + float2( 0.0f, o.y), 0.0f).rgb * 2.0f;
    sum += LowerTexture.SampleLevel(ColorSampler, uv + float2( o.x,  o.y), 0.0f).rgb * 1.0f;
    sum += LowerTexture.SampleLevel(ColorSampler, uv + float2(-o.x,  0.0f), 0.0f).rgb * 2.0f;
    sum += LowerTexture.SampleLevel(ColorSampler, uv,                       0.0f).rgb * 4.0f;
    sum += LowerTexture.SampleLevel(ColorSampler, uv + float2( o.x,  0.0f), 0.0f).rgb * 2.0f;
    sum += LowerTexture.SampleLevel(ColorSampler, uv + float2(-o.x, -o.y), 0.0f).rgb * 1.0f;
    sum += LowerTexture.SampleLevel(ColorSampler, uv + float2( 0.0f,-o.y), 0.0f).rgb * 2.0f;
    sum += LowerTexture.SampleLevel(ColorSampler, uv + float2( o.x, -o.y), 0.0f).rgb * 1.0f;
    sum *= (1.0f / 16.0f);

    const float3 sameLevel = SourceTexture.SampleLevel(ColorSampler, uv, 0.0f).rgb;
    DestTexture[dispatchThreadID.xy] = float4(max(sameLevel + sum, 0.0f), 1.0f);
}
