// Hi-Zミップチェーン構築パス。深度バッファ(単一ミップ)から、コンピュートシェーダーで
// 1x1まで縮小するミップチェーンを構築する。消費者はオクルージョンカリング
// (HiZCull.hlsliのIsAabbOccludedByHiZ)と、デバッグ表示。
//
// CSCopy: 深度(単一ミップのTexture2D)をHi-Zチェーンのミップ0(RWTexture2D)へそのままコピーする。
// CSDownsample: Hi-Zチェーンの隣接する2ミップ(いずれもRWTexture2Dとしてバインド)を使い、
// ミップNの2x2ブロックからミップN+1の1テクセルを求める。
//
// 【守るべき不変条件: ミップNのテクセル群はミップ0の画像を隙間なく覆う】
// 読む側(HiZCull.hlsli)は「ミップ0の座標を mip だけ右シフトしたものが、そこを担当する
// テクセルの番号」という前提で引く。ミップの寸法は max(1, 親/2) の**切り捨て**なので、
// 親の辺が奇数のときは 2x2 だけでは最終行・列がどのテクセルからも読まれず、この前提が崩れる。
//
//   例) 親が45行なら子は22行。子の最終テクセル(21)が読むのは親の 42,43 までで、44 が落ちる。
//
// **落ちる向きは「見えているものを消す」側。** minが1つ欠けるとHi-Zの値は本来より大きくなり、
// 遮蔽判定 `maxNdcZ < hiZMin` が成立しやすくなる。
// そこで**辺が奇数のときは最終テクセルだけが端数の1行(列)も読む**(最悪3x3の9タップ)。
// 最終テクセルが残りを全部吸うので、読む側は「はみ出したら最終テクセルへクランプ」で正しくなる。
//
// 常に3x3をクランプ付きで読む書き方でも保守側ではあるが、全テクセルが1行ぶん重なって
// ピラミッド全体が緩くなり、本来間引ける塊を取りこぼす。だから条件付きにしてある。
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

    float result = min(min(d00, d10), min(d01, d11));

    // 端数の吸収(冒頭の不変条件を参照)。ソースの辺が奇数のとき、最終テクセルだけが
    // 1行(列)を取りこぼす。DstSizeは切り捨てなので、取りこぼすのは常に最終テクセルだけ。
    // クランプは SrcSize が 1 の段でのみ実際に効く(そこでは重複して読むだけで害は無い)
    const bool2 oddSrc = (SrcSize & 1u) != 0u;
    const bool2 lastDst = dispatchThreadID.xy == (DstSize - uint2(1, 1));
    const bool takeExtraX = oddSrc.x && lastDst.x;
    const bool takeExtraY = oddSrc.y && lastDst.y;

    if (takeExtraX)
    {
        result = min(result, SrcMip[min(base + uint2(2, 0), srcMaxCoord)]);
        result = min(result, SrcMip[min(base + uint2(2, 1), srcMaxCoord)]);
    }
    if (takeExtraY)
    {
        result = min(result, SrcMip[min(base + uint2(0, 2), srcMaxCoord)]);
        result = min(result, SrcMip[min(base + uint2(1, 2), srcMaxCoord)]);
    }
    if (takeExtraX && takeExtraY)
    {
        result = min(result, SrcMip[min(base + uint2(2, 2), srcMaxCoord)]);
    }

    DstMip[dispatchThreadID.xy] = result;
}
