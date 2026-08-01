#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ModelSource.h"

// ベイク済みアンビエントオクルージョン(遮蔽マップ)の生成。
//
// 手順は次の3段階(詳細と設計判断はdocs/Architecture.htmlの22章):
//   1. xatlas(MIT)でメッシュごとに「重なりの無いライトマップUV」を生成し、
//      Vertex::UV1へ書き込む。ソースモデルのTEXCOORD0はタイリング前提で
//      面ごとに固有の場所を持たないため、AOの焼き先には使えない
//   2. モデル全体の三角形からBVHを構築する(遮蔽はメッシュ単位ではなくモデル全体で
//      決まるため。床のAOには壁による遮蔽が要る)
//   3. ライトマップUV空間の各テクセルから半球方向へレイを飛ばし、GPUのコンピュート
//      シェーダーで可視率を求める
//
// GPUデバイスはこのモジュールが自前で用意する小さなD3D11デバイスを使う
// (KurenaiEngine.dll側のBC7圧縮用デバイスは非公開のため)。
namespace KurenaiPacker
{
    struct OcclusionBakeOptions
    {
        // 出力する遮蔽マップの一辺(テクセル)。xatlasのパック解像度も兼ねる
        uint32_t Resolution = 512;

        // テクセルあたりのレイ本数。多いほどノイズが減るが線形に遅くなる
        uint32_t RayCount = 128;

        // bent normal用のテクセルあたりのレイ本数。
        // AO側(RayCount)より多いのは、スカラーの平均よりベクトル和のほうが収束が遅いため。
        // 0にするとbent normalを焼かない(従来どおり遮蔽マップだけを出力する)
        uint32_t BentNormalRayCount = 256;

        // レイの最大長。0 = 自動(モデルのバウンズ対角の10%)。
        // 無限長にすると屋外モデルで地面が空を向いた面まで一様に暗くなるため、
        // 「近傍の遮蔽だけを拾う」ようにこの距離で打ち切る
        float RayLength = 0.0f;

        // チャート境界のにじみを防ぐために、有効テクセルの外側へ色を広げる幅(テクセル)
        uint32_t DilationPixels = 4;

        // 【UV展開の内部分割】三角形数がこの閾値を超えたメッシュだけ、xatlasへ渡す前に
        // 空間分割して複数回AddMeshする。0 = 分割しない(従来どおり1メッシュ=1AddMesh)。
        //
        // 分割されるのは「xatlasに見せるトポロジー」だけで、遮蔽マップの粒度は変わらない
        // ―― 分割した全チャンクは1枚の共有アトラスへ詰められるため、出力は従来どおり
        // メッシュあたり遮蔽マップ1枚(MeshTextures[meshIndex])のままである。
        //
        // 【なぜ分割するか】xatlasのComputeChartsはメッシュごとに1タスクで並列化され、
        // かつチャート統合(mergeCharts)と種まき(Place seeds)がチャートグループ内で
        // 2乗に効く。連結した単一の巨大メッシュはこの両方を踏み抜き、Chinese Dragon
        // (871306三角形・1メッシュ)で394秒かかる一方、Sponza(26万三角形・25メッシュ)は
        // 13秒で終わる。分割すると2乗が表面化せず、コア数ぶんの並列化も効く(22.6.6節)
        uint32_t UnwrapSplitThreshold = 50000;

        // 分割後の1チャンクあたりの目標三角形数(22.6.6節)
        uint32_t UnwrapChunkTriangles = 100000;
    };

    struct OcclusionBakeResult
    {
        // メッシュごとの遮蔽マップ(R8、Resolution x Resolution、行優先)。
        // 空のvector = そのメッシュはUV展開に失敗して焼けなかった(遮蔽マップ無しとして扱う)
        std::vector<std::vector<uint8_t>> MeshTextures;

        // メッシュごとのbent normal(RGBA float32、Resolution x Resolution、行優先)。
        // 1テクセルあたり4要素で、.xyz = bRaw(正規化しない)、.w = 有効フラグ(0または1)。
        //
        // 【float32で持つ理由】書き出しはfp16だが、途中のダイレーションと検証をfp32で行う。
        // 量子化前の値でチェックリスト(length <= 1、aoB >= aoN、既存AOとの一致)を確認しないと、
        // 見つけた誤差がベイクのバグなのか量子化なのか切り分けられない。
        // テクセルあたり16バイトになるため、遮蔽マップ(1バイト)の16倍のメモリを使う
        std::vector<std::vector<float>> MeshBentNormals;

        uint32_t Resolution = 0;
        size_t BakedMeshCount = 0;
        size_t SkippedMeshCount = 0;
    };

    // sourceModelの各メッシュへライトマップUVを生成し、遮蔽マップを焼く。
    //
    // 【sourceModelは書き換えられる】xatlasの展開はチャート境界で頂点を複製するため、
    // 各メッシュのVertices/Indicesが展開結果で置き換わる(頂点数が増える)。
    // 呼び出し側はこの関数の後でジオメトリを書き出すこと。
    //
    // GPUデバイスの作成に失敗した場合など、ベイク自体が成立しない場合はstd::runtime_errorを投げる。
    // 個々のメッシュの展開失敗は例外にせず、そのメッシュだけスキップして続行する
    OcclusionBakeResult BakeOcclusion(SourceModel& sourceModel, const OcclusionBakeOptions& options);
}
