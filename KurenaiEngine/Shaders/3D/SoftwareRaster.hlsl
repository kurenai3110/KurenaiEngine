// コンピュートシェーダーによる自前ソフトウェアラスタライザの被覆判定パス。
//
//   CSRaster      1スレッド = 1三角形。小さい三角形を自分でラスタライズし、
//                 大きすぎるものは巨大三角形リストへ登録するだけで抜ける
//   CSRasterLarge 1スレッドグループ(256スレッド) = 巨大三角形1個。
//                 グループ数がGPU上でしか分からないため間接ディスパッチで発行する
//
// どちらもvisibility buffer(深度と三角形番号を詰めた64bit値)へ書くだけで、色は作らない。
// そこからジオメトリを引き直して深度・法線・陰影を書き出すのはSoftwareRasterResolve.hlsl。
//
// 【Resolveを別ファイルにしている理由】HLSLのレジスタ割り当てはファイル単位で行われ、
// 同じファイル内の別エントリポイントで同じレジスタを使い回すことはできない。
// このエンジンのコンピュート用UAVスロットはu0〜u3の4本しかないため、
// 被覆判定側(u0〜u2)とResolve側(u0〜u3)を1ファイルに同居させると枠が足りなくなる。
//
// 【bindlessが無い構成でもコンパイルは通す】shader-checkはDX12経路を
// KURENAI_BINDLESS の定義あり・なしの2通りでコンパイルする。定義なしでは
// KURENAI_BINDLESS_BUFFER が存在しない(Bindless.hlsli参照)ため、本体を
// #if で囲って空の関数へ縮退させる。この経路は実行されない
// (IRHIDevice::SupportsSoftwareRaster()がbindlessを必須にしている)。

#include "SoftwareRasterCommon.hlsli"

// C++側のディスパッチ数計算(KurenaiEngine3D.cpp)と一致させること
#define KURENAI_SWRASTER_GROUP_SIZE 64
#define KURENAI_SWRASTER_LARGE_GROUP_SIZE 256

StructuredBuffer<SWRasterMeshInfo> MeshInfos : register(t0);
// CSRasterLargeのみ: CSRasterが登録した巨大三角形の通し番号
StructuredBuffer<uint> LargeEntriesRead : register(t1);

// 深度と三角形番号を詰めた64bit値(SWRasterPackVisibility)。画素ごとに1要素
RWStructuredBuffer<uint64_t> Visibility : register(u0);
// CSRasterのみ: 巨大三角形の通し番号を積むリスト
RWStructuredBuffer<uint> LargeEntries : register(u1);
// CSRasterのみ: 間接ディスパッチ引数。X成分をそのままカウンタとして使う
RWByteAddressBuffer IndirectArgs : register(u2);

#if defined(KURENAI_BINDLESS)

// 1つの画素へ深度テスト付きで書き込む。
// 判定と書き込みを1つのアトミックにまとめてあるため競合しない(理由は
// SoftwareRasterCommon.hlsliのSWRasterPackVisibilityのコメント)
void SWRasterWritePixel(int2 pixel, float depthNdc, uint triangleIndex)
{
    // クリア値0(=遠平面=当たり無し)と区別がつかなくなるため、z=0ちょうどは書き込まない
    if (depthNdc <= 0.0f)
    {
        return;
    }

    const uint pixelIndex = (uint)pixel.y * (uint)RenderSize.x + (uint)pixel.x;
    InterlockedMax(Visibility[pixelIndex], SWRasterPackVisibility(depthNdc, triangleIndex));
}

// --- CSRaster: 1スレッド = 1三角形 -----------------------------------------------------

[numthreads(KURENAI_SWRASTER_GROUP_SIZE, 1, 1)]
void CSRaster(uint3 groupId : SV_GroupID, uint groupThreadId : SV_GroupIndex)
{
    // Dispatchの1次元あたりの上限は65535なので、C++側で2次元へ分解してある。
    // DispatchParams.x(X方向のグループ数)から線形なグループ番号を復元する
    const uint linearGroup = groupId.y * DispatchParams.x + groupId.x;
    const uint triangleIndex = linearGroup * KURENAI_SWRASTER_GROUP_SIZE + groupThreadId;

    if (triangleIndex >= DispatchParams.y)
    {
        return;
    }

    const uint meshIndex = SWRasterFindMesh(MeshInfos, DispatchParams.z, triangleIndex);
    const SWRasterTriangle tri = SWRasterFetchTriangle(MeshInfos, meshIndex, triangleIndex);
    if (!tri.Valid)
    {
        return;
    }

    const int4 bounds = SWRasterScreenBounds(tri);
    if (bounds.x > bounds.z || bounds.y > bounds.w)
    {
        // 画面外
        return;
    }

    // 【巨大三角形を別パスへ逃がす】1スレッド1三角形方式の唯一の弱点は、画面全体を覆う
    // 三角形を1スレッドが200万回ループしてTDRを起こすこと。しきい値を超えたものは
    // 自分ではラスタライズせず、リストへ登録するだけで抜ける。これで小三角形パスの
    // 1スレッドあたりの仕事量は必ずしきい値以下に収まる
    const uint bboxArea = (uint)(bounds.z - bounds.x + 1) * (uint)(bounds.w - bounds.y + 1);
    if (bboxArea > DispatchParams.w)
    {
        // 引数バッファのX成分(スレッドグループ数X)をそのままカウンタとして使う。
        // 別のカウンタバッファも集計用ディスパッチも要らない。
        // 【容量で打ち切らない】溢れをResolve側で検出できるよう、登録はしないが数は数える
        uint slot;
        IndirectArgs.InterlockedAdd(0, 1, slot);
        if (slot < LargeParams.x)
        {
            LargeEntries[slot] = triangleIndex;
        }
        // Y=Z=1は何度書いても同じ値なので、競合を気にせず全スレッドが書いてよい
        IndirectArgs.Store(4, 1);
        IndirectArgs.Store(8, 1);
        return;
    }

    for (int y = bounds.y; y <= bounds.w; ++y)
    {
        for (int x = bounds.x; x <= bounds.z; ++x)
        {
            const float2 pixelCenter = float2((float)x + 0.5f, (float)y + 0.5f);
            const float3 bary = SWRasterBarycentric(tri, pixelCenter);
            if (!SWRasterInside(bary))
            {
                continue;
            }

            SWRasterWritePixel(int2(x, y), SWRasterInterpolateDepth(tri, bary), triangleIndex);
        }
    }
}

// --- CSRasterLarge: 1スレッドグループ = 巨大三角形1個 ----------------------------------

[numthreads(KURENAI_SWRASTER_LARGE_GROUP_SIZE, 1, 1)]
void CSRasterLarge(uint3 groupId : SV_GroupID, uint groupThreadId : SV_GroupIndex)
{
    // 間接ディスパッチのグループ数 = 登録を試みた巨大三角形の個数。
    // カウンタは容量で打ち切っていないため、容量を超えたグループはリストの範囲外を指す
    const uint entryIndex = groupId.x;
    if (entryIndex >= LargeParams.x)
    {
        return;
    }

    const uint triangleIndex = LargeEntriesRead[entryIndex];

    const uint meshIndex = SWRasterFindMesh(MeshInfos, DispatchParams.z, triangleIndex);
    const SWRasterTriangle tri = SWRasterFetchTriangle(MeshInfos, meshIndex, triangleIndex);
    if (!tri.Valid)
    {
        return;
    }

    const int4 bounds = SWRasterScreenBounds(tri);
    if (bounds.x > bounds.z || bounds.y > bounds.w)
    {
        return;
    }

    const uint width = (uint)(bounds.z - bounds.x + 1);
    const uint height = (uint)(bounds.w - bounds.y + 1);
    const uint pixelCount = width * height;

    // 256スレッドでbboxをストライドしながら分担する。画面全体を覆う三角形でも
    // 1スレッドあたり 2,073,600 / 256 ≒ 8100 反復で有界になる
    for (uint i = groupThreadId; i < pixelCount; i += KURENAI_SWRASTER_LARGE_GROUP_SIZE)
    {
        const int x = bounds.x + (int)(i % width);
        const int y = bounds.y + (int)(i / width);

        const float2 pixelCenter = float2((float)x + 0.5f, (float)y + 0.5f);
        const float3 bary = SWRasterBarycentric(tri, pixelCenter);
        if (!SWRasterInside(bary))
        {
            continue;
        }

        SWRasterWritePixel(int2(x, y), SWRasterInterpolateDepth(tri, bary), triangleIndex);
    }
}

#else // KURENAI_BINDLESS

// bindless非対応構成向けの縮退。この経路は実行されない(ファイル冒頭のコメント参照)が、
// shader-checkがこの構成でもコンパイルするためエントリポイントだけ用意する

[numthreads(KURENAI_SWRASTER_GROUP_SIZE, 1, 1)]
void CSRaster(uint3 groupId : SV_GroupID, uint groupThreadId : SV_GroupIndex)
{
}

[numthreads(KURENAI_SWRASTER_LARGE_GROUP_SIZE, 1, 1)]
void CSRasterLarge(uint3 groupId : SV_GroupID, uint groupThreadId : SV_GroupIndex)
{
}

#endif // KURENAI_BINDLESS
