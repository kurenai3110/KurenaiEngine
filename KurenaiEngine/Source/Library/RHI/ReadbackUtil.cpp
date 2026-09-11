#include "ReadbackUtil.h"

#include <cstring>
#include <string>

#include "Core/Logger.h"
#include "RHIReadbackFormat.h"

namespace Kurenai::RHI
{
    bool ValidateTextureReadbackRequest(
        const char* backendTag, bool isReadbackTexture, const void* outData, uint32_t sizeInBytes,
        const TextureReadbackDesc& desc, uint32_t& outTightRowPitch)
    {
        outTightRowPitch = 0;

        if (!isReadbackTexture)
        {
            Core::Logger::Error(backendTag, "ReadbackData: リードバック用ではないテクスチャから読もうとしました");
            return false;
        }
        if (outData == nullptr || sizeInBytes == 0)
        {
            Core::Logger::Error(backendTag, "ReadbackData: 出力先がnullptrかサイズが0です");
            return false;
        }

        // パディングを剥がしたあとの必要バイト数。呼び出し側にはこれを要求する
        const uint32_t tightRowPitch = desc.Width * desc.BytesPerTexel;
        const uint64_t tightTotal = static_cast<uint64_t>(tightRowPitch) * desc.Height;
        if (sizeInBytes < tightTotal)
        {
            Core::Logger::Error(
                backendTag,
                "ReadbackData: 出力先のサイズ(" + std::to_string(sizeInBytes) + ")が必要量(" +
                    std::to_string(tightTotal) + ")に足りません");
            return false;
        }

        outTightRowPitch = tightRowPitch;
        return true;
    }

    bool ValidateBufferReadbackRequest(
        const char* backendTag, bool isReadbackBuffer, const void* outData, uint32_t sizeInBytes,
        uint32_t bufferSizeInBytes)
    {
        if (!isReadbackBuffer)
        {
            Core::Logger::Error(backendTag, "ReadbackData: BufferUsage::Readback以外のバッファから読もうとしました");
            return false;
        }
        if (outData == nullptr || sizeInBytes == 0)
        {
            Core::Logger::Error(backendTag, "ReadbackData: 出力先がnullptrかサイズが0です");
            return false;
        }
        if (sizeInBytes > bufferSizeInBytes)
        {
            Core::Logger::Error(
                backendTag,
                "ReadbackData: 要求サイズ(" + std::to_string(sizeInBytes) + ")がバッファサイズ(" +
                    std::to_string(bufferSizeInBytes) + ")を超えています");
            return false;
        }
        return true;
    }

    void CopyReadbackRowsTightly(
        void* outData, const void* mappedData, uint32_t tightRowPitch, uint32_t srcRowPitch, uint32_t height)
    {
        const auto* src = static_cast<const uint8_t*>(mappedData);
        auto* dst = static_cast<uint8_t*>(outData);
        for (uint32_t y = 0; y < height; ++y)
        {
            std::memcpy(
                dst + static_cast<size_t>(y) * tightRowPitch,
                src + static_cast<size_t>(y) * srcRowPitch,
                tightRowPitch);
        }
    }

    bool ValidateTextureReadbackCopy(
        const char* backendTag, uint32_t mipLevel, uint32_t arraySlice, uint32_t srcWidth, uint32_t srcHeight,
        uint32_t srcMipLevels, uint32_t srcArraySize, uint32_t dstWidth, uint32_t dstHeight)
    {
        if (mipLevel >= srcMipLevels || arraySlice >= srcArraySize)
        {
            Core::Logger::Error(
                backendTag,
                "CopyTextureToReadback: サブリソースの指定が範囲外です (mipLevel=" + std::to_string(mipLevel) + "/" +
                    std::to_string(srcMipLevels) + ", arraySlice=" + std::to_string(arraySlice) + "/" +
                    std::to_string(srcArraySize) + ")");
            return false;
        }

        const uint32_t mipWidth = ReadbackMipExtent(srcWidth, mipLevel);
        const uint32_t mipHeight = ReadbackMipExtent(srcHeight, mipLevel);
        if (dstWidth != mipWidth || dstHeight != mipHeight)
        {
            Core::Logger::Error(
                backendTag,
                "CopyTextureToReadback: 受け皿の寸法(" + std::to_string(dstWidth) + "x" + std::to_string(dstHeight) +
                    ")がコピー元のミップ" + std::to_string(mipLevel) + "(" + std::to_string(mipWidth) + "x" +
                    std::to_string(mipHeight) +
                    ")と一致しません。CreateReadbackTextureに渡したミップと同じものを指定してください");
            return false;
        }
        return true;
    }

    bool PrepareReadbackTextureDesc(
        const char* backendTag, uint32_t mipLevel, uint32_t srcMipLevels, uint32_t srcWidth, uint32_t srcHeight,
        DXGI_FORMAT srcFormat, TextureReadbackDesc& outDesc)
    {
        outDesc = TextureReadbackDesc{};

        if (mipLevel >= srcMipLevels)
        {
            Core::Logger::Error(
                backendTag,
                "CreateReadbackTexture: ミップレベルが範囲外です (mipLevel=" + std::to_string(mipLevel) +
                    ", MipLevels=" + std::to_string(srcMipLevels) + ")");
            return false;
        }

        const uint32_t mipWidth = ReadbackMipExtent(srcWidth, mipLevel);
        const uint32_t mipHeight = ReadbackMipExtent(srcHeight, mipLevel);

        const TextureReadbackDesc desc = DescribeReadbackFormat(srcFormat, mipWidth, mipHeight);
        if (desc.ElementType == TextureElementType::Unknown)
        {
            // BC圧縮のアセットテクスチャなど。**黙って0で埋めた結果を返さない**
            Core::Logger::Error(
                backendTag,
                "CreateReadbackTexture: 対応していないフォーマットです (DXGI_FORMAT=" +
                    std::to_string(static_cast<int>(srcFormat)) +
                    ")。RHIReadbackFormat.hの対応表に無いため読み出せません");
            return false;
        }

        outDesc = desc;
        return true;
    }
}
