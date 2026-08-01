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
