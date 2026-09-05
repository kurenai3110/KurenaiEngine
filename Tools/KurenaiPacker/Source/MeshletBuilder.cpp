#include "MeshletBuilder.h"

#include <algorithm>
#include <cstring>

#include "meshoptimizer.h"

namespace KurenaiPacker
{
    using Kurenai::Assets::MeshletEntry;
    using Kurenai::Assets::PackMeshletTriangle;
    using Kurenai::Assets::Vertex;
    using Kurenai::Assets::kMaxMeshletLODCount;
    using Kurenai::Assets::kMeshletConeWeight;
    using Kurenai::Assets::kMeshletMaxTriangles;
    using Kurenai::Assets::kMeshletMaxVertices;

    namespace
    {
        // 1段ぶんのメッシュレットを構築し、resultのメッシュレット3配列へ追記する。
        // 戻り値は追記したメッシュレットの数(0なら構築できなかった)。
        //
        // outReorderedIndices が非nullptrなら、メッシュレット順に並べ直したインデックスを
        // そこへ書き出す(LOD0でだけ使う。簡略化した段のインデックスは.kgeomへ書かない)
        size_t AppendMeshletsForLOD(
            MeshletBuildResult& result,
            const std::vector<uint32_t>& lodIndices,
            uint32_t lodLevel,
            std::vector<uint32_t>* outReorderedIndices)
        {
            if (lodIndices.empty() || (lodIndices.size() % 3) != 0 || result.Vertices.empty())
            {
                return 0;
            }

            // 位置はVertexの先頭12バイト(Vertex::Position)にあるため、先頭を指してストライドを渡す
            const float* positions = result.Vertices[0].Position;

            const size_t maxMeshlets =
                meshopt_buildMeshletsBound(lodIndices.size(), kMeshletMaxVertices, kMeshletMaxTriangles);

            std::vector<meshopt_Meshlet> rawMeshlets(maxMeshlets);
            // 最悪ケースの必要量はどちらもindex_count(vertex_countではない)
            std::vector<uint32_t> rawMeshletVertices(lodIndices.size());
            std::vector<unsigned char> rawMeshletTriangles(lodIndices.size());

            const size_t meshletCount = meshopt_buildMeshlets(
                rawMeshlets.data(),
                rawMeshletVertices.data(),
                rawMeshletTriangles.data(),
                lodIndices.data(),
                lodIndices.size(),
                positions,
                result.Vertices.size(),
                sizeof(Vertex),
                kMeshletMaxVertices,
                kMeshletMaxTriangles,
                kMeshletConeWeight);

            if (meshletCount == 0)
            {
                return 0;
            }
            rawMeshlets.resize(meshletCount);

            // meshoptimizerが返すmeshlet_trianglesは、メッシュレットごとに4バイト境界へ
            // 揃うようパディングが入る(triangle_offsetは3の倍数とは限らない)。
            // こちらは1三角形=1要素のuint32配列へ詰め直すので、パディングを持ち込まないよう
            // オフセットは自前で数え直す
            for (const meshopt_Meshlet& raw : rawMeshlets)
            {
                uint32_t* meshletVertices = &rawMeshletVertices[raw.vertex_offset];
                unsigned char* meshletTriangles = &rawMeshletTriangles[raw.triangle_offset];

                // メッシュレット内での頂点・三角形の並びを整えると、ラスタライザのスループットと
                // 頂点の再利用率が上がる。ローカル番号の対応もこの中で整合が保たれる
                meshopt_optimizeMeshlet(meshletVertices, meshletTriangles, raw.triangle_count, raw.vertex_count);

                MeshletEntry entry{};
                // オフセットはメッシュのブロック先頭からの相対。全LOD段を1つのブロックへ
                // 連結するので、段が変わっても数え方は変わらない
                entry.VertexOffset = static_cast<uint32_t>(result.MeshletVertices.size());
                entry.TriangleOffset = static_cast<uint32_t>(result.MeshletTriangles.size());
                entry.VertexCount = raw.vertex_count;
                entry.TriangleCount = raw.triangle_count;
                entry.LODLevel = lodLevel;
                // MaterialIndexはメッシュの材質なのでMeshletBuilderからは決められない。
                // PackageWriterが書き出す直前に転記する
                entry.MaterialIndex = 0;

                for (uint32_t i = 0; i < raw.vertex_count; ++i)
                {
                    result.MeshletVertices.push_back(meshletVertices[i]);
                }

                for (uint32_t t = 0; t < raw.triangle_count; ++t)
                {
                    const uint32_t a = meshletTriangles[t * 3 + 0];
                    const uint32_t b = meshletTriangles[t * 3 + 1];
                    const uint32_t c = meshletTriangles[t * 3 + 2];
                    result.MeshletTriangles.push_back(PackMeshletTriangle(a, b, c));

                    if (outReorderedIndices != nullptr)
                    {
                        // グローバルなインデックスバッファ側は2段の間接参照を解決して書く
                        outReorderedIndices->push_back(meshletVertices[a]);
                        outReorderedIndices->push_back(meshletVertices[b]);
                        outReorderedIndices->push_back(meshletVertices[c]);
                    }
                }

                const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
                    meshletVertices, meshletTriangles, raw.triangle_count, positions, result.Vertices.size(), sizeof(Vertex));

                entry.BoundsCenter[0] = bounds.center[0];
                entry.BoundsCenter[1] = bounds.center[1];
                entry.BoundsCenter[2] = bounds.center[2];
                entry.BoundsRadius = bounds.radius;
                entry.ConeAxis[0] = bounds.cone_axis[0];
                entry.ConeAxis[1] = bounds.cone_axis[1];
                entry.ConeAxis[2] = bounds.cone_axis[2];
                entry.ConeCutoff = bounds.cone_cutoff;

                result.Meshlets.push_back(entry);
            }

            return meshletCount;
        }
    }

    MeshletBuildResult BuildMeshlets(
        const std::vector<Vertex>& vertices,
        const std::vector<uint32_t>& indices,
        bool enableMeshlets,
        unsigned int lodCount)
    {
        MeshletBuildResult result;
        result.Vertices = vertices;
        result.Indices = indices;

        if (!enableMeshlets || lodCount == 0 || vertices.empty() || indices.empty() || (indices.size() % 3) != 0)
        {
            return result;
        }

        const uint32_t requestedLODCount =
            std::min<uint32_t>(static_cast<uint32_t>(lodCount), kMaxMeshletLODCount);

        // --- 1. 頂点の並びを最適化する -------------------------------------------------------
        //
        // メッシュレット化の前に掛けるのが重要。buildMeshletsは入力の三角形の並び順を
        // 手掛かりに隣接した三角形を1つの塊へまとめるため、キャッシュ最適化で
        // 局所性が上がっているほど「まとまりのよい」メッシュレットになる。
        //
        // optimizeVertexFetchは頂点配列そのものを並べ替える。.kgeomへ書くのは
        // この並べ替え後の頂点なので、呼び出し側は入力ではなくresult.Verticesを使うこと。
        //
        // **LODの簡略化もこの後の頂点・インデックスから始める。** meshopt_simplifyは
        // 頂点を増やさずインデックスを削るだけなので、全段が同じ頂点バッファを共有できる
        meshopt_optimizeVertexCache(result.Indices.data(), indices.data(), indices.size(), vertices.size());

        const size_t uniqueVertexCount = meshopt_optimizeVertexFetch(
            result.Vertices.data(),
            result.Indices.data(),
            result.Indices.size(),
            vertices.data(),
            vertices.size(),
            sizeof(Vertex));
        // どの三角形からも参照されていない頂点は末尾へ寄せられ、戻り値の外側になる。
        // 使われない頂点を.kgeomへ書いても無駄なので切り詰める
        result.Vertices.resize(uniqueVertexCount);

        // --- 2. LOD0(原寸)をメッシュレットへ分割する ----------------------------------------
        //
        // 同時に、インデックスバッファをメッシュレット順に組み立て直す。
        // レイトレーシング側がヒットした三角形番号から所属メッシュレットを
        // 二分探索で引けるようにするため(ModelPackage.hの.kgeom v3の説明)
        result.Meshlets.reserve(meshopt_buildMeshletsBound(result.Indices.size(), kMeshletMaxVertices, kMeshletMaxTriangles));
        result.MeshletVertices.reserve(result.Indices.size());
        result.MeshletTriangles.reserve(result.Indices.size() / 3);

        std::vector<uint32_t> reorderedIndices;
        reorderedIndices.reserve(result.Indices.size());

        const size_t lod0MeshletCount = AppendMeshletsForLOD(result, result.Indices, 0, &reorderedIndices);
        if (lod0MeshletCount == 0)
        {
            result.Meshlets.clear();
            result.MeshletVertices.clear();
            result.MeshletTriangles.clear();
            return result;
        }

        // 【三角形の総数は変わらない】メッシュレット化は分割であって間引きではないため、
        // 並べ替え後のインデックス数は元と一致する。ここがずれるということは
        // 詰め替えのどこかを取りこぼしているので、黙って進めず入力のまま返す
        if (reorderedIndices.size() != result.Indices.size())
        {
            result.Meshlets.clear();
            result.MeshletVertices.clear();
            result.MeshletTriangles.clear();
            return result;
        }

        result.Indices = std::move(reorderedIndices);
        result.LODCount = 1;
        result.LODMeshletOffsets[0] = 0;
        result.LODMeshletCounts[0] = static_cast<uint32_t>(lod0MeshletCount);
        result.LODTriangleCounts[0] = static_cast<uint32_t>(result.Indices.size() / 3);

        // --- 3. 簡略化して段を積む(離散LOD) ------------------------------------------------
        //
        // 段ごとに独立したメッシュレット群を作る。増幅シェーダーは1段だけを選んで描く。
        //
        // **打ち切り条件を持つ。** 「4段作る」と決め打ちにすると、もう潰せない形状に対しても
        // ほぼ同じ三角形数の段を積んでしまい、.kgeomが太るだけで何の効果も無い段が残る。
        //
        // 【段ができないメッシュがあるのは正常】三角形どうしが辺を共有していない
        // (ばらばらの板や小片の集まりのような)メッシュは、潰せる辺がそもそも無いので
        // 三角形が1つも減らない。モン・サン＝ミシェルの島で実測したところ、11メッシュ中
        // 47,600三角形のものが 47,600 → 47,600(削減0%・誤差0.00000)だった。
        //
        // 【試して駄目だった対処を記録しておく】「法線やUVの継ぎ目で頂点が分かれているせいだ」
        // と考えて meshopt_generatePositionRemap で位置だけ繋ぎ直してから簡略化してみたが、
        // このメッシュの削減率は0%のまま変わらず、**逆に繋がっているメッシュのほうが悪化した**
        // (島の別メッシュで LOD3 が 780 → 1362 三角形になり、段が1つ減った)。
        // meshopt_SimplifyLockBorder を外しても同じく0%だったので、継ぎ目でも境界固定でもなく、
        // 形状そのものが簡略化できないという結論
        std::vector<uint32_t> lodIndices = result.Indices;

        for (uint32_t lod = 1; lod < requestedLODCount; ++lod)
        {
            const size_t currentTriangles = lodIndices.size() / 3;

            // メッシュレット1〜2個ぶんまで小さくなったらそれ以上刻む意味が無い
            // (1メッシュレット未満には分割できないので、段を足しても同じものが増えるだけ)
            if (currentTriangles <= static_cast<size_t>(kMeshletMaxTriangles) * 2)
            {
                break;
            }

            const size_t targetIndexCount = (lodIndices.size() / 2 / 3) * 3;   // 三角形を半分に(3の倍数へ丸める)
            if (targetIndexCount < 3)
            {
                break;
            }

            std::vector<uint32_t> simplified(lodIndices.size());
            float resultError = 0.0f;
            const size_t simplifiedIndexCount = meshopt_simplify(
                simplified.data(),
                lodIndices.data(),
                lodIndices.size(),
                result.Vertices[0].Position,
                result.Vertices.size(),
                sizeof(Vertex),
                targetIndexCount,
                kSimplifyTargetError,
                // 【境界を固定する】開いた辺(メッシュの縁)を動かさない。地形タイルのように
                // 隣のモデルと辺を共有しているものは、縁が動くとタイル同士の間に隙間が開く。
                //
                // 外した場合と実測で比べたが、**段の数も三角形数も改善しなかった**
                // (簡略化できないメッシュは外しても0%のまま)。得るものが無く、
                // タイル同士の隙間という実害だけが増えるので固定したままにする
                meshopt_SimplifyLockBorder,
                &resultError);

            simplified.resize(simplifiedIndexCount);

            // 【要求の9割に届かなければ打ち切る】それ以上潰せない形状に当たったということ。
            // ここで止めないと、ほぼ同じ三角形数の段が上限まで積まれる
            const size_t achieved = lodIndices.size() - simplifiedIndexCount;
            const size_t requested = lodIndices.size() - targetIndexCount;
            if (simplifiedIndexCount < 3 || requested == 0 || achieved * 10 < requested * 9)
            {
                break;
            }

            const uint32_t offsetBefore = static_cast<uint32_t>(result.Meshlets.size());
            const size_t appended = AppendMeshletsForLOD(result, simplified, lod, nullptr);
            if (appended == 0)
            {
                break;
            }

            result.LODMeshletOffsets[lod] = offsetBefore;
            result.LODMeshletCounts[lod] = static_cast<uint32_t>(appended);
            result.LODTriangleCounts[lod] = static_cast<uint32_t>(simplifiedIndexCount / 3);
            result.LODCount = lod + 1;

            lodIndices = std::move(simplified);
        }

        return result;
    }
}
