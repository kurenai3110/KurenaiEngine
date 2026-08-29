#include "ShaderLoader.h"

#include <cstring>
#include <fstream>
#include <stdexcept>

#include "Core/Logger.h"
#include "Core/StringUtil.h"

namespace Kurenai::Assets
{
    namespace
    {
        using Core::WideToUtf8;

        // 1つの.kshaderに収める上限。壊れたファイルのEntryCount/サイズを信じて
        // 巨大なresizeを行い、確保に失敗して落ちるのを防ぐための関所。
        // 実際の値は現行最大のSky.hlsl系でもエントリ数十・数百KB程度で、桁が2つ違う
        constexpr uint32_t kMaxEntryCount = 4096u;
        constexpr uint32_t kMaxStringPoolSize = 1u << 20;    // 1MB
        constexpr uint32_t kMaxBytecodeSize = 64u << 20;     // 64MB

        // StringPool(offset,length)からUTF-8部分文字列を安全に取り出す。壊れた.kshaderが
        // 範囲外を指していてもプロセスを異常終了させないよう、必ず範囲チェックを行う
        // (ModelLoader.cpp / ShowLoader.cppの同名関数と同じ扱い)
        std::string ReadPoolString(const std::string& pool, uint32_t offset, uint32_t length, const char* fieldNameForError)
        {
            if (static_cast<uint64_t>(offset) + length > pool.size())
            {
                throw std::runtime_error(std::string("シェーダーのStringPool参照が範囲外です: ") + fieldNameForError);
            }
            return pool.substr(offset, length);
        }
    }

    ShaderPackageData LoadShaderPackage(const std::wstring& filePath)
    {
        std::ifstream in;
        in.open(filePath, std::ios::binary);
        if (!in.is_open())
        {
            throw std::runtime_error("シェーダーパッケージを開けませんでした: " + WideToUtf8(filePath));
        }

        ShaderPackageHeader header{};
        std::vector<ShaderEntry> entries;
        std::string stringPool;
        ShaderPackageData data{};

        try
        {
            in.exceptions(std::ios::failbit | std::ios::badbit);

            in.read(reinterpret_cast<char*>(&header), sizeof(header));
            if (std::memcmp(header.Magic, kShaderMagic, sizeof(kShaderMagic)) != 0)
            {
                throw std::runtime_error("マジックナンバーが不正です");
            }
            if (header.Version != kShaderVersion)
            {
                throw std::runtime_error(
                    "バージョンが対応していません(ファイル: " + std::to_string(header.Version) +
                    ", ランタイム: " + std::to_string(kShaderVersion) + ")");
            }
            if (header.EntryStride != sizeof(ShaderEntry))
            {
                throw std::runtime_error("エントリのレイアウトが現在のランタイムと一致しません");
            }
            if (header.EntryCount == 0u)
            {
                throw std::runtime_error("エントリが1つも入っていません");
            }
            if (header.EntryCount > kMaxEntryCount || header.StringPoolSize > kMaxStringPoolSize ||
                header.BytecodeSize > kMaxBytecodeSize)
            {
                throw std::runtime_error(
                    "ヘッダーのサイズが上限を超えています(エントリ: " + std::to_string(header.EntryCount) +
                    ", StringPool: " + std::to_string(header.StringPoolSize) +
                    ", バイトコード: " + std::to_string(header.BytecodeSize) + ")");
            }

            entries.resize(header.EntryCount);
            in.read(
                reinterpret_cast<char*>(entries.data()),
                static_cast<std::streamsize>(entries.size() * sizeof(ShaderEntry)));

            stringPool.resize(header.StringPoolSize);
            if (header.StringPoolSize > 0u)
            {
                in.read(stringPool.data(), static_cast<std::streamsize>(stringPool.size()));
            }

            data.Bytecode.resize(header.BytecodeSize);
            if (header.BytecodeSize > 0u)
            {
                in.read(reinterpret_cast<char*>(data.Bytecode.data()), static_cast<std::streamsize>(data.Bytecode.size()));
            }
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error(
                "シェーダーパッケージの読み込みに失敗しました(" + WideToUtf8(filePath) + "): " + e.what());
        }

        data.VariantMask = header.VariantMask;
        data.DebugBuild = (header.Flags & kShaderFlagDebugBuild) != 0u;

        data.Entries.reserve(entries.size());
        for (const ShaderEntry& src : entries)
        {
            if (src.Stage > static_cast<uint32_t>(ShaderPackageStage::Mesh))
            {
                throw std::runtime_error(
                    "未知のシェーダーステージです(" + std::to_string(src.Stage) + "): " + WideToUtf8(filePath));
            }
            if (src.Variant >= kShaderVariantCount)
            {
                throw std::runtime_error(
                    "未知のバリアントです(" + std::to_string(src.Variant) + "): " + WideToUtf8(filePath));
            }
            // バイトコードの範囲は必ず検査する。ここを信じて後段でポインタ演算をするため
            if (static_cast<uint64_t>(src.BytecodeOffset) + src.BytecodeSize > data.Bytecode.size())
            {
                throw std::runtime_error("バイトコードの参照が範囲外です: " + WideToUtf8(filePath));
            }
            if (src.BytecodeSize == 0u)
            {
                throw std::runtime_error("バイトコードが空のエントリがあります: " + WideToUtf8(filePath));
            }

            ShaderPackageEntry entry{};
            entry.Name = ReadPoolString(stringPool, src.NameOffset, src.NameLength, "EntryPoint");
            entry.Profile = ReadPoolString(stringPool, src.ProfileOffset, src.ProfileLength, "Profile");
            entry.Stage = static_cast<ShaderPackageStage>(src.Stage);
            entry.Variant = static_cast<ShaderVariant>(src.Variant);
            entry.BytecodeOffset = src.BytecodeOffset;
            entry.BytecodeSize = src.BytecodeSize;
            data.Entries.push_back(std::move(entry));
        }

        return data;
    }

    const ShaderPackageEntry* FindShaderEntry(
        const ShaderPackageData& package,
        const std::string& entryPoint,
        ShaderPackageStage stage,
        ShaderVariant variant)
    {
        for (const ShaderPackageEntry& entry : package.Entries)
        {
            if (entry.Variant == variant && entry.Stage == stage && entry.Name == entryPoint)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    const ShaderPackageData& ShaderPackageCache::Get(const std::wstring& filePath)
    {
        auto it = m_Packages.find(filePath);
        if (it != m_Packages.end())
        {
            return it->second;
        }

        ShaderPackageData data = LoadShaderPackage(filePath);
        auto inserted = m_Packages.emplace(filePath, std::move(data));
        return inserted.first->second;
    }

    void ShaderPackageCache::Clear()
    {
        if (m_Packages.empty())
        {
            return;
        }

        Core::Logger::Info(
            "ShaderLoader",
            "シェーダーパッケージのキャッシュを解放しました(" + std::to_string(m_Packages.size()) + "ファイル / " +
                std::to_string(TotalBytecodeSize() / 1024u) + "KB)");
        m_Packages.clear();
    }

    size_t ShaderPackageCache::TotalBytecodeSize() const
    {
        size_t total = 0;
        for (const auto& pair : m_Packages)
        {
            total += pair.second.Bytecode.size();
        }
        return total;
    }
}
