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

#endif // KURENAI_MESHLET_HLSLI
