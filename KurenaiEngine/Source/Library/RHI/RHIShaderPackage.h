#pragma once

#include <cstdint>
#include <vector>

#include "Assets/ShaderLoader.h"
#include "RHIDesc.h"
#include "RHIEnums.h"

namespace Kurenai::RHI
{
    // .kshader からバイトコードを取り出す、DX11 / DX12 共通の処理。
    //
    // シェーダーの生成経路は「実行時に .hlsl をコンパイルする」から
    // 「ビルド時に焼いた .kshader を読む」へ置き換わっており、実行時コンパイルは残していない。
    // ShaderDesc::FilePath が指すのも .hlsl ではなく .kshader

    // RHIのステージを .kshader のステージ番号へ写す。
    // 【値を揃えているだけの変換をわざわざ書く理由】ファイル形式をRHIのenumの並びに
    // 依存させないため。RHI側でenumを並び替えても焼き済みのパッケージの意味は変わらない
    inline Assets::ShaderPackageStage ToPackageStage(ShaderStage stage)
    {
        switch (stage)
        {
        case ShaderStage::Vertex:
            return Assets::ShaderPackageStage::Vertex;
        case ShaderStage::Pixel:
            return Assets::ShaderPackageStage::Pixel;
        case ShaderStage::Compute:
            return Assets::ShaderPackageStage::Compute;
        case ShaderStage::Amplification:
            return Assets::ShaderPackageStage::Amplification;
        case ShaderStage::Mesh:
        default:
            return Assets::ShaderPackageStage::Mesh;
        }
    }

    // パッケージを(必要なら読み込んで)指定バリアントのバイトコードを返す。
    // 見つからない場合はログを出して std::runtime_error を投げる
    // (シェーダーが1つでも作れなければ描画は成立しないため、従来のCreateShaderと同じ扱い)。
    // backendTag はログのカテゴリ("DX11" / "DX12")
    std::vector<uint8_t> LoadShaderBytecode(
        Assets::ShaderPackageCache& cache,
        const ShaderDesc& desc,
        Assets::ShaderVariant variant,
        const char* backendTag);
}
