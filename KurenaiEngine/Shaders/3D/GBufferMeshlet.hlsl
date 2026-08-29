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
#include "HiZCull.hlsli"

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

// --- カリング統計 -----------------------------------------------------------------------
//
// 【なぜ数えるのか】「間引き0」は、判定式が常に通しているのか本当に全部見えているのかを
// 区別できない。CPU側のフラスタムカリングが判定数と間引き数を対で出しているのと同じ理由で、
// ここでも「判定した数」と併せて出す。
//
// **オクルージョンは視錐台+コーンとは別のカウンタにする。** 合算すると
// 「俯瞰と街路で差が出るか」というオクルージョン固有の確認ができない。
//
// 【グループ内でまとめてから1回だけ足す】メッシュレット1個ごとにグローバルのカウンタを
// InterlockedAddすると、都市シーンでは1フレーム数十万回の競合になる。
// グループ共有のカウンタへ集約し、スレッド0が1グループあたり3回だけ足す
groupshared uint s_StatsTested;
groupshared uint s_StatsFrustumCulled;
groupshared uint s_StatsOcclusionCulled;

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

// --- Hi-Zオクルージョンカリング ---------------------------------------------------------
//
// 【判定そのものは HiZCull.hlsli にある】モデル単位で同じ判定を行うコンピュートシェーダー
// (ModelCull.hlsl)と共有するため。判定の向きはReverse-Zで決まり、逆に書いても
// コンパイルは通り絵もそれらしく出るので、実装を2つに増やすと片方だけが
// 「間引きすぎる」あるいは「一度も間引かない」まま気づけない。
Texture2D<float> HiZTexture : register(t8);

bool IsMeshletOccluded(float3 centerWorld, float radiusWorld)
{
    if (OcclusionCullParams.x == 0.0f || MeshletOcclusionMode == 0u)
    {
        return false;
    }

    // 【投影に使う行列は「そのHi-Zを描いた行列」でなければならない】
    // モード1で読めるのは前フレームの深度から作ったHi-Zなので前フレームの行列、
    // モード2は同じフレームの深度プリパスから作ったものなので今フレームの行列。
    // 取り違えてもコンパイルは通り絵もそれらしく出る(GBufferCommon.hlsliのMeshletOcclusionMode参照)
    const bool usePrevFrame = (MeshletOcclusionMode == 1u);
    const float4x4 occlusionViewProj = usePrevFrame ? PrevViewProj : ViewProj;

    // 1フレーム古いHi-Zで判定するための保守的な膨張。
    //   - 半径倍率: バウンディング球がメッシュレットの実体より緩いこと、カメラ回転による見え方の変化
    //   - カメラ移動距離: 前フレームからの視差ずれ。シーンが静的なので原因はカメラの移動だけ
    // 2項は別々の失敗に効くので、片方を上げてももう片方の穴は塞がらない。
    // **今フレームのHi-Zならどちらも要らない** ―― ずれる原因そのものが無い
    const float radius = usePrevFrame
        ? (radiusWorld * OcclusionCullParams.y + OcclusionCullParams.z)
        : radiusWorld;

    return IsAabbOccludedByHiZ(
        HiZTexture, occlusionViewProj,
        centerWorld - radius, centerWorld + radius,
        HiZScreenParams.xy, (uint)OcclusionCullParams.w);
}

// --- 増幅シェーダー -------------------------------------------------------------------

[numthreads(KURENAI_AMPLIFICATION_GROUP_SIZE, 1, 1)]
void ASMain(uint dispatchThreadId : SV_DispatchThreadID, uint groupThreadId : SV_GroupThreadID)
{
    const bool statsEnabled = (MeshletCullStatsParams.x != 0.0f) && (MeshletStatsEnabled != 0u);

    if (groupThreadId == 0)
    {
        s_VisibleCount = 0;
        s_StatsTested = 0;
        s_StatsFrustumCulled = 0;
        s_StatsOcclusionCulled = 0;
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
        // G-Bufferでは半透明(BLEND)を落とす。
        //
        // 【材質で落ちた塊はカリングの統計に数えない】数えると、そのパスが除外している
        // 材質のぶんだけ「間引き率」が薄まり、俯瞰と街路の差を見る目的に使えなくなる
        const bool materialAccepted =
            MeshletPassesMaterialFilter(meshlet.Flags, MeshletFilterReject, MeshletFilterRequire);

        // 【この順に判定する】視錐台と法線コーンは定数時間だが、Hi-Z判定は8頂点の投影と
        // テクスチャ読みを伴う。先に安いほうで落とせば、画面外・背面の塊ではHi-Zを一切読まない
        const bool frustumOrConeCulled = !MeshletSphereInFrustum(ViewProj, centerWorld, radiusWorld)
            || IsMeshletBackfacing(meshlet, centerWorld);
        const bool occlusionCulled =
            materialAccepted && !frustumOrConeCulled && IsMeshletOccluded(centerWorld, radiusWorld);

        if (statsEnabled && materialAccepted)
        {
            InterlockedAdd(s_StatsTested, 1);
            if (frustumOrConeCulled)
            {
                InterlockedAdd(s_StatsFrustumCulled, 1);
            }
            else if (occlusionCulled)
            {
                InterlockedAdd(s_StatsOcclusionCulled, 1);
            }
        }

        if (materialAccepted && !frustumOrConeCulled && !occlusionCulled)
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

    // グループ内の集計をグローバルのカウンタへ1回だけ足す。
    // 【DispatchMeshより前に、かつバリアの後で行うこと】バリアの前だと集計が完了していない
    if (statsEnabled && groupThreadId == 0)
    {
        RWStructuredBuffer<uint> cullStats = KURENAI_BINDLESS_BUFFER((uint)MeshletCullStatsParams.y);
        InterlockedAdd(cullStats[0], s_StatsTested);
        InterlockedAdd(cullStats[1], s_StatsFrustumCulled);
        InterlockedAdd(cullStats[2], s_StatsOcclusionCulled);
    }

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
