#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Assets/ShaderPackage.h"

namespace Kurenai::ShaderPacker
{
    // 焼き上がった1エントリ(1バリアント分)
    struct BuiltEntry
    {
        std::string Name;                    // エントリポイント名
        std::string Profile;                 // "ps_6_6" 等
        Assets::ShaderPackageStage Stage = Assets::ShaderPackageStage::Vertex;
        Assets::ShaderVariant Variant = Assets::ShaderVariant::Dxbc50;
        std::vector<uint8_t> Bytecode;
    };

    // .kshader を書き出す。書式の定義は Assets/ShaderPackage.h を参照。
    // 中間ディレクトリが無ければ作る。失敗時は false を返し outError に理由を入れる。
    //
    // 【一時ファイルへ書いてから置き換える】ビルド中に中断されたとき、
    // 中途半端な .kshader が残ると次回の増分判定で「新しいから作り直さなくてよい」と
    // 判断され、壊れたパッケージを掴んだまま起動することになる
    bool WriteShaderPackage(
        const std::wstring& outPath,
        const std::vector<BuiltEntry>& entries,
        uint32_t variantMask,
        bool debugBuild,
        std::string& outError);

    // .kshader の中身を標準出力へ印字する(--dump)。読めなければ false
    bool DumpShaderPackage(const std::wstring& path, std::string& outError);
}
