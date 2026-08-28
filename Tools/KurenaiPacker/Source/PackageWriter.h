#pragma once

#include <cstdint>
#include <string>

#include "ModelSource.h"
#include "OcclusionBaker.h"

// SourceModel(assimp解析結果)を.kmodel/.kgeom/.ktexとして書き出す。
// テクスチャのデコード・ミップ生成・GPU BC7圧縮はKurenaiEngine.dllのRHI::TextureImageを
// そのまま利用する(KurenaiPacker自身はGPUデバイスを一切自前で持たない。
// TextureImage内部の専用D3D11デバイスがヘッドレスに初期化・利用される)。
namespace KurenaiPacker
{
    struct PackOptions
    {
        // 既存の.ktexがあっても再圧縮して上書きする。省略時は存在確認のみで
        // スキップし、高速に再パックする(.ktexは元画像のタイムスタンプを持たないため、
        // 更新の要否は「再実行するかどうか」というビルド上の判断に一本化される)
        bool Force = false;

        // テクスチャ処理のワーカースレッド数。0 = 自動(論理コア数、上限8)
        unsigned int JobCount = 0;

        // BakeOcclusion()の結果。nullptrならベイクしていない(遮蔽マップはソースモデルが
        // 持っているものだけを使う)。非nullptrの場合、焼けたメッシュについては
        // ソースモデル側のocclusionTextureより焼いた結果を優先する
        const OcclusionBakeResult* BakedOcclusion = nullptr;

        // メッシュレット(メッシュシェーダー用の分割情報)を生成するか。
        // falseにすると頂点キャッシュ最適化もインデックスの並べ替えも行わず、
        // 頂点/インデックスを入力のまま書き出す(メッシュレット関連のフィールドは0)。
        //
        // 既定でtrueなのは、生成しても従来の描画経路にまったく影響が無いため
        // (増えるのは.kgeomの容量だけで、メッシュシェーダー非対応環境では読まれない)。
        // falseにする用途は「見た目の異常がメッシュレット化に由来するかどうか」の切り分け
        bool EnableMeshlets = true;

        // メッシュレットの離散LODを何段まで作るか(LOD0を含む)。1なら従来どおり原寸のみ。
        // 0はEnableMeshlets=falseと同じ扱い。上限はAssets::kMaxMeshletLODCount。
        //
        // 既定で段を作るのは、フォーマットのバージョンを上げるたびに全アセットの再パックが
        // 要るため。あとから段を足したくなったときに847モデルの変換をやり直さずに済む
        unsigned int MeshletLODCount = 4;

        // 埋め込みテクスチャの取り出し先ディレクトリ(SourceModel::EmbeddedTextures->Directory())。
        // ここから始まるパスは入力モデルの外にあるため、そのままでは_External/へ平坦化されて
        // しまう。埋め込み由来と分かるように_Embedded/へ振り分けるために使う。空なら埋め込み無し
        std::wstring EmbeddedTextureDirectory;
    };

    struct PackResult
    {
        size_t MeshCount = 0;
        size_t VertexCount = 0;
        size_t IndexCount = 0;
        size_t TextureRequested = 0;   // ユニークな(パス,sRGB)の組の数
        size_t TextureGenerated = 0;   // 新規にBC7圧縮して.ktexを書いた数
        size_t TextureSkippedExisting = 0; // 既存の.ktexをそのまま使った数(Force=false時)
        size_t TextureFailed = 0;      // 読み込み失敗でフォールバック(-1)になった数
        size_t OcclusionBaked = 0;     // ベイクした遮蔽マップを.ktexとして書いた数
        size_t BentNormalBaked = 0;    // ベイクしたbent normalを.ktexとして書いた数
        size_t MeshletCount = 0;       // 生成したメッシュレットの総数(全LOD段の合計)
        size_t MeshletLOD0Count = 0;   // うちLOD0(原寸)のぶん。描画が実際に回すのはこちら
        // 段ごとの三角形数(要素0がLOD0)。段を進めるごとに減っていることの確認に使う
        size_t MeshletTrianglesByLOD[4] = { 0, 0, 0, 0 };
    };

    // sourceModelを指定した.kmodelパスへ書き出す。
    // outputKModelPath: 出力する.kmodelのフルパス。この親ディレクトリが.kgeom/.ktexの
    //                   ミラー先ルートになる(存在しない場合は作成する)
    // sourceModelDirectory: 入力モデルファイルの親ディレクトリ(絶対パス)。テクスチャの
    //                   出力先を入力からの相対パスでミラーするために使う
    // 失敗時(出力先へ書き込めない等)はstd::runtime_errorを投げる
    PackResult WriteModelPackage(
        const SourceModel& sourceModel,
        const std::wstring& outputKModelPath,
        const std::wstring& sourceModelDirectory,
        const PackOptions& options);
}
