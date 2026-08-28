#include "ShaderEntryScanner.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>

namespace Kurenai::ShaderPacker
{
    namespace
    {
        using Assets::ShaderPackageStage;

        // 同じ (ステージ, 名前) を二重に登録しない
        bool Contains(const std::vector<ScannedEntry>& entries, const std::string& name, ShaderPackageStage stage)
        {
            for (const ScannedEntry& e : entries)
            {
                if (e.Name == name && e.Stage == stage)
                {
                    return true;
                }
            }
            return false;
        }

        // 名前だけで既出かを見る([numthreads]の判定でメッシュシェーダーを除外するために使う)
        bool ContainsName(const std::vector<ScannedEntry>& entries, const std::string& name)
        {
            for (const ScannedEntry& e : entries)
            {
                if (e.Name == name)
                {
                    return true;
                }
            }
            return false;
        }

        void AddUnique(std::vector<ScannedEntry>& entries, const std::string& name, ShaderPackageStage stage)
        {
            if (!Contains(entries, name, stage))
            {
                entries.push_back(ScannedEntry{ name, stage });
            }
        }

        bool ReadWholeFile(const std::filesystem::path& path, std::string& outText)
        {
            std::ifstream in(path, std::ios::binary | std::ios::ate);
            if (!in) { return false; }
            const std::streamoff size = in.tellg();
            if (size < 0) { return false; }
            in.seekg(0, std::ios::beg);
            outText.resize(static_cast<size_t>(size));
            if (size > 0 && !in.read(outText.data(), size)) { return false; }
            return true;
        }

        void ExpandRecursive(
            const std::filesystem::path& path,
            std::set<std::filesystem::path>& visited,
            std::string& out,
            std::string& outError)
        {
            std::error_code ec;
            std::filesystem::path key = std::filesystem::weakly_canonical(path, ec);
            if (ec) { key = path; }
            if (!visited.insert(key).second)
            {
                return;   // 展開済み(#pragma once 相当)
            }

            std::string text;
            if (!ReadWholeFile(path, text))
            {
                if (outError.empty())
                {
                    outError = "インクルードを読めませんでした: " + path.string();
                }
                return;
            }

            // パターン中に )" が現れるため、生文字列の区切りを既定から変えている
            const std::regex includeRe(R"RX((?:^|\n)[ \t]*#include[ \t]+"([^"]+)")RX", std::regex::ECMAScript);
            auto begin = std::sregex_iterator(text.begin(), text.end(), includeRe);
            const auto end = std::sregex_iterator();

            size_t copied = 0;
            for (auto it = begin; it != end; ++it)
            {
                const auto matchBegin = static_cast<size_t>(it->position(0));
                const auto matchLength = static_cast<size_t>(it->length(0));
                out.append(text, copied, matchBegin - copied);
                out.append("\n");
                copied = matchBegin + matchLength;

                ExpandRecursive(path.parent_path() / (*it)[1].str(), visited, out, outError);
            }
            out.append(text, copied, text.size() - copied);
            out.append("\n");
        }
    }

    std::string ExpandIncludes(const std::wstring& filePath, std::string& outError)
    {
        std::set<std::filesystem::path> visited;
        std::string expanded;
        ExpandRecursive(std::filesystem::path(filePath), visited, expanded, outError);
        return expanded;
    }

    const char* StagePrefix(ShaderPackageStage stage)
    {
        switch (stage)
        {
        case ShaderPackageStage::Vertex:
            return "vs";
        case ShaderPackageStage::Compute:
            return "cs";
        case ShaderPackageStage::Amplification:
            return "as";
        case ShaderPackageStage::Mesh:
            return "ms";
        case ShaderPackageStage::Pixel:
        default:
            return "ps";
        }
    }

    std::vector<ScannedEntry> ScanEntryPoints(const std::string& source)
    {
        std::vector<ScannedEntry> entries;

        const auto ecma = std::regex::ECMAScript;

        // 【(?m) の代わりに (?:^|\n) を使う】check-shaders.ps1 側は複数行モードの ^ で
        // 行頭を表しているが、MSVCの std::regex には multiline フラグが無い
        // (C++17で追加されたが、このツールチェーンの <regex> は持っていない)。
        // 「文字列の先頭、または改行の直後」と書き下すことで同じ意味にする。
        // 改行を1文字消費する点だけが違うが、拾うのはキャプチャした関数名だけなので影響しない

        // メッシュシェーダー: [outputtopology(...)] を持つ。
        // 【[numthreads]より先に見ること】メッシュシェーダーは両方の属性を持つため、
        // 順序を逆にするとコンピュートシェーダーとして拾ってしまう
        {
            const std::regex re(R"(\[outputtopology\s*\([^)]*\)\][\s\S]{0,200}?void\s+(\w+)\s*\()", ecma);
            for (auto it = std::sregex_iterator(source.begin(), source.end(), re); it != std::sregex_iterator(); ++it)
            {
                AddUnique(entries, (*it)[1].str(), ShaderPackageStage::Mesh);
            }
        }

        // [numthreads] を持つ関数。本体に DispatchMesh( があれば増幅シェーダー、
        // 無ければコンピュートシェーダー
        {
            const std::regex re(R"(\[numthreads\s*\([^)]*\)\]\s*\w+\s+(\w+)\s*\()", ecma);
            const std::regex dispatchMesh(R"(DispatchMesh\s*\()", ecma);
            for (auto it = std::sregex_iterator(source.begin(), source.end(), re); it != std::sregex_iterator(); ++it)
            {
                const std::string name = (*it)[1].str();
                if (ContainsName(entries, name))
                {
                    continue;
                }
                const auto offset = static_cast<size_t>(it->position(0));
                const std::string rest = source.substr(offset);
                const ShaderPackageStage stage =
                    std::regex_search(rest, dispatchMesh) ? ShaderPackageStage::Amplification : ShaderPackageStage::Compute;
                AddUnique(entries, name, stage);
            }
        }

        // ピクセル: "<type> PSxxx(...) : SV_Target" / "PSOutput PSxxx(...)" /
        // シャドウパスのようにレンダーターゲットを持たない "void PSMain(...)"
        {
            const std::regex re(R"((?:^|\n)\s*\w+\s+(PS\w*)\s*\([^)]*\)\s*:\s*SV_\w+)", ecma);
            for (auto it = std::sregex_iterator(source.begin(), source.end(), re); it != std::sregex_iterator(); ++it)
            {
                AddUnique(entries, (*it)[1].str(), ShaderPackageStage::Pixel);
            }
        }
        {
            const std::regex re(R"((?:^|\n)\s*PSOutput\s+(PS\w*)\s*\()", ecma);
            for (auto it = std::sregex_iterator(source.begin(), source.end(), re); it != std::sregex_iterator(); ++it)
            {
                AddUnique(entries, (*it)[1].str(), ShaderPackageStage::Pixel);
            }
        }
        {
            const std::regex re(R"((?:^|\n)\s*void\s+(PS\w*)\s*\()", ecma);
            for (auto it = std::sregex_iterator(source.begin(), source.end(), re); it != std::sregex_iterator(); ++it)
            {
                AddUnique(entries, (*it)[1].str(), ShaderPackageStage::Pixel);
            }
        }

        // 頂点: "<struct> VSxxx(...)"。戻り値がvoidのものは除き、
        // 引数にSV_があるか戻り値の型名がInput/Outputを含むものだけを採る
        {
            const std::regex re(R"((?:^|\n)\s*(\w+)\s+(VS\w*)\s*\(([^)]*)\))", ecma);
            const std::regex hasSV(R"(SV_)", ecma);
            const std::regex inputOutput(R"(Input|Output)", ecma);
            for (auto it = std::sregex_iterator(source.begin(), source.end(), re); it != std::sregex_iterator(); ++it)
            {
                const std::string returnType = (*it)[1].str();
                const std::string parameters = (*it)[3].str();
                if (returnType == "void")
                {
                    continue;
                }
                if (std::regex_search(parameters, hasSV) || std::regex_search(returnType, inputOutput))
                {
                    AddUnique(entries, (*it)[2].str(), ShaderPackageStage::Vertex);
                }
            }
        }

        return entries;
    }
}
