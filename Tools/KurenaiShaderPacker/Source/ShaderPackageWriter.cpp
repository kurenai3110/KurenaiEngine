#include "ShaderPackageWriter.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

#include "Core/StringUtil.h"

namespace fs = std::filesystem;

namespace Kurenai::ShaderPacker
{
    namespace
    {
        using namespace Kurenai::Assets;
        using Core::WideToUtf8;

        // StringPoolへ文字列を追加し、(offset,length)を返す。同じ文字列は使い回す
        struct StringPoolBuilder
        {
            std::string Pool;
            std::map<std::string, uint32_t> Offsets;

            void Add(const std::string& text, uint32_t& outOffset, uint32_t& outLength)
            {
                outLength = static_cast<uint32_t>(text.size());
                auto it = Offsets.find(text);
                if (it != Offsets.end())
                {
                    outOffset = it->second;
                    return;
                }
                outOffset = static_cast<uint32_t>(Pool.size());
                Offsets.emplace(text, outOffset);
                Pool += text;
            }
        };

        const char* VariantName(ShaderVariant variant)
        {
            switch (variant)
            {
            case ShaderVariant::Dxbc50:
                return "Dxbc50";
            case ShaderVariant::Dxil65:
                return "Dxil65";
            case ShaderVariant::Dxil66:
                return "Dxil66(bindless)";
            default:
                return "?";
            }
        }

        const char* StageName(ShaderPackageStage stage)
        {
            switch (stage)
            {
            case ShaderPackageStage::Vertex:
                return "vs";
            case ShaderPackageStage::Pixel:
                return "ps";
            case ShaderPackageStage::Compute:
                return "cs";
            case ShaderPackageStage::Amplification:
                return "as";
            case ShaderPackageStage::Mesh:
                return "ms";
            default:
                return "?";
            }
        }
    }

    bool WriteShaderPackage(
        const std::wstring& outPath,
        const std::vector<BuiltEntry>& entries,
        uint32_t variantMask,
        bool debugBuild,
        std::string& outError)
    {
        if (entries.empty())
        {
            outError = "エントリが1つもありません";
            return false;
        }

        StringPoolBuilder pool;
        std::vector<ShaderEntry> fileEntries;
        std::vector<uint8_t> bytecode;
        fileEntries.reserve(entries.size());

        for (const BuiltEntry& src : entries)
        {
            if (src.Bytecode.empty())
            {
                outError = "バイトコードが空のエントリがあります: " + src.Name;
                return false;
            }

            ShaderEntry entry{};
            pool.Add(src.Name, entry.NameOffset, entry.NameLength);
            pool.Add(src.Profile, entry.ProfileOffset, entry.ProfileLength);
            entry.Stage = static_cast<uint32_t>(src.Stage);
            entry.Variant = static_cast<uint32_t>(src.Variant);
            entry.BytecodeOffset = static_cast<uint32_t>(bytecode.size());
            entry.BytecodeSize = static_cast<uint32_t>(src.Bytecode.size());
            bytecode.insert(bytecode.end(), src.Bytecode.begin(), src.Bytecode.end());
            fileEntries.push_back(entry);
        }

        ShaderPackageHeader header{};
        std::memcpy(header.Magic, kShaderMagic, sizeof(kShaderMagic));
        header.Version = kShaderVersion;
        header.EntryStride = static_cast<uint32_t>(sizeof(ShaderEntry));
        header.EntryCount = static_cast<uint32_t>(fileEntries.size());
        header.StringPoolSize = static_cast<uint32_t>(pool.Pool.size());
        header.BytecodeSize = static_cast<uint32_t>(bytecode.size());
        header.Flags = debugBuild ? kShaderFlagDebugBuild : 0u;
        header.VariantMask = variantMask;

        std::error_code ec;
        const fs::path finalPath(outPath);
        fs::create_directories(finalPath.parent_path(), ec);

        // 中断で壊れたパッケージを残さないよう、一時ファイルへ書いてから置き換える
        const fs::path tempPath = finalPath.wstring() + L".tmp";
        {
            std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                outError = "出力ファイルを開けませんでした: " + WideToUtf8(tempPath.wstring());
                return false;
            }
            out.write(reinterpret_cast<const char*>(&header), sizeof(header));
            out.write(
                reinterpret_cast<const char*>(fileEntries.data()),
                static_cast<std::streamsize>(fileEntries.size() * sizeof(ShaderEntry)));
            if (!pool.Pool.empty())
            {
                out.write(pool.Pool.data(), static_cast<std::streamsize>(pool.Pool.size()));
            }
            if (!bytecode.empty())
            {
                out.write(reinterpret_cast<const char*>(bytecode.data()), static_cast<std::streamsize>(bytecode.size()));
            }
            if (!out)
            {
                outError = "書き出しに失敗しました: " + WideToUtf8(tempPath.wstring());
                return false;
            }
        }

        fs::rename(tempPath, finalPath, ec);
        if (ec)
        {
            // 既存ファイルがある場合、環境によってはrenameが失敗するので消してから再試行する
            fs::remove(finalPath, ec);
            std::error_code ec2;
            fs::rename(tempPath, finalPath, ec2);
            if (ec2)
            {
                outError = "出力先へ置き換えられませんでした: " + WideToUtf8(finalPath.wstring()) + " (" + ec2.message() + ")";
                fs::remove(tempPath, ec2);
                return false;
            }
        }

        return true;
    }

    bool DumpShaderPackage(const std::wstring& path, std::string& outError)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            outError = "開けませんでした: " + WideToUtf8(path);
            return false;
        }

        ShaderPackageHeader header{};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!in || std::memcmp(header.Magic, kShaderMagic, sizeof(kShaderMagic)) != 0)
        {
            outError = "マジックナンバーが不正です";
            return false;
        }

        std::vector<ShaderEntry> entries(header.EntryCount);
        in.read(
            reinterpret_cast<char*>(entries.data()),
            static_cast<std::streamsize>(entries.size() * sizeof(ShaderEntry)));
        std::string pool(header.StringPoolSize, '\0');
        if (header.StringPoolSize > 0u)
        {
            in.read(pool.data(), static_cast<std::streamsize>(pool.size()));
        }
        if (!in)
        {
            outError = "読み込みが途中で終わりました";
            return false;
        }

        std::cout << WideToUtf8(path) << "\n"
                  << "  Version       : " << header.Version << "\n"
                  << "  EntryCount    : " << header.EntryCount << "\n"
                  << "  StringPoolSize: " << header.StringPoolSize << "\n"
                  << "  BytecodeSize  : " << header.BytecodeSize << "\n"
                  << "  Flags         : " << header.Flags
                  << ((header.Flags & kShaderFlagDebugBuild) ? " (Debug)" : " (Release)") << "\n"
                  << "  VariantMask   : " << header.VariantMask << "\n";

        for (const ShaderEntry& e : entries)
        {
            const std::string name =
                (static_cast<uint64_t>(e.NameOffset) + e.NameLength <= pool.size()) ? pool.substr(e.NameOffset, e.NameLength) : "?";
            const std::string profile = (static_cast<uint64_t>(e.ProfileOffset) + e.ProfileLength <= pool.size())
                                            ? pool.substr(e.ProfileOffset, e.ProfileLength)
                                            : "?";
            std::cout << "  - " << name << " [" << StageName(static_cast<ShaderPackageStage>(e.Stage)) << "] "
                      << VariantName(static_cast<ShaderVariant>(e.Variant)) << " " << profile << " " << e.BytecodeSize
                      << " bytes\n";
        }
        return true;
    }
}
