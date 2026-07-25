#include "TextureImage.h"

#include <Windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <DirectXTex.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "Core/Logger.h"

namespace Kurenai::RHI
{
    namespace
    {
        std::string WideToUtf8(const std::wstring& wide)
        {
            if (wide.empty())
            {
                return {};
            }

            int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string narrow(length, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, narrow.data(), length, nullptr, nullptr);
            narrow.resize(length - 1);
            return narrow;
        }

        void ThrowIfFailed(HRESULT hr, const std::string& message)
        {
            if (FAILED(hr))
            {
                char hrText[16];
                std::snprintf(hrText, sizeof(hrText), "0x%08X", static_cast<unsigned int>(hr));
                const std::string fullMessage = message + " (HRESULT: " + hrText + ")";
                Core::Logger::Error("TextureImage", fullMessage);
                throw std::runtime_error(fullMessage);
            }
        }

        bool HasExtension(const std::wstring& path, const wchar_t* extension)
        {
            const size_t extLen = wcslen(extension);
            if (path.size() < extLen)
            {
                return false;
            }
            return _wcsicmp(path.c_str() + (path.size() - extLen), extension) == 0;
        }

        bool GetFileTimeAndSize(const std::wstring& path, uint64_t& outTime, uint64_t& outSize)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            {
                return false;
            }
            outTime = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
            outSize = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            return true;
        }

        // sRGBの有無でキャッシュファイル名を分ける。同じ画像ファイルがベースカラー(sRGB)と
        // メタリック/ラフネス(linear)の両方から参照された場合に、片方のキャッシュをもう片方が
        // 誤って読んでしまう(色空間の解釈を取り違える)ことを防ぐため
        std::wstring GetCachePath(const std::wstring& filePath, bool sRGB)
        {
            return filePath + (sRGB ? L".srgb.ktexcache" : L".linear.ktexcache");
        }

        std::wstring GetCacheTempPath(const std::wstring& filePath, bool sRGB)
        {
            return GetCachePath(filePath, sRGB) + L".tmp";
        }

        constexpr char kTexCacheMagic[4] = { 'K', 'T', 'C', '1' };
        // BC7圧縮+ミップ生成済みのDDSペイロードをそのまま格納する形式のバージョン
        constexpr uint32_t kTexCacheVersion = 1;

        struct TexCacheHeader
        {
            char Magic[4];
            uint32_t Version;
            uint64_t SourceFileTime;
            uint64_t SourceFileSize;
            uint64_t PayloadSize;
        };

        // キャッシュの読み込みに失敗した場合(存在しない/バージョン不一致/ソース更新/壊れている)は
        // falseを返し、呼び出し側は通常のWICデコード経路にフォールバックする
        bool TryLoadDDSCache(
            const std::wstring& cachePath,
            uint64_t sourceTime,
            uint64_t sourceSize,
            DirectX::TexMetadata& outMetadata,
            DirectX::ScratchImage& outImage)
        {
            std::ifstream in(cachePath, std::ios::binary);
            if (!in.is_open())
            {
                return false;
            }

            try
            {
                in.exceptions(std::ios::failbit | std::ios::badbit);

                TexCacheHeader header{};
                in.read(reinterpret_cast<char*>(&header), sizeof(header));
                if (std::memcmp(header.Magic, kTexCacheMagic, sizeof(kTexCacheMagic)) != 0 ||
                    header.Version != kTexCacheVersion ||
                    header.SourceFileTime != sourceTime ||
                    header.SourceFileSize != sourceSize ||
                    header.PayloadSize == 0)
                {
                    return false;
                }

                std::vector<uint8_t> payload(header.PayloadSize);
                in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));

                return SUCCEEDED(DirectX::LoadFromDDSMemory(payload.data(), payload.size(), DirectX::DDS_FLAGS_NONE, &outMetadata, outImage));
            }
            catch (const std::exception& e)
            {
                Core::Logger::Warning("TextureImage", "テクスチャキャッシュの読み込みに失敗したため、通常経路にフォールバックします: " + std::string(e.what()));
                return false;
            }
        }

        // キャッシュの書き込みに失敗しても例外にはしない(次回また生成を試みるだけでよいため)。
        // 一時ファイルへ書いてから完了時のみ本来のパスへリネームする(ModelLoaderの.kmodelcacheと同じ設計)
        void WriteDDSCacheBestEffort(const std::wstring& filePath, bool sRGB, uint64_t sourceTime, uint64_t sourceSize, const DirectX::ScratchImage& compressed)
        {
            DirectX::Blob blob;
            HRESULT hr = DirectX::SaveToDDSMemory(compressed.GetImages(), compressed.GetImageCount(), compressed.GetMetadata(), DirectX::DDS_FLAGS_NONE, blob);
            if (FAILED(hr))
            {
                Core::Logger::Warning("TextureImage", "テクスチャキャッシュのDDSエンコードに失敗しました: " + WideToUtf8(filePath));
                return;
            }

            const std::wstring tempPath = GetCacheTempPath(filePath, sRGB);
            std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
                Core::Logger::Warning("TextureImage", "テクスチャキャッシュの書き込みに失敗しました: " + WideToUtf8(tempPath));
                return;
            }

            TexCacheHeader header{};
            std::memcpy(header.Magic, kTexCacheMagic, sizeof(kTexCacheMagic));
            header.Version = kTexCacheVersion;
            header.SourceFileTime = sourceTime;
            header.SourceFileSize = sourceSize;
            header.PayloadSize = blob.GetBufferSize();

            out.write(reinterpret_cast<const char*>(&header), sizeof(header));
            out.write(reinterpret_cast<const char*>(blob.GetBufferPointer()), static_cast<std::streamsize>(blob.GetBufferSize()));
            if (!out)
            {
                Core::Logger::Warning("TextureImage", "テクスチャキャッシュの書き込みに失敗しました: " + WideToUtf8(tempPath));
                return;
            }
            out.close();

            MoveFileExW(tempPath.c_str(), GetCachePath(filePath, sRGB).c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        // BC7圧縮専用の、エンジン本体のレンダリングデバイス(DX11/DX12どちらでもよい)とは独立した
        // 小さなD3D11デバイス。GPUのコンピュートシェーダーでBC7を圧縮するDirectXTexの
        // GPU版Compress()はID3D11Deviceしか受け付けないため、DX12バックエンド利用時でも
        // このデバイスだけは常にD3D11で用意する。将来テクスチャキャッシュ生成を独立した
        // ビルドツールへ切り出す際も、この関数はIRHIDeviceに一切依存していないためそのまま移植できる
        ID3D11Device* GetCompressionDevice()
        {
            static Microsoft::WRL::ComPtr<ID3D11Device> device = []() -> Microsoft::WRL::ComPtr<ID3D11Device>
            {
                UINT createDeviceFlags = 0;
#if defined(_DEBUG)
                createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
                // BCDirectCompute(DirectXTex内部のGPU圧縮実装)はフィーチャーレベル10.0以上で
                // 動作するが、10.xでは互換用のシェーダーモデル4版シェーダーに切り替わるため
                // 11系を優先しつつ10.xまでは許容する
                const D3D_FEATURE_LEVEL featureLevels[] =
                {
                    D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                    D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                };

                Microsoft::WRL::ComPtr<ID3D11Device> result;
                const HRESULT hr = D3D11CreateDevice(
                    nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
                    featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
                    &result, nullptr, nullptr);
                if (FAILED(hr))
                {
                    // GPU圧縮用デバイスが作れない場合はnullptrを返し、呼び出し側(CompressBC7)で
                    // 圧縮失敗として扱う(呼び出し元のLoadFromFileが非圧縮フォールバックする)
                    Core::Logger::Warning("TextureImage", "BC7圧縮用GPUデバイスの作成に失敗しました");
                    return nullptr;
                }
                return result;
            }();
            return device.Get();
        }

        // BCDirectCompute(DirectXTexのGPU圧縮)はデバイスのイミディエイトコンテキストを直接使うため、
        // 複数スレッドから同時に呼ぶとコンテキストが競合する。GetCompressionDevice()は
        // ワーカースレッド以外(RHI::DX11Device/DX12Device::CreateTextureFromFile等)からも
        // 呼ばれ得るため、共有デバイスの利用全体をミューテックスで直列化する
        std::mutex& CompressionDeviceMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        // GPU(コンピュートシェーダー)でBC7圧縮する。CPU版フォールバックは持たない
        // (ソフトウェアBC7圧縮は実用的な速度が出ないため。詳細はGetCompressionDeviceのコメント参照)。
        // GPU圧縮用デバイスが無い/圧縮呼び出し自体が失敗した場合は失敗のHRESULTを返し、
        // 呼び出し元のLoadFromFileが非圧縮のまま使うフォールバックへ回る
        HRESULT CompressBC7(
            const DirectX::Image* srcImages, size_t nimages, const DirectX::TexMetadata& metadata,
            DXGI_FORMAT format, DirectX::ScratchImage& compressed)
        {
            ID3D11Device* gpuDevice = GetCompressionDevice();
            if (!gpuDevice)
            {
                return E_FAIL;
            }

            std::lock_guard<std::mutex> lock(CompressionDeviceMutex());
            return DirectX::Compress(
                gpuDevice, srcImages, nimages, metadata, format,
                DirectX::TEX_COMPRESS_DEFAULT, DirectX::TEX_ALPHA_WEIGHT_DEFAULT, compressed);
        }
    }

    struct TextureImage::Impl
    {
        DirectX::TexMetadata Metadata{};
        DirectX::ScratchImage Image;
    };

    TextureImage::TextureImage()
        : m_Impl(std::make_unique<Impl>())
    {
    }

    TextureImage::TextureImage(TextureImage&& other) noexcept = default;
    TextureImage& TextureImage::operator=(TextureImage&& other) noexcept = default;
    TextureImage::~TextureImage() = default;

    const DirectX::TexMetadata& TextureImage::GetMetadata() const
    {
        return m_Impl->Metadata;
    }

    const DirectX::ScratchImage& TextureImage::GetImage() const
    {
        return m_Impl->Image;
    }

    uint64_t TextureImage::GetSizeInBytes() const
    {
        return static_cast<uint64_t>(m_Impl->Image.GetPixelsSize());
    }

    TextureImage TextureImage::LoadFromFile(const std::wstring& filePath, bool sRGB)
    {
        TextureImage result;

        // DDS/TGAは既に圧縮・ミップ済みであることを前提とした配布形式のため、キャッシュは作らず
        // そのまま読み込む(PNG/JPG等の無圧縮形式のみがBC7+ミップキャッシュの対象)
        if (HasExtension(filePath, L".dds"))
        {
            ThrowIfFailed(
                DirectX::LoadFromDDSFile(filePath.c_str(), DirectX::DDS_FLAGS_NONE, &result.m_Impl->Metadata, result.m_Impl->Image),
                "DDSテクスチャの読み込みに失敗しました: " + WideToUtf8(filePath));
            if (sRGB)
            {
                result.m_Impl->Image.OverrideFormat(DirectX::MakeSRGB(result.m_Impl->Metadata.format));
                result.m_Impl->Metadata = result.m_Impl->Image.GetMetadata();
            }
            return result;
        }
        if (HasExtension(filePath, L".tga"))
        {
            ThrowIfFailed(
                DirectX::LoadFromTGAFile(filePath.c_str(), DirectX::TGA_FLAGS_NONE, &result.m_Impl->Metadata, result.m_Impl->Image),
                "TGAテクスチャの読み込みに失敗しました: " + WideToUtf8(filePath));
            if (sRGB)
            {
                result.m_Impl->Image.OverrideFormat(DirectX::MakeSRGB(result.m_Impl->Metadata.format));
                result.m_Impl->Metadata = result.m_Impl->Image.GetMetadata();
            }
            return result;
        }

        uint64_t sourceTime = 0;
        uint64_t sourceSize = 0;
        const bool haveSourceStat = GetFileTimeAndSize(filePath, sourceTime, sourceSize);

        if (haveSourceStat && TryLoadDDSCache(GetCachePath(filePath, sRGB), sourceTime, sourceSize, result.m_Impl->Metadata, result.m_Impl->Image))
        {
            return result;
        }

        DirectX::TexMetadata rawMetadata{};
        DirectX::ScratchImage rawImage;
        ThrowIfFailed(
            DirectX::LoadFromWICFile(filePath.c_str(), DirectX::WIC_FLAGS_FORCE_RGB, &rawMetadata, rawImage),
            "テクスチャの読み込みに失敗しました: " + WideToUtf8(filePath));
        if (sRGB)
        {
            rawImage.OverrideFormat(DirectX::MakeSRGB(rawMetadata.format));
        }

        // ミップマップ生成に失敗しても致命的ではないため、失敗時はミップ無しの元画像のまま
        // 圧縮処理へ進む(サンプラーはミップ無しテクスチャも正しく扱える)
        DirectX::ScratchImage mipChain;
        const HRESULT mipHr = DirectX::GenerateMipMaps(rawImage.GetImages(), rawImage.GetImageCount(), rawImage.GetMetadata(), DirectX::TEX_FILTER_DEFAULT, 0, mipChain);
        if (FAILED(mipHr))
        {
            Core::Logger::Warning("TextureImage", "ミップマップ生成に失敗したため、ミップ無しのまま使用します: " + WideToUtf8(filePath));
        }
        DirectX::ScratchImage& baseImage = SUCCEEDED(mipHr) ? mipChain : rawImage;

        // BC7のソフトウェア圧縮は非常に重く(実機で1枚あたり数十秒規模になることを確認済み)、
        // GPU(コンピュートシェーダー)版のCompress()を優先して使う(CompressBC7参照。
        // GPUが使えない環境ではCPU版へ自動フォールバックする)
        const DXGI_FORMAT compressedFormat = sRGB ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
        DirectX::ScratchImage compressed;
        const HRESULT compressHr = CompressBC7(
            baseImage.GetImages(), baseImage.GetImageCount(), baseImage.GetMetadata(),
            compressedFormat, compressed);
        if (FAILED(compressHr))
        {
            // 圧縮に失敗した場合は非圧縮のまま使用する(キャッシュも書かない。次回また圧縮を試みる)
            Core::Logger::Warning("TextureImage", "BC7圧縮に失敗したため非圧縮のまま使用します: " + WideToUtf8(filePath));
            result.m_Impl->Metadata = baseImage.GetMetadata();
            result.m_Impl->Image = std::move(baseImage);
            return result;
        }

        if (haveSourceStat)
        {
            WriteDDSCacheBestEffort(filePath, sRGB, sourceTime, sourceSize, compressed);
        }

        result.m_Impl->Metadata = compressed.GetMetadata();
        result.m_Impl->Image = std::move(compressed);
        return result;
    }
}
