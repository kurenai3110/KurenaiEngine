#include "RHIShaderPackage.h"

#include <stdexcept>

#include "Core/Logger.h"
#include "Core/StringUtil.h"

namespace Kurenai::RHI
{
    namespace
    {
        const char* VariantName(Assets::ShaderVariant variant)
        {
            switch (variant)
            {
            case Assets::ShaderVariant::Dxbc50:
                return "Dxbc50(SM 5.0)";
            case Assets::ShaderVariant::Dxil65:
                return "Dxil65(SM 6.5)";
            case Assets::ShaderVariant::Dxil66:
                return "Dxil66(SM 6.6 / bindless)";
            default:
                return "?";
            }
        }
    }

    std::vector<uint8_t> LoadShaderBytecode(
        Assets::ShaderPackageCache& cache,
        const ShaderDesc& desc,
        Assets::ShaderVariant variant,
        const char* backendTag)
    {
        const std::string context =
            Core::WideToUtf8(desc.FilePath) + " (" + desc.EntryPoint + ", " + VariantName(variant) + ")";

        const Assets::ShaderPackageData* package = nullptr;
        try
        {
            package = &cache.Get(desc.FilePath);
        }
        catch (const std::exception& e)
        {
            const std::string message = std::string("シェーダーパッケージを読み込めませんでした: ") + e.what();
            Core::Logger::Error(backendTag, message);
            throw std::runtime_error(message);
        }

        const Assets::ShaderPackageEntry* entry =
            Assets::FindShaderEntry(*package, desc.EntryPoint, ToPackageStage(desc.Stage), variant);
        if (!entry)
        {
            // ここに来るのは「パッケージは読めたが、要求したエントリが入っていない」場合。
            // エンジンが要求するエントリをパッカーが焼き漏らしているか(検出規則の取りこぼし)、
            // 出力フォルダに古い.kshaderが残っているかのどちらか。どちらも原因が分かりにくいので、
            // 何を探して見つからなかったのかを必ず具体的に残す
            const std::string message =
                "シェーダーパッケージに該当するエントリがありません: " + context +
                " ―― パッカーの焼き漏らしか、出力フォルダの.kshaderが古い可能性があります";
            Core::Logger::Error(backendTag, message);
            throw std::runtime_error(message);
        }

        const uint8_t* begin = package->Bytecode.data() + entry->BytecodeOffset;
        return std::vector<uint8_t>(begin, begin + entry->BytecodeSize);
    }
}
