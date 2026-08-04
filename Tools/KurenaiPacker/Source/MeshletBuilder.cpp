#include "MeshletBuilder.h"

#include <cstring>

#include "meshoptimizer.h"

namespace KurenaiPacker
{
    using Kurenai::Assets::MeshletEntry;
    using Kurenai::Assets::PackMeshletTriangle;
    using Kurenai::Assets::Vertex;
    using Kurenai::Assets::kMeshletConeWeight;
    using Kurenai::Assets::kMeshletMaxTriangles;
    using Kurenai::Assets::kMeshletMaxVertices;

    MeshletBuildResult BuildMeshlets(
        const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, bool enableMeshlets)
    {
        MeshletBuildResult result;
        result.Vertices = vertices;
        result.Indices = indices;

        if (!enableMeshlets || vertices.empty() || indices.empty() || (indices.size() % 3) != 0)
        {
            return result;
        }

        // --- 1. 頂点の並びを最適化する -------------------------------------------------------
        //
        // メッシュレット化の前に掛けるのが重要。buildMeshletsは入力の三角形の並び順を
        // 手掛かりに隣接した三角形を1つの塊へまとめるため、キャッシュ最適化で
        // 局所性が上がっているほど「まとまりのよい」メッシュレットになる。
        //
        // optimizeVertexFetchは頂点配列そのものを並べ替える。.kgeomへ書くのは
        // この並べ替え後の頂点なので、呼び出し側は入力ではなくresult.Verticesを使うこと
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

        // --- 2. メッシュレットへ分割する -----------------------------------------------------
        //
        // 位置はVertexの先頭12バイト(Vertex::Position)にあるため、先頭を指してストライドを渡す
        const float* positions = result.Vertices.empty() ? nullptr : result.Vertices[0].Position;

        const size_t maxMeshlets =
            meshopt_buildMeshletsBound(result.Indices.size(), kMeshletMaxVertices, kMeshletMaxTriangles);

        std::vector<meshopt_Meshlet> rawMeshlets(maxMeshlets);
        // 最悪ケースの必要量はどちらもindex_count(vertex_countではない)
        std::vector<uint32_t> rawMeshletVertices(result.Indices.size());
        std::vector<unsigned char> rawMeshletTriangles(result.Indices.size());

        const size_t meshletCount = meshopt_buildMeshlets(
            rawMeshlets.data(),
            rawMeshletVertices.data(),
            rawMeshletTriangles.data(),
            result.Indices.data(),
            result.Indices.size(),
            positions,
            result.Vertices.size(),
            sizeof(Vertex),
            kMeshletMaxVertices,
            kMeshletMaxTriangles,
            kMeshletConeWeight);

        if (meshletCount == 0)
        {
            return result;
        }
        rawMeshlets.resize(meshletCount);

        // --- 3. .kgeomのブロック形式へ詰め替える ---------------------------------------------
        //
        // meshoptimizerが返すmeshlet_trianglesは、メッシュレットごとに4バイト境界へ
        // 揃うようパディングが入る(triangle_offsetは3の倍数とは限らない)。
        // こちらは1三角形=1要素のuint32配列へ詰め直すので、パディングを持ち込まないよう
        // オフセットは自前で数え直す。
        //
        // 同時に、インデックスバッファをメッシュレット順に組み立て直す。
        // レイトレーシング側がヒットした三角形番号から所属メッシュレットを
        // 二分探索で引けるようにするため(ModelPackage.hの.kgeom v3の説明)
        result.Meshlets.reserve(meshletCount);
        result.MeshletVertices.reserve(result.Indices.size());
        result.MeshletTriangles.reserve(result.Indices.size() / 3);

        std::vector<uint32_t> reorderedIndices;
        reorderedIndices.reserve(result.Indices.size());

        for (const meshopt_Meshlet& raw : rawMeshlets)
        {
            uint32_t* meshletVertices = &rawMeshletVertices[raw.vertex_offset];
            unsigned char* meshletTriangles = &rawMeshletTriangles[raw.triangle_offset];

            // メッシュレット内での頂点・三角形の並びを整えると、ラスタライザのスループットと
            // 頂点の再利用率が上がる。ローカル番号の対応もこの中で整合が保たれる
            meshopt_optimizeMeshlet(meshletVertices, meshletTriangles, raw.triangle_count, raw.vertex_count);

            MeshletEntry entry{};
            entry.VertexOffset = static_cast<uint32_t>(result.MeshletVertices.size());
            entry.TriangleOffset = static_cast<uint32_t>(result.MeshletTriangles.size());
            entry.VertexCount = raw.vertex_count;
            entry.TriangleCount = raw.triangle_count;

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

                // グローバルなインデックスバッファ側は2段の間接参照を解決して書く
                reorderedIndices.push_back(meshletVertices[a]);
                reorderedIndices.push_back(meshletVertices[b]);
                reorderedIndices.push_back(meshletVertices[c]);
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
        return result;
    }
}
