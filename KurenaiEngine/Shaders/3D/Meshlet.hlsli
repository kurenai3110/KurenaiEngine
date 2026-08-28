// メッシュレットに関する、ラスタライズ経路とレイトレーシング経路で共有する定義。
//
// 【なぜ共有するのか】この2つの経路は同じ.kgeomから同じ塊分けを見ているはずで、
// それを確かめる手段が「メッシュレットごとの色分けを両方に出して見比べる」こと。
// 色の作り方が1文字でも違うと同じ塊が違う色になり、確認そのものが成立しなくなるため、
// 定義を写さずここへ集める(NormalEncoding.hlsliのOctEncodeをCPU/GPUで揃えているのと同じ考え方)。

#ifndef KURENAI_MESHLET_HLSLI
#define KURENAI_MESHLET_HLSLI

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
// 平面はViewProjから直接取り出す。このエンジンはmul(vector, matrix)の規約なので
// clip.x = dot(v, ViewProjの0列目)、clip.w = dot(v, ViewProjの3列目) になる。
// クリップ空間の条件 -w <= x <= w、-w <= y <= w、0 <= z <= w をそれぞれ
// 「dot(平面, v) >= 0」の形に直したものが6枚の平面。
//
// 【行ではなく列から作ること】列と行を取り違えると、特定の視線方向でだけ
// 100%誤検出する(真下を向いたときに全部消える等)。しかもYawを振る対照実験は
// 素通りしてしまうため、絵を見ているだけでは気づけない(実装史39章)。
//
// 【Reverse-Zでもこのままでよい】近平面と遠平面の意味は入れ替わるが、
// 0 <= z <= w という条件自体は変わらないため、平面の式は同じで済む。
//
// 【正射影でもそのまま使える】シャドウのカスケードは平行光の正射影だが、
// クリップ空間の条件は透視投影と同じなのでこの導出がそのまま当てはまる。
//
// 【TAAのジッターは無視してよい】ViewProjにはサブピクセルのジッターが乗っているが、
// ずれはピクセル単位以下で、バウンディング球という保守的な近似の余裕に埋もれる
bool MeshletSphereInFrustum(float4x4 viewProj, float3 center, float radius)
{
    // ViewProjの列ベクトル(HLSLのfloat4x4は行優先の添字なので、列は_mXY表記で取り出す)
    const float4 col0 = float4(viewProj._m00, viewProj._m10, viewProj._m20, viewProj._m30);
    const float4 col1 = float4(viewProj._m01, viewProj._m11, viewProj._m21, viewProj._m31);
    const float4 col2 = float4(viewProj._m02, viewProj._m12, viewProj._m22, viewProj._m32);
    const float4 col3 = float4(viewProj._m03, viewProj._m13, viewProj._m23, viewProj._m33);

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
