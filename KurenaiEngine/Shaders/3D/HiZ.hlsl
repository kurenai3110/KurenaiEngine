// Hi-Zミップチェーン構築パス。GBufferの深度バッファ(単一ミップ)から、コンピュートシェーダーで
// 1x1まで縮小するミップチェーンを構築する。用途はまだ無い(将来のオクルージョンカリングや
// SSRのレイマーチング高速化向けの土台)ため、現時点ではミップチェーンの生成のみを行う。
//
// CSCopy: GBuffer深度(単一ミップのTexture2D)をHi-Zチェーンのミップ0(RWTexture2D)へそのままコピーする。
// CSDownsample: Hi-Zチェーンの隣接する2ミップ(いずれもRWTexture2Dとしてバインド)を使い、
// ミップNの2x2ブロックからミップN+1の1テクセルを求める。奇数解像度では右端・下端のブロックが
// 実質1x1/1x2/2x1になるため、はみ出す座標はソース側の最終行・列にクランプして常に4テクセル分読む。
//
// ダウンサンプルは「最小値」を採用する。このエンジンはReverse-Z(近平面=1.0/遠平面=0.0、
// 深度比較はGREATER)を使っており(RHIDesc.hのPipelineStateDesc::ReverseZ参照)、あるブロックの
// 最小深度値はそのブロック内で「最も遠い(最も奥にある)可視サーフェス」を表す。オクルージョン
// カリングでは、候補オブジェクトの最も近い点でさえこの値より奥(小さい)であれば、ブロック内の
// どのピクセルにも遮蔽されていることが保証できる。これは非Reverse-Zで最大値を取る一般的な
// Hi-Zカリング手法を、Reverse-Zの向きに合わせて反転させたものにあたる。

cbuffer HiZConstants : register(b0)
{
    uint2 SrcSize;
    uint2 DstSize;
};

Texture2D<float> SrcDepth : register(t0);
RWTexture2D<float> DstMip0 : register(u0);

[numthreads(8, 8, 1)]
void CSCopy(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= DstSize.x || dispatchThreadID.y >= DstSize.y)
    {
        return;
    }

    DstMip0[dispatchThreadID.xy] = SrcDepth.Load(int3(dispatchThreadID.xy, 0));
}

RWTexture2D<float> SrcMip : register(u0);
RWTexture2D<float> DstMip : register(u1);

[numthreads(8, 8, 1)]
void CSDownsample(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= DstSize.x || dispatchThreadID.y >= DstSize.y)
    {
        return;
    }

    const uint2 srcMaxCoord = SrcSize - uint2(1, 1);
    const uint2 base = dispatchThreadID.xy * 2;

    const float d00 = SrcMip[min(base + uint2(0, 0), srcMaxCoord)];
    const float d10 = SrcMip[min(base + uint2(1, 0), srcMaxCoord)];
    const float d01 = SrcMip[min(base + uint2(0, 1), srcMaxCoord)];
    const float d11 = SrcMip[min(base + uint2(1, 1), srcMaxCoord)];

    DstMip[dispatchThreadID.xy] = min(min(d00, d10), min(d01, d11));
}
