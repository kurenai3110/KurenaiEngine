// 頂点接線からTBN行列を組む共通関数。G-Buffer・反射プローブのキャプチャ・平面反射・
// 半透明フォワードの4つが同じ式を使うため、写さずにここへ集める
// (NormalEncoding.hlsliのOctEncodeをCPU/GPUで揃えているのと同じ考え方)。
//
// このヘッダーはリソースも定数バッファも参照しないので、どの段からでも#includeしてよい。

#ifndef KURENAI_TANGENT_FRAME_HLSLI
#define KURENAI_TANGENT_FRAME_HLSLI

// 頂点接線(xyz)と従法線の向き(w = +1/-1)からTBN行列を構築する。
// UV/位置の画面空間微分(ddx/ddy)から近似する手法は、UV継ぎ目(シームがある円筒状展開の
// グラス類など)でピクセルクアッドがトポロジー的に不連続になり法線が破綻するため使用しない。
float3x3 ComputeTangentFrame(float3 N, float4 tangent)
{
    // 頂点補間でTとNの直交性が崩れるため、ピクセル単位でGram-Schmidt再直交化する。
    //
    // 【退化に備える】頂点接線そのものはパッカーが非退化にしているが(ModelSource.cppが
    // 縮退時にNへ直交する軸へ差し替える)、**補間の結果がNと平行になることは防げない。**
    // そうなると残差は長さ0になり、normalizeは 0/0 = NaN を返す。NaNはOctEncodeの
    // 割り算も素通りしてG-Bufferの法線に残り、fp16でも生き残って下流のライティングへ伝わる
    // (実測でPenumbraH4の数十画素がNaNになることを、接線をNと平行に潰す陽性対照で確認した)。
    // 同じ式をCPU側へ写しているOcclusionBaker.cppには以前から同じガードが入っている。
    float3 rawT = tangent.xyz - N * dot(N, tangent.xyz);
    float3 T;
    if (dot(rawT, rawT) > 1e-12f)
    {
        T = normalize(rawT);
    }
    else
    {
        // 退化したときはNに直交する任意の軸を選ぶ。**接空間の向きは復元できない**ので、
        // 法線マップを持つ面ではその面の凹凸の向きが不定になる ―― ただしNaNを流すよりは良く、
        // 法線マップを持たない面(normalSampleが(0,0,1))では結果はNのままで変わらない。
        // Nの成分が最も小さい軸を種に取ると、crossが退化しない
        float3 seed = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
        T = normalize(cross(seed, N));
    }
    float3 B = cross(N, T) * tangent.w;
    return float3x3(T, B, N);
}

#endif // KURENAI_TANGENT_FRAME_HLSLI
