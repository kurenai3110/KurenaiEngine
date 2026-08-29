// メッシュレットに関する、ラスタライズ経路とレイトレーシング経路で共有する定義。
//
// 【なぜ共有するのか】この2つの経路は同じ.kgeomから同じ塊分けを見ているはずで、
// それを確かめる手段が「メッシュレットごとの色分けを両方に出して見比べる」こと。
// 色の作り方が1文字でも違うと同じ塊が違う色になり、確認そのものが成立しなくなるため、
// 定義を写さずここへ集める(NormalEncoding.hlsliのOctEncodeをCPU/GPUで揃えているのと同じ考え方)。

#ifndef KURENAI_MESHLET_HLSLI
#define KURENAI_MESHLET_HLSLI

#include "Frustum.hlsli"

// メッシュレットに属さない/引けないことを表す番号。
// ラスタ側は従来の頂点シェーダーで描かれたピクセル、RT側はメッシュレットを持たない
// .kmodel(--no-meshletsでパックしたもの)にヒットした場合がこれにあたる
static const uint kInvalidMeshletIndex = 0xFFFFFFFFu;

// メッシュレット番号から見分けやすい色を作る。
// 隣り合う番号の色が近くならないよう、大きめの素数を掛けてからビットを散らす。
// kInvalidMeshletIndexは灰色 ―― 色分け表示のはずなのに灰色が見えたら、
// そこはメッシュレットを経由していない、と一目で分かる
float3 MeshletDebugColor(uint meshletIndex)
{
    if (meshletIndex == kInvalidMeshletIndex)
    {
        return float3(0.5f, 0.5f, 0.5f);
    }

    const uint hash = meshletIndex * 2654435761u;
    return float3(
        float((hash >> 0) & 0xFFu) / 255.0f,
        float((hash >> 8) & 0xFFu) / 255.0f,
        float((hash >> 16) & 0xFFu) / 255.0f);
}

// Assets::GpuMeshlet(64バイト、Source/Library/Assets/Model.h)と1対1で対応。
// 並びとサイズを一致させること。
//
// VertexOffset/TriangleOffsetは**モデル単位に連結した表の中でのオフセット**で、
// ディスク形式(Assets::MeshletEntry)のメッシュ内相対の値ではない。
// 付け替えはModelLoaderが読み込み時に行っている
struct Meshlet
{
    uint VertexOffset;
    uint TriangleOffset;
    uint VertexCount;
    uint TriangleCount;
    float3 BoundsCenter;
    float BoundsRadius;
    float3 ConeAxis;
    float ConeCutoff;
    // この塊が属するメッシュの頂点バッファ(bindless番号)。
    // 頂点だけはメッシュ単位のバッファのままなので、塊ごとに選ぶ
    uint VertexBufferIndex;
    uint MaterialIndex;
    // Assets::kGpuMaterialFlag* の写し
    uint Flags;
    // このメッシュ内で何番目の塊か。**モデル内の通し番号ではない。**
    // 色分け表示をレイトレーシング側(RTFindMeshlet)と揃えるためにこの値を使う
    uint MeshletIndexInMesh;
};

// Assets::Vertex(56バイト)と1対1で対応。
//
// 【構造化バッファは詰めて並ぶ】定数バッファと違い、StructuredBuffer<T>のTは
// C++と同じ「メンバの型のアラインメントに従った詰めた配置」になる。
// 定数バッファの規則(float3の直後のfloatが16バイト境界をまたげない等)は適用されない。
// 実際のオフセットは Position=0 / Normal=12 / UV=24 / Tangent=32 / UV1=48 で、
// Assets::Vertexと完全に一致する
struct MeshVertex
{
    float3 Position;
    float3 Normal;
    float2 UV;
    float4 Tangent;
    float2 UV1;
};

// この塊を、いま描いているパスで描くべきか(材質によるふるい分け)。
//
// 1回のDispatchMeshでモデル全体を描くようになると、ドローやPSOの分割では
// 「半透明はG-Bufferに描かない」「カットアウトだけピクセルシェーダーを通す」といった
// 出し分けができない。どのマテリアルを描くパスなのかを定数バッファのマスクで受け取り、
// 増幅シェーダーがここで捨てる
bool MeshletPassesMaterialFilter(uint flags, uint rejectMask, uint requireMask)
{
    return (flags & rejectMask) == 0 && (flags & requireMask) == requireMask;
}

// バウンディング球(ワールド空間)が視錐台と交差するか。
//
// 【平面の取り出しは Frustum.hlsli にある】行から作るか列から作るかの取り違えは
// 過去に「真下を向いたときだけ100%誤検出する」壊れ方をした箇所で、しかもYawを振る
// 対照実験は素通りする(実装史39章)。モデル単位で同じ判定を行うコンピュートシェーダー
// (ModelCull.hlsl)と実装を分けないよう、取り出しは1箇所に集めてある
bool MeshletSphereInFrustum(float4x4 viewProj, float3 center, float radius)
{
    float4 planes[6];
    ExtractFrustumPlanes(viewProj, planes);
    return SphereInFrustumPlanes(planes, center, radius);
}

// Worldに含まれる最大スケールを求める。バウンディング球の半径をワールド空間へ移すのに使う。
// 3軸で違うスケールがかかっている場合、最大のものを使えば球は必ず元の形状を包む
float MeshletMaxWorldScale(float4x4 world)
{
    // mul(vector, matrix)規約なので、ローカルのx/y/z軸はWorldの各行に対応する
    const float sx = length(float3(world._m00, world._m01, world._m02));
    const float sy = length(float3(world._m10, world._m11, world._m12));
    const float sz = length(float3(world._m20, world._m21, world._m22));
    return max(sx, max(sy, sz));
}

#endif // KURENAI_MESHLET_HLSLI
