// G-Bufferパスのメッシュシェーダー版(増幅シェーダー + メッシュシェーダー)。
//
// 【何が変わるのか】従来のGBuffer.hlslは入力アセンブラが流す頂点を1つずつ変換するため、
// カリングの粒度が「DrawIndexed 1回 = メッシュ全体」しかない。ドラゴンのように
// 1メッシュが数十万三角形あるモデルでは、画面外・背面の三角形もすべてラスタライザまで
// 到達してしまう。
//
// このパスはKurenaiPackerが焼いたメッシュレット(頂点64個・三角形124個までの塊、
// Assets::MeshletEntry)を単位にし、増幅シェーダーが塊ごとに錐台・背面カリングしてから
// 生き残ったものだけをメッシュシェーダーへ渡す。
//
// 【ピクセルシェーダーはGBuffer.hlslのものをそのまま使う】出力するPSInputの中身は
// VSMainとまったく同じにしてあるので、G-Bufferへの書き込み方は1行も変わらない
// (Water.hlslがGBufferCommon.hlsliのVSMainを共有しているのと同じ考え方)。
// そのためメッシュレットのON/OFFを切り替えても見た目は一致するはずで、
// 一致しなければこのファイルのどこかが間違っている、と切り分けられる。
//
// 【bindlessが前提】頂点もメッシュレットもResourceDescriptorHeap経由で読む。
// メッシュシェーダー対応GPUは実質すべてSM 6.6にも対応しているため、
// bindlessが無い環境向けの別実装は用意していない
// (DX12Device::DetectMeshShaderSupportがbindless非対応なら丸ごと無効にする)。

// 【このファイルはピクセルシェーダーを持たない】G-Bufferへの書き込みはGBuffer.hlslの
// PSMainがそのまま担う(出力するPSInputの中身をVSMainと同じにしてあるため)。
// メッシュレットの色分け表示(PSMainMeshletDebug)もGBuffer.hlsl側に置いてある ――
// このファイルは増幅シェーダー用のgroupshared宣言を持っており、
// そこからピクセルシェーダーをコンパイルさせない方が安全なため
#include "GBufferCommon.hlsli"
#include "Bindless.hlsli"

// 【Meshlet / MeshVertex の定義は Meshlet.hlsli にある】シャドウ版の
// メッシュシェーダー(ShadowMeshlet.hlsl)と共有するため。写して2つに増やすと
// 片方だけ直したときにジオメトリの読み方が静かに食い違う

// 増幅シェーダー1グループが判定するメッシュレット数。
// 生き残ったメッシュレット番号をペイロードで渡すため、ペイロードの配列長でもある。
// メッシュシェーダーのペイロードは16KBまでだが、ここでは32×4バイト=128バイトしか使わない
#define KURENAI_AMPLIFICATION_GROUP_SIZE 32

// メッシュシェーダーの1グループのスレッド数。1スレッドが頂点1つと三角形1つを担当するため、
// メッシュレットの上限(頂点64・三角形124、Assets::kMeshletMax*)以上あればよい
#define KURENAI_MESH_GROUP_SIZE 128

struct MeshletPayload
{
    uint MeshletIndices[KURENAI_AMPLIFICATION_GROUP_SIZE];
};

groupshared MeshletPayload s_Payload;
groupshared uint s_VisibleCount;

// --- カリング -------------------------------------------------------------------------

// 【錐台カリングとスケール計算は Meshlet.hlsli にある】シャドウ版と共有するため。
// 視錐台平面を行から作るか列から作るかは過去に取り違えて100%誤検出した箇所で、
// 実装を2つに増やすと片方だけが壊れたまま気づけない(実装史39章)

// 法線コーンによる背面カリング。この塊の三角形の法線がすべて
// 「軸ConeAxisを中心とする半頂角acos(ConeCutoff)の円錐」に収まることを利用し、
// 視線がその円錐の内側にあれば全部背面なので丸ごと落とす。
//
// 【ConeCutoffが1のメッシュレットは落とさない】法線の広がりが半球を超えて
// コーンで表せない場合、meshoptimizerはConeCutoff=1・ConeAxis=(0,0,0)を返す。
// この値だとdot(...)=0 >= 1 が常に偽なので判定自体は安全に「通す」側へ倒れるが、
// 長さ0の軸をnormalizeするとNaNになるため、先に弾いておく
bool IsMeshletBackfacing(Meshlet meshlet, float3 centerWorld)
{
    if (meshlet.ConeCutoff >= 1.0f)
    {
        return false;
    }

    // コーンの軸は法線と同じく面の向きなので、Worldではなく法線行列で変換する
    // (非一様スケールで向きが歪むのを防ぐ)
    const float3 axisWorld = normalize(mul(meshlet.ConeAxis, (float3x3)NormalMatrix));
    const float3 viewDir = normalize(centerWorld - CameraPosition.xyz);
    return dot(viewDir, axisWorld) >= meshlet.ConeCutoff;
}

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
        // 表はモデル単位なので、このドローが見る範囲の先頭(MeshletOffset)を足す
        const uint meshletIndex = MeshletOffset + dispatchThreadId;
        StructuredBuffer<Meshlet> meshlets = KURENAI_BINDLESS_BUFFER(MeshletBufferIndex);
        const Meshlet meshlet = meshlets[meshletIndex];

        const float3 centerWorld = mul(float4(meshlet.BoundsCenter, 1.0f), World).xyz;
        const float radiusWorld = meshlet.BoundsRadius * MeshletMaxWorldScale(World);

        // 材質によるふるい分け(GBufferCommon.hlsliのMeshletFilterReject/Require参照)。
        // G-Bufferでは半透明(BLEND)を落とす
        const bool materialAccepted =
            MeshletPassesMaterialFilter(meshlet.Flags, MeshletFilterReject, MeshletFilterRequire);

        if (materialAccepted && MeshletSphereInFrustum(ViewProj, centerWorld, radiusWorld)
            && !IsMeshletBackfacing(meshlet, centerWorld))
        {
            // 【波の幅に依存しない詰め方】WavePrefixCountBitsを使うと1グループが
            // 1波に収まることを暗に仮定することになる(波幅32/64はGPUによって違う)。
            // グループ共有のカウンタなら仮定が要らず、頻度も低いので競合の実害も無い
            uint slot;
            InterlockedAdd(s_VisibleCount, 1, slot);
            s_Payload.MeshletIndices[slot] = meshletIndex;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    // DispatchMeshはグループ内の全スレッドが同じ引数で1回だけ呼ぶ決まり。
    // バリア後のs_VisibleCountは全スレッドで同じ値になっている
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
    // メッシュシェーダーの出力配列長はコンパイル時定数でなければならず、
    // C++側のヘッダーをHLSLへ取り込む手段が無いため写している。
    // パッカーが焼く上限を変えたらここも直すこと(小さいままだと出力が溢れる)
    out vertices PSInput outVertices[64],
    out indices uint3 outTriangles[124])
{
    const uint meshletIndex = payload.MeshletIndices[groupId];

    StructuredBuffer<Meshlet> meshlets = KURENAI_BINDLESS_BUFFER(MeshletBufferIndex);
    const Meshlet meshlet = meshlets[meshletIndex];

    // 実際に出力する頂点数・三角形数の申告。これより後にoutVertices/outTrianglesへ書く
    SetMeshOutputCounts(meshlet.VertexCount, meshlet.TriangleCount);

    if (groupThreadId < meshlet.VertexCount)
    {
        StructuredBuffer<uint> meshletVertices = KURENAI_BINDLESS_BUFFER(MeshletVertexBufferIndex);
        // 【NonUniformResourceIndexが要る】頂点バッファの番号はメッシュレットごとに違い、
        // 1回のディスパッチでメッシュを跨ぐと同じ波の中で値が発散する。
        // 付け忘れると未定義動作になるが、**絵はそれらしく出たまま静かに壊れる**
        StructuredBuffer<MeshVertex> vertices =
            KURENAI_BINDLESS_BUFFER(NonUniformResourceIndex(meshlet.VertexBufferIndex));

        const uint globalVertexIndex = meshletVertices[meshlet.VertexOffset + groupThreadId];
        const MeshVertex vertex = vertices[globalVertexIndex];

        // ここから下はGBufferCommon.hlsliのVSMainと同じ変換。
        // 【両者を必ず揃えること】どちらかだけ直すと、メッシュレットのON/OFFで
        // 見た目が食い違うという分かりにくい壊れ方をする
        PSInput output;
        const float3 worldPos = mul(float4(vertex.Position, 1.0f), World).xyz;
        output.Position = mul(float4(worldPos, 1.0f), ViewProj);
        output.Normal = mul(vertex.Normal, (float3x3)NormalMatrix);
        output.WorldPos = worldPos;
        output.UV = vertex.UV;
        output.LightmapUV = vertex.UV1;
        output.Tangent = float4(mul(vertex.Tangent.xyz, (float3x3)World), vertex.Tangent.w * TangentSignFlip);
        output.CurClip = output.Position;
        output.PrevClip = mul(float4(worldPos, 1.0f), PrevViewProj);
        // 【モデル内の通し番号ではなくメッシュ内の番号を書く】色分け表示は
        // レイトレーシング側(RaytracingScene.hlsliのRTFindMeshlet)と同じ色でなければ
        // 見比べる意味が無く、あちらはメッシュ内の番号を返す
        output.MeshletIndex = meshlet.MeshletIndexInMesh;
        // ピクセルシェーダーがマテリアルテーブルを引くための番号。
        // 塊の中では全頂点で同じ値になる(メッシュレットは材質を跨がない)ので、
        // PSInput側のnointerpolationがそのまま正しい値を拾う
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
