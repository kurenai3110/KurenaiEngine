#pragma once

#include <cstdint>
#include <vector>

#include "Assets/ModelPackage.h"
#include "Assets/Vertex.h"

// メッシュをメッシュレット(頂点64個・三角形124個までの塊)へ分割する、KurenaiPacker側の
// オフライン処理。分割そのものはmeshoptimizer(ThirdParty/meshoptimizer)が行い、
// ここはその結果を.kgeomのブロック形式(Assets::MeshletEntry ほか)へ詰め替える。
//
// 【なぜオフラインなのか】分割は入力ジオメトリだけで決まり実行時に変化しない一方、
// 数十万三角形のメッシュでは秒単位かかる。KurenaiPackerが事前に済ませておけば
// ランタイムは読むだけで済む(遮蔽マップのベイクと同じ考え方)。
//
// 【meshoptimizerに依存するのはKurenaiPackerだけ】xatlasと同じく、KurenaiEngineの
// 各DLLはこのライブラリにリンクしない。ランタイムが扱うのは焼き上がった.kgeomのみ。
namespace KurenaiPacker
{
    // 1メッシュ分の分割結果。
    //
    // 【頂点とインデックスも返す】メッシュレット化の前に頂点キャッシュ/フェッチの
    // 最適化を掛けるため、頂点の並びが入力から変わる。さらにインデックスバッファは
    // メッシュレット順に並べ替えて返す(理由はModelPackage.hの.kgeom v3の説明)。
    // 呼び出し側は入力ではなくこちらを.kgeomへ書くこと
    struct MeshletBuildResult
    {
        std::vector<Kurenai::Assets::Vertex> Vertices;
        std::vector<uint32_t> Indices;

        // 以下はEnableMeshlets=falseの場合すべて空になる。
        // **離散LODの全段を連結して持つ**(LOD0が先頭)。段の範囲はLODMeshletOffsets/Counts
        std::vector<Kurenai::Assets::MeshletEntry> Meshlets;
        // 各メッシュレットが使う頂点の、Vertices内での番号
        std::vector<uint32_t> MeshletVertices;
        // 三角形1つにつき1要素。ローカル頂点番号3つをAssets::PackMeshletTriangleで詰めたもの
        std::vector<uint32_t> MeshletTriangles;

        // 実際に作れた段数(1〜kMaxMeshletLODCount)。メッシュレットが無ければ0
        uint32_t LODCount = 0;
        // 段ごとの、Meshlets配列内での開始番号と個数。LODCount未満の要素だけが有効
        uint32_t LODMeshletOffsets[Kurenai::Assets::kMaxMeshletLODCount] = {};
        uint32_t LODMeshletCounts[Kurenai::Assets::kMaxMeshletLODCount] = {};
        // 段ごとの三角形数。段が進むごとに減っていることの確認と、パック結果の表示に使う
        uint32_t LODTriangleCounts[Kurenai::Assets::kMaxMeshletLODCount] = {};
    };

    // meshopt_simplifyへ渡す許容誤差(メッシュの大きさに対する相対値)。
    //
    // 【誤差ではなく削減率で段を決める】離散LODは段ごとに三角形を半分にしていく作りなので、
    // 主導するのは目標三角形数のほう。この値は「そこまで潰すと形が壊れる」ときに
    // 簡略化を止めさせるための上限として効く。5%はメッシュの対角に対する割合で、
    // 段の切り替えが起きる距離ではまず知覚できない大きさ。
    // これを超えて潰せない場合は要求の9割に届かず、段の生成そのものが打ち切られる
    constexpr float kSimplifyTargetError = 0.05f;

    // verticesとindicesからメッシュレットを構築する。
    //
    // enableMeshletsがfalseの場合は入力をそのまま複製して返し、最適化も分割も行わない
    // (KurenaiPackerの--no-meshlets用。メッシュレット化が原因かどうかの切り分けに使う)。
    //
    // lodCount: 作る段数の上限(LOD0を含む)。1なら原寸のみ。0はenableMeshlets=falseと同じ。
    // 実際の段数は形状によってこれより少なくなる(それ以上潰せなければ打ち切る)。
    //
    // 三角形を持たないメッシュ(indicesが空、または3の倍数でない)は、警告を出さずに
    // 入力をそのまま返す。呼び出し側はMeshletsが空なら「メッシュレット無し」として扱えばよい
    MeshletBuildResult BuildMeshlets(
        const std::vector<Kurenai::Assets::Vertex>& vertices,
        const std::vector<uint32_t>& indices,
        bool enableMeshlets,
        unsigned int lodCount = 1);
}
