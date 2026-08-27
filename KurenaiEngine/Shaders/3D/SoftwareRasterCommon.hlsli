// コンピュートシェーダーによる自前ソフトウェアラスタライザの共有ヘッダー。
//
// 【何をしているのか】ハードウェアラスタライザがブラックボックスで行っている処理
// ―― 頂点変換・背面カリング・スクリーン空間への投影・エッジ関数による被覆判定・
// 深度テスト・透視補正補間 ―― を明示的なコードとして書き下し、
// ハードウェアの結果(G-Buffer)と直接突き合わせられるようにするためのもの。
// 既存のG-Buffer経路の代わりではなく、比較用の独立したパスとして動く。
//
// 【bindlessが前提】三角形1つにつき、所属メッシュの頂点バッファとインデックスバッファを
// ResourceDescriptorHeap経由で引く。デバイス側の判定(DX12Device::DetectSoftwareRasterSupport)が
// bindlessと64bit整数アトミックの両方を要求するため、非対応環境ではパス自体が実行されない。
//
// 【深度はReverse-Z】近平面がNDC z=1.0、遠平面がz=0.0。
// 「深度値が大きいほど手前」であり、最も手前を取るならmaxを使う。

#ifndef KURENAI_SOFTWARE_RASTER_COMMON_HLSLI
#define KURENAI_SOFTWARE_RASTER_COMMON_HLSLI

#include "Bindless.hlsli"

// KurenaiEngine3D.cppの匿名名前空間にあるSWRasterConstantsと1対1で対応。
// 並びとサイズを一致させること
cbuffer SWRasterConstants : register(b1)
{
    // ワールド→クリップ空間。TAAのジッターを含む(m_TAAEnabledがfalseならジッターは0)。
    // ハードウェアのG-Bufferパスとまったく同じ行列を渡すことで、深度の比較が意味を持つ
    float4x4 ViewProj;
    // xy=レンダー解像度(画素)、zw=その逆数
    float4 RenderSize;
    // xyz=太陽光が進む向き(正規化済み)、w=未使用。陰影付けの見た目確認にだけ使う
    float4 SunDirection;
    // x=CSRasterのディスパッチをX方向へ何グループに分けたか(2D分解の復元に使う)
    // y=シーン全体の三角形数、z=メッシュレコード数、w=巨大三角形とみなすbbox画素面積のしきい値
    uint4 DispatchParams;
    // x=巨大三角形リストの容量、yzw=未使用
    uint4 LargeParams;
};

// KurenaiEngine3D.cppのSWRasterMeshInfo(160バイト)と1対1で対応。
//
// 【構造化バッファは詰めて並ぶ】定数バッファと違い、StructuredBuffer<T>のTは
// C++と同じ「メンバの型のアラインメントに従った詰めた配置」になる
// (GBufferMeshlet.hlslのMeshVertexのコメントと同じ話)
struct SWRasterMeshInfo
{
    float4x4 World;
    float4x4 NormalMatrix;
    // 頂点バッファ(StructuredBuffer<SWRasterVertex>)とインデックスバッファ
    // (StructuredBuffer<uint>)のbindless番号
    uint VertexBufferIndex;
    uint IndexBufferIndex;
    // シーン全体の通し三角形番号における、このメッシュの先頭。二分探索のキー
    uint FirstTriangle;
    uint TriangleCount;
    // ミラーリングされたインスタンス(Worldの行列式が負)なら-1。
    // 表裏判定の符号を反転させる。これが無いと鏡像配置のモデルだけ裏返って消える
    float FrontFaceSign;
    uint Flags;
    uint2 Padding;
};

// Assets::Vertex(56バイト)と1対1で対応。GBufferMeshlet.hlslのMeshVertexと同じ宣言で、
// レイアウトを変えるときは両方を直すこと(共通ヘッダーに置いていないのは、
// GBufferMeshlet.hlsl側が増幅シェーダー用のgroupsharedを持つ独立したファイルのため)
struct SWRasterVertex
{
    float3 Position;
    float3 Normal;
    float2 UV;
    float4 Tangent;
    float2 UV1;
};

// クリップ空間のwがこれ以下の頂点を持つ三角形は棄却する。
// 近平面クリッピングを実装していないため、カメラの手前や背後へ回り込んだ三角形は
// 描かれずに消える(壁に近づくと穴が開く)。フェーズ2で解消する
static const float kSWRasterMinClipW = 1e-4f;

// スクリーン空間へ投影した1頂点
struct SWRasterProjected
{
    // xy=スクリーン座標(画素、原点は左上・Y下向き)、z=NDCの深度(Reverse-Z)
    float3 Screen;
    // 1/w。透視補正補間に使う
    float InvW;
    float3 Normal;
    float2 UV;
};

// 投影済みの三角形3頂点と、スクリーン空間での符号付き面積
struct SWRasterTriangle
{
    SWRasterProjected V[3];
    // 2倍の符号付き面積(エッジ関数の和)。Y下向きのスクリーン座標系で計算しているため、
    // 正 = 画面上で時計回り = D3DのFrontCounterClockwise=FALSEにおける表面
    float Area2;
    bool Valid;
};

// エッジ関数。(a→b)の直線に対して点cがどちら側にあるか。
// 3辺すべてで符号が揃えば点は三角形の内側にある。3つの和が三角形の2倍面積になる
float SWRasterEdge(float2 a, float2 b, float2 c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// シーン全体の通し三角形番号から所属メッシュのレコード番号を引く。
// MeshInfo[].FirstTriangleは単調増加なので二分探索でよい
// (専用の逆引きテーブルは三角形数×4バイト = Bistroで11MBになるため持たない)
uint SWRasterFindMesh(StructuredBuffer<SWRasterMeshInfo> meshInfos, uint meshCount, uint triangleIndex)
{
    uint lo = 0;
    uint hi = meshCount - 1;
    while (lo < hi)
    {
        // 「FirstTriangle <= triangleIndex を満たす最大の要素」を探す。
        // 上側へ寄せた中点を取らないと lo == hi - 1 で無限ループになる
        const uint mid = (lo + hi + 1) / 2;
        if (meshInfos[mid].FirstTriangle <= triangleIndex)
        {
            lo = mid;
        }
        else
        {
            hi = mid - 1;
        }
    }
    return lo;
}

// 1つの三角形をワールド変換してスクリーン空間へ投影する。
// 背面・退化・近平面より手前の三角形はValid=falseで返る。
//
// 【bindless専用】頂点とインデックスをResourceDescriptorHeapから引くため、
// KURENAI_BINDLESSが定義されていない構成ではこの関数自体が存在しない。
// 呼び出し側(SoftwareRaster.hlsl / SoftwareRasterResolve.hlsl)も同じマクロで
// 本体を切り替えているため、非対応構成でも未定義参照にはならない
#if defined(KURENAI_BINDLESS)
SWRasterTriangle SWRasterFetchTriangle(
    StructuredBuffer<SWRasterMeshInfo> meshInfos, uint meshIndex, uint triangleIndex)
{
    SWRasterTriangle tri = (SWRasterTriangle)0;
    tri.Valid = false;

    const SWRasterMeshInfo mesh = meshInfos[meshIndex];

    StructuredBuffer<SWRasterVertex> vertices = KURENAI_BINDLESS_BUFFER(mesh.VertexBufferIndex);
    StructuredBuffer<uint> indices = KURENAI_BINDLESS_BUFFER(mesh.IndexBufferIndex);

    // メッシュ内での三角形番号 → インデックスバッファの位置
    const uint localTriangle = triangleIndex - mesh.FirstTriangle;
    const uint indexBase = localTriangle * 3;

    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        const SWRasterVertex v = vertices[indices[indexBase + i]];

        const float3 worldPos = mul(float4(v.Position, 1.0f), mesh.World).xyz;
        const float4 clip = mul(float4(worldPos, 1.0f), ViewProj);

        // 近平面クリッピング未実装。wが0以下の頂点はNDCへ落とせず、そのまま割ると
        // 符号が反転して画面上のあらぬ位置へ飛ぶため、三角形ごと棄却する
        if (clip.w <= kSWRasterMinClipW)
        {
            return tri;
        }

        const float invW = 1.0f / clip.w;
        const float3 ndc = clip.xyz * invW;

        SWRasterProjected p;
        // NDC(X右・Y上、[-1,1])からスクリーン座標(X右・Y下、画素)へ。
        // Yの符号反転を忘れても絵は出るが、上下が逆のまま静かに間違う
        p.Screen = float3(
            (ndc.x * 0.5f + 0.5f) * RenderSize.x,
            (0.5f - ndc.y * 0.5f) * RenderSize.y,
            ndc.z);
        p.InvW = invW;
        p.Normal = mul(v.Normal, (float3x3)mesh.NormalMatrix);
        p.UV = v.UV;

        tri.V[i] = p;
    }

    tri.Area2 = SWRasterEdge(tri.V[0].Screen.xy, tri.V[1].Screen.xy, tri.V[2].Screen.xy);

    // 背面カリング。ミラーリングされたインスタンスは表裏の判定が入れ替わる
    // (ハードウェア経路がFrontCounterClockwise=trueの別PSOで描いているのと同じ対処)
    if (tri.Area2 * mesh.FrontFaceSign <= 0.0f)
    {
        return tri;
    }

    // 退化三角形(面積0)は重心座標が求まらない
    if (abs(tri.Area2) < 1e-9f)
    {
        return tri;
    }

    tri.Valid = true;
    return tri;
}
#endif // KURENAI_BINDLESS

// 投影済み三角形のスクリーン空間バウンディングボックス(画素、両端を含む)。
// 戻り値 xy=最小、zw=最大。画面外へ完全に出ている場合は最小が最大を上回る
int4 SWRasterScreenBounds(SWRasterTriangle tri)
{
    const float2 lo = min(tri.V[0].Screen.xy, min(tri.V[1].Screen.xy, tri.V[2].Screen.xy));
    const float2 hi = max(tri.V[0].Screen.xy, max(tri.V[1].Screen.xy, tri.V[2].Screen.xy));

    // 判定は画素中心(x+0.5, y+0.5)で行うため、中心座標に直してから床/天井を取る
    int4 bounds;
    bounds.x = max((int)floor(lo.x - 0.5f), 0);
    bounds.y = max((int)floor(lo.y - 0.5f), 0);
    bounds.z = min((int)ceil(hi.x - 0.5f), (int)RenderSize.x - 1);
    bounds.w = min((int)ceil(hi.y - 0.5f), (int)RenderSize.y - 1);
    return bounds;
}

// 画素中心における重心座標。面積で割ってあるため、3成分すべてが0以上なら内側
// (面積の向きに依らず符号が揃う)。
//
// 【フィルルール】D3Dのtop-leftルールとサブピクセルスナップは再現しない。
// 3辺とも >= 0 で判定するため共有辺は二重被覆になるが、隙間は開かない。
// シルエットに±1画素の差が出る
float3 SWRasterBarycentric(SWRasterTriangle tri, float2 pixelCenter)
{
    const float e0 = SWRasterEdge(tri.V[1].Screen.xy, tri.V[2].Screen.xy, pixelCenter);
    const float e1 = SWRasterEdge(tri.V[2].Screen.xy, tri.V[0].Screen.xy, pixelCenter);
    const float e2 = SWRasterEdge(tri.V[0].Screen.xy, tri.V[1].Screen.xy, pixelCenter);
    return float3(e0, e1, e2) / tri.Area2;
}

bool SWRasterInside(float3 bary)
{
    return all(bary >= 0.0f);
}

// スクリーン空間で線形補間した深度(NDC)。
//
// 【なぜ透視補正が要らないのか】この射影行列(Camera::GetProjectionMatrix)は
// _33=a, _43=b, _34=1 なので clip.w = viewZ、すなわち z_ndc = a + b/viewZ = a + b*invW。
// z_ndcはinvWの1次関数であり、invWはスクリーン空間で線形に変化するのでz_ndcも線形になる。
// ハードウェアが深度をスクリーン空間で線形補間するのと数学的に同値
float SWRasterInterpolateDepth(SWRasterTriangle tri, float3 bary)
{
    return bary.x * tri.V[0].Screen.z + bary.y * tri.V[1].Screen.z + bary.z * tri.V[2].Screen.z;
}

// 透視補正済みの補間重み。頂点属性(法線・UV)はこちらで補間する。
//
// スクリーン空間で線形なのは属性そのものではなく「属性 / w」なので、重心座標に1/wを
// 掛けて正規化し直す。これを忘れると、奥行きのある面で属性が手前へ寄る古典的な破綻になる
float3 SWRasterPerspectiveWeights(SWRasterTriangle tri, float3 bary)
{
    const float3 weighted = float3(
        bary.x * tri.V[0].InvW,
        bary.y * tri.V[1].InvW,
        bary.z * tri.V[2].InvW);
    const float sum = weighted.x + weighted.y + weighted.z;
    return weighted / max(sum, 1e-20f);
}

// 深度と三角形番号を1つの64bit値へ詰める。
//
// 【なぜ64bitなのか】深度テストと三角形IDの書き込みを別々のアトミックにすると、
// 「勝者が深度を書いた後、敗者がIDを書く」競合が必ず起きる。判定と書き込みを
// 1つのInterlockedMaxにまとめればこの競合は消えるが、そのためには深度とIDが
// 同じワードに入る必要がある。Bistro Exteriorの284万三角形はIDに22bit使うため、
// 32bitでは深度に10bitしか残らずZファイティングで使い物にならない。
//
// z_ndcは[0,1]の非負floatなので、asuintのビットパターンがそのまま大小関係を保つ。
// Reverse-Zにより「大きいz = 手前」なのでInterlockedMaxがそのまま最近傍を選び、
// 深度が完全に同値なら三角形番号の大きい方が勝つ = 完全に決定的になる。
// クリア値0は z_ndc==0(遠平面)なので「当たり無し」を意味する
uint64_t SWRasterPackVisibility(float depthNdc, uint triangleIndex)
{
    return (((uint64_t)asuint(depthNdc)) << 32) | (uint64_t)triangleIndex;
}

#endif // KURENAI_SOFTWARE_RASTER_COMMON_HLSLI
