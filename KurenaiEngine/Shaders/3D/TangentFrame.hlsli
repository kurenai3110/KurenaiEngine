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
    // 頂点補間でTとNの直交性が崩れるため、ピクセル単位でGram-Schmidt再直交化する
    float3 T = normalize(tangent.xyz - N * dot(N, tangent.xyz));
    float3 B = cross(N, T) * tangent.w;
    return float3x3(T, B, N);
}

#endif // KURENAI_TANGENT_FRAME_HLSLI
