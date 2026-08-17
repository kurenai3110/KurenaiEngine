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

// Assets::MeshletEntry(48バイト)と1対1で対応。並びとサイズを一致させること
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

// バウンディング球(ワールド空間)が視錐台と交差するか。
//
// 平面はViewProjから直接取り出す。このエンジンはmul(vector, matrix)の規約なので
// clip.x = dot(v, ViewProjの0列目)、clip.w = dot(v, ViewProjの3列目) になる。
// クリップ空間の条件 -w <= x <= w、-w <= y <= w、0 <= z <= w をそれぞれ
// 「dot(平面, v) >= 0」の形に直したものが6枚の平面。
//
// 【Reverse-Zでもこのままでよい】近平面と遠平面の意味は入れ替わるが、
// 0 <= z <= w という条件自体は変わらないため、平面の式は同じで済む。
//
// 【TAAのジッターは無視してよい】ViewProjにはサブピクセルのジッターが乗っているが、
// ずれはピクセル単位以下で、バウンディング球という保守的な近似の余裕に埋もれる
bool IsSphereInFrustum(float3 center, float radius)
{
    // ViewProjの列ベクトル(HLSLのfloat4x4は行優先の添字なので、列は_mXY表記で取り出す)
    const float4 col0 = float4(ViewProj._m00, ViewProj._m10, ViewProj._m20, ViewProj._m30);
    const float4 col1 = float4(ViewProj._m01, ViewProj._m11, ViewProj._m21, ViewProj._m31);
    const float4 col2 = float4(ViewProj._m02, ViewProj._m12, ViewProj._m22, ViewProj._m32);
    const float4 col3 = float4(ViewProj._m03, ViewProj._m13, ViewProj._m23, ViewProj._m33);

    float4 planes[6];
    planes[0] = col3 + col0; // 左   (x >= -w)
    planes[1] = col3 - col0; // 右   (x <=  w)
    planes[2] = col3 + col1; // 下   (y >= -w)
    planes[3] = col3 - col1; // 上   (y <=  w)
    planes[4] = col2;        // 手前 (z >=  0)
    planes[5] = col3 - col2; // 奥   (z <=  w)

    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        // 平面を正規化しないと「距離」の尺度が平面ごとに変わり、radiusと比較できない
        const float length3 = length(planes[i].xyz);
        if (length3 <= 0.0f)
        {
            // 射影行列が退化している(想定外)。カリングを諦めて通す
            continue;
        }

        const float4 plane = planes[i] / length3;
        if (dot(plane.xyz, center) + plane.w < -radius)
        {
            return false;
        }
    }
    return true;
}

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

// Worldに含まれる最大スケールを求める。バウンディング球の半径をワールド空間へ移すのに使う。
// 3軸で違うスケールがかかっている場合、最大のものを使えば球は必ず元の形状を包む
float MaxWorldScale()
{
    // mul(vector, matrix)規約なので、ローカルのx/y/z軸はWorldの各行に対応する
    const float sx = length(float3(World._m00, World._m01, World._m02));
    const float sy = length(float3(World._m10, World._m11, World._m12));
    const float sz = length(float3(World._m20, World._m21, World._m22));
    return max(sx, max(sy, sz));
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
        StructuredBuffer<Meshlet> meshlets = KURENAI_BINDLESS_BUFFER(MeshletBufferIndex);
        const Meshlet meshlet = meshlets[dispatchThreadId];

        const float3 centerWorld = mul(float4(meshlet.BoundsCenter, 1.0f), World).xyz;
        const float radiusWorld = meshlet.BoundsRadius * MaxWorldScale();

        if (IsSphereInFrustum(centerWorld, radiusWorld) && !IsMeshletBackfacing(meshlet, centerWorld))
        {
            // 【波の幅に依存しない詰め方】WavePrefixCountBitsを使うと1グループが
            // 1波に収まることを暗に仮定することになる(波幅32/64はGPUによって違う)。
            // グループ共有のカウンタなら仮定が要らず、頻度も低いので競合の実害も無い
            uint slot;
            InterlockedAdd(s_VisibleCount, 1, slot);
            s_Payload.MeshletIndices[slot] = dispatchThreadId;
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
        StructuredBuffer<MeshVertex> vertices = KURENAI_BINDLESS_BUFFER(VertexBufferIndex);

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
        output.MeshletIndex = meshletIndex;

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
