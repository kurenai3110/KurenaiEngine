// シャドウパスのメッシュシェーダー版(増幅シェーダー + メッシュシェーダー)。
//
// 【なぜ要るのか】メッシュレット経路はG-Bufferにしか無かった。そのためモデルを
// 1ドローで描けるようになっても、シャドウだけは従来どおりメッシュ単位で、
// しかも**カスケード4枚ぶん**発行され続ける。PLATEAU LOD2の1タイル(メッシュ1,715個)なら
// G-Bufferが1ドローになる一方でシャドウは1,715×4=6,860ドローのまま、という片手落ちになる。
//
// 【G-Buffer版と分けている理由】シャドウパスのb0はFrameConstantsではなく
// CascadeConstants(カスケードのビュー射影1つだけ)で、cbufferのレイアウトが違う。
// ジオメトリの読み方(Meshlet / MeshVertex / 錐台カリング)はMeshlet.hlsliで共有しており、
// このファイルが持つのは「ライト視点で深度だけを書く」ぶんの差分だけ。
//
// 【背面カリングを行わない】G-Buffer版は法線コーンとカメラ位置で背面の塊を落とすが、
// ここで使えるのはCascadeConstantsのViewProjだけで、平行光の向きを直接は持っていない。
// 錐台カリングだけ行う。影は表裏どちらの面からも落ちるため、落とし損ねよりは
// 通しすぎるほうが安全側でもある。
//
// 【bindlessが前提】G-Buffer版と同じく、頂点もメッシュレットもResourceDescriptorHeap経由。
// メッシュシェーダー対応の判定にbindless対応が含まれている
// (DX12Device::DetectMeshShaderSupport)ため、非対応環境向けの実装は用意していない
#include "Bindless.hlsli"
#include "Meshlet.hlsli"

// シャドウパス専用の定数バッファ。カスケードごとにこの1つの行列だけを差し替えて
// 同じジオメトリを描き直す(頂点シェーダー版のShadow.hlslと同じもの)
cbuffer CascadeConstants : register(b0)
{
    float4x4 ViewProj;
};

#include "ObjectConstants.hlsli"

#define KURENAI_AMPLIFICATION_GROUP_SIZE 32
#define KURENAI_MESH_GROUP_SIZE 128

struct MeshletPayload
{
    uint MeshletIndices[KURENAI_AMPLIFICATION_GROUP_SIZE];
};

groupshared MeshletPayload s_Payload;
groupshared uint s_VisibleCount;

// メッシュシェーダーからピクセルシェーダーへ渡すもの。
// 深度しか書かないので位置だけで足りるが、カットアウト用のPSがベースカラーを
// 引く必要があるためUVとマテリアル番号も運ぶ(不透明用のPSOはPSを持たないので、
// そちらでは補間器が使われず無駄にもならない)
struct ShadowPSInput
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
    // 塊の中では全頂点で同じ値になる(メッシュレットは材質を跨がない)
    nointerpolation uint MaterialIndex : TEXCOORD1;
};

// --- 増幅シェーダー -------------------------------------------------------------------

[numthreads(KURENAI_AMPLIFICATION_GROUP_SIZE, 1, 1)]
void ASMain(uint dispatchThreadId : SV_DispatchThreadID, uint groupThreadId : SV_GroupThreadID)
{
    if (groupThreadId == 0)
    {
        s_VisibleCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    if (dispatchThreadId < MeshletCount)
    {
        const uint meshletIndex = MeshletOffset + dispatchThreadId;
        StructuredBuffer<Meshlet> meshlets = KURENAI_BINDLESS_BUFFER(MeshletBufferIndex);
        const Meshlet meshlet = meshlets[meshletIndex];

        const float3 centerWorld = mul(float4(meshlet.BoundsCenter, 1.0f), World).xyz;
        const float radiusWorld = meshlet.BoundsRadius * MeshletMaxWorldScale(World);

        // 材質によるふるい分け。シャドウは不透明用(PS無し)とカットアウト用(clipあり)で
        // 2回に分けて発行されるため、どちらの回なのかをマスクで受け取る
        const bool materialAccepted =
            MeshletPassesMaterialFilter(meshlet.Flags, MeshletFilterReject, MeshletFilterRequire);

        // カスケードの正射影でカリングする。カメラではなくライト側の錐台なので、
        // 画面外にあっても影を落とすものは残る
        if (materialAccepted && MeshletSphereInFrustum(ViewProj, centerWorld, radiusWorld))
        {
            uint slot;
            InterlockedAdd(s_VisibleCount, 1, slot);
            s_Payload.MeshletIndices[slot] = meshletIndex;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    DispatchMesh(s_VisibleCount, 1, 1, s_Payload);
}

// --- メッシュシェーダー ---------------------------------------------------------------

[outputtopology("triangle")]
[numthreads(KURENAI_MESH_GROUP_SIZE, 1, 1)]
void MSMain(
    uint groupThreadId : SV_GroupThreadID,
    uint groupId : SV_GroupID,
    in payload MeshletPayload payload,
    // 【Assets::kMeshletMaxVertices / kMeshletMaxTriangles と必ず一致させること】
    out vertices ShadowPSInput outVertices[64],
    out indices uint3 outTriangles[124])
{
    const uint meshletIndex = payload.MeshletIndices[groupId];

    StructuredBuffer<Meshlet> meshlets = KURENAI_BINDLESS_BUFFER(MeshletBufferIndex);
    const Meshlet meshlet = meshlets[meshletIndex];

    SetMeshOutputCounts(meshlet.VertexCount, meshlet.TriangleCount);

    if (groupThreadId < meshlet.VertexCount)
    {
        StructuredBuffer<uint> meshletVertices = KURENAI_BINDLESS_BUFFER(MeshletVertexBufferIndex);
        // NonUniformResourceIndexが要る理由はBindless.hlsliのコメント参照
        StructuredBuffer<MeshVertex> vertices =
            KURENAI_BINDLESS_BUFFER(NonUniformResourceIndex(meshlet.VertexBufferIndex));

        const uint globalVertexIndex = meshletVertices[meshlet.VertexOffset + groupThreadId];
        const MeshVertex vertex = vertices[globalVertexIndex];

        // 頂点シェーダー版(Shadow.hlslのVSMain)と同じ変換。
        // 【両者を必ず揃えること】どちらかだけ直すと、メッシュレットのON/OFFで
        // 影の位置が食い違うという分かりにくい壊れ方をする
        ShadowPSInput output;
        const float3 worldPos = mul(float4(vertex.Position, 1.0f), World).xyz;
        output.Position = mul(float4(worldPos, 1.0f), ViewProj);
        output.UV = vertex.UV;
        output.MaterialIndex = meshlet.MaterialIndex;

        outVertices[groupThreadId] = output;
    }

    if (groupThreadId < meshlet.TriangleCount)
    {
        StructuredBuffer<uint> meshletTriangles = KURENAI_BINDLESS_BUFFER(MeshletTriangleBufferIndex);
        // ローカル頂点番号3つが下位24bitへ詰まっている(Assets::PackMeshletTriangle)
        const uint packed = meshletTriangles[meshlet.TriangleOffset + groupThreadId];
        outTriangles[groupThreadId] = uint3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
    }
}
