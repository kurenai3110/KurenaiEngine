// G-Bufferの法線をオクタヘドラル図法でエンコード/デコードする共通関数。
// 格納フォーマットはR16G16_Float(浮動小数点)のため、従来のR8G8B8A8(0〜1へ再マップして格納)と異なり
// [-1,1]の符号付き値をそのまま格納できる。低ラフネスの鏡面ハイライトのバンディングを抑えるため、
// チャンネル数を4→2に減らしつつビット深度を8bit→16bitへ増やす目的で導入した
// (参考: Cigolle et al., "A Survey of Efficient Representations for Independent Unit Vectors", 2014)

float2 OctEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
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
