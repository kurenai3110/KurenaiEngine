// 自前ソフトウェアラスタライザのResolveパス。1スレッド = 1画素。
//
// SoftwareRaster.hlslが書いたvisibility buffer(深度と三角形番号を詰めた64bit値)から
// 三角形番号を取り出し、そのジオメトリを引き直して再変換することで、
// スクリーン座標・invW・重心座標を復元して深度・法線・陰影を書き出す。
//
// 【中間の三角形レコードを持たない】頂点3個の変換は数十FLOPしかないため、
// ラスタライズ時の変換結果を保存しておくより引き直す方が安い。
// Bistro級で48〜136MBになる中間レコードバッファを丸ごと省ける。
//
// 【被覆判定と別ファイルにしている理由】SoftwareRaster.hlsl冒頭のコメント参照
// (コンピュート用UAVスロットがu0〜u3の4本しかないため)。

#include "NormalEncoding.hlsli"
#include "SoftwareRasterCommon.hlsli"

#define KURENAI_SWRASTER_RESOLVE_GROUP_SIZE 8

StructuredBuffer<SWRasterMeshInfo> MeshInfos : register(t0);
StructuredBuffer<uint64_t> VisibilityRead : register(t1);

RWTexture2D<float4> OutColor : register(u0);
RWTexture2D<float> OutDepth : register(u1);
RWTexture2D<float2> OutNormal : register(u2);
// 巨大三角形リストの溢れを検出するためだけに読む。この引数バッファはUAVしか持たないため
// SRVではなくUAVスロットへ張る(書き込みはしない)
RWByteAddressBuffer IndirectArgsRead : register(u3);

#if defined(KURENAI_BINDLESS)

[numthreads(KURENAI_SWRASTER_RESOLVE_GROUP_SIZE, KURENAI_SWRASTER_RESOLVE_GROUP_SIZE, 1)]
void CSResolve(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= (uint)RenderSize.x || pixel.y >= (uint)RenderSize.y)
    {
        return;
    }

    // 【溢れの可視化】CSRasterはリスト容量を超えた分を登録せずに落とすが、カウンタ自体は
    // 打ち切らずに増やしている。超過していたら画面左上を塗って気づけるようにする
    // (引数バッファをCPUへ読み戻す経路が無いためログには出せない)
    const uint requestedLargeCount = IndirectArgsRead.Load(0);
    if (requestedLargeCount > LargeParams.x && pixel.x < 32 && pixel.y < 32)
    {
        OutColor[pixel] = float4(1.0f, 0.0f, 1.0f, 1.0f);
        OutDepth[pixel] = 0.0f;
        OutNormal[pixel] = float2(0.0f, 0.0f);
        return;
    }

    const uint pixelIndex = pixel.y * (uint)RenderSize.x + pixel.x;
    const uint64_t packed = VisibilityRead[pixelIndex];

    const uint depthBits = (uint)(packed >> 32);
    if (depthBits == 0)
    {
        // 当たり無し(ジオメトリが1つも被覆しなかった画素)。
        // Reverse-Zの遠平面と同じ0を書く
        OutColor[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        OutDepth[pixel] = 0.0f;
        OutNormal[pixel] = float2(0.0f, 0.0f);
        return;
    }

    const float depthNdc = asfloat(depthBits);
    const uint triangleIndex = (uint)(packed & 0xFFFFFFFFull);

    // 三角形番号からジオメトリを引き直して再変換する。ラスタライズ時とまったく同じ
    // 変換を通るため、重心座標も一致する
    const uint meshIndex = SWRasterFindMesh(MeshInfos, DispatchParams.z, triangleIndex);
    const SWRasterTriangle tri = SWRasterFetchTriangle(MeshInfos, meshIndex, triangleIndex);
    if (!tri.Valid)
    {
        // ここへ来るのはラスタライズ時と判定が食い違った場合だけで、本来起きない。
        // 深度だけは書いておき、法線と色は当たり無しと同じ扱いにする
        OutColor[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        OutDepth[pixel] = depthNdc;
        OutNormal[pixel] = float2(0.0f, 0.0f);
        return;
    }

    const float2 pixelCenter = float2((float)pixel.x + 0.5f, (float)pixel.y + 0.5f);
    const float3 bary = SWRasterBarycentric(tri, pixelCenter);
    const float3 weights = SWRasterPerspectiveWeights(tri, bary);

    float3 normal =
        weights.x * tri.V[0].Normal + weights.y * tri.V[1].Normal + weights.z * tri.V[2].Normal;
    const float normalLength = length(normal);
    // 長さ0の法線(データ不正や補間による打ち消し)をnormalizeするとNaNが流れ、
    // 原因の分かりにくい黒画面になる。カメラを向いた既定値へ落として先へ進める
    normal = (normalLength > 1e-6f) ? (normal / normalLength) : float3(0.0f, 0.0f, -1.0f);

    // 【フェーズ1の陰影】マテリアルテクスチャは引かないため、定数アルベドにランバート項を
    // 掛けただけの見た目にする。物理単位の太陽輝度を使わないのは、露出設定に左右されず
    // いつでも同じ明るさで形を確認できるようにするため。ハードウェアと突き合わせる対象は
    // 色ではなく深度(Present Mode 5)と法線(Present Mode 7)のバッファ
    const float ndotl = saturate(dot(normal, -SunDirection.xyz));
    const float shade = (ndotl * 0.8f + 0.2f) * 0.7f;
    OutColor[pixel] = float4(shade, shade, shade, 1.0f);

    OutDepth[pixel] = depthNdc;
    // ハードウェアのG-Buffer法線とまったく同じ符号化(R16G16_Floatのオクタヘドラル)にして、
    // Present.hlslのMode 7で並べて差分が取れるようにする
    OutNormal[pixel] = OctEncode(normal);
}

#else // KURENAI_BINDLESS

// bindless非対応構成向けの縮退(SoftwareRaster.hlslと同じ理由)
[numthreads(KURENAI_SWRASTER_RESOLVE_GROUP_SIZE, KURENAI_SWRASTER_RESOLVE_GROUP_SIZE, 1)]
void CSResolve(uint3 dispatchThreadId : SV_DispatchThreadID)
{
}

#endif // KURENAI_BINDLESS
