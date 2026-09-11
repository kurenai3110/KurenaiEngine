// G-Bufferの法線をオクタヘドラル図法でエンコード/デコードする共通関数。
// 格納フォーマットはR16G16_Float(浮動小数点)のため、従来のR8G8B8A8(0〜1へ再マップして格納)と異なり
// [-1,1]の符号付き値をそのまま格納できる。低ラフネスの鏡面ハイライトのバンディングを抑えるため、
// チャンネル数を4→2に減らしつつビット深度を8bit→16bitへ増やす目的で導入した
// (参考: Cigolle et al., "A Survey of Efficient Representations for Independent Unit Vectors", 2014)

#ifndef KURENAI_NORMAL_ENCODING_HLSLI
#define KURENAI_NORMAL_ENCODING_HLSLI

float2 OctEncode(float3 n)
{
    // 【0除算を塞ぐ】nは単位ベクトルの前提なのでL1ノルムは1〜√3に収まり、この下限は
    // 正しい入力では一度も効かない(効かせないための下限であって、丸めるための下限ではない)。
    // 効くのは呼び出し側が長さ0やNaNを渡したときで、そこで 0/0 のNaNを新しく作らずに済む。
    // **NaNをここで止められるわけではない** ―― 入ってきたNaNはそのまま出る。
    // 入口でNaNを作らないようにするのはTangentFrame.hlsliのComputeTangentFrame側の仕事
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-8f);
    if (n.z < 0.0f)
    {
        float2 signNotZero = float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
        n.xy = (1.0f - abs(n.yx)) * signNotZero;
    }
    return n.xy;
}

float3 OctDecode(float2 f)
{
    float3 n = float3(f.x, f.y, 1.0f - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.x += n.x >= 0.0f ? -t : t;
    n.y += n.y >= 0.0f ? -t : t;
    return normalize(n);
}

#endif
