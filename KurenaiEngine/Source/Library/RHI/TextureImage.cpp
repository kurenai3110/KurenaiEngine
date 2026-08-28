#include "TextureImage.h"

#include <Windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <DirectXTex.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "Assets/ModelPackage.h"
#include "Core/Logger.h"
#include "Core/StringUtil.h"

namespace Kurenai::RHI
{
    namespace
    {
        using Core::WideToUtf8;

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

        // DDSヘッダだけ読めれば足りる(DXT10拡張ヘッダを含めても148バイト)。
        // KurenaiPackerのExistingKtexIsUnsupportedと同じ値
        constexpr size_t kDdsHeaderProbeBytes = 256;

        // DDSファイルのヘッダ長。'DDS 'マジック(4) + DDS_HEADER(124) と、
        // DXT10拡張がある場合の DDS_HEADER_DXT10(20)。BC7はDXGIフォーマットなので後者になる。
        // ペイロード長から全ミップのバイト数を引いた差がこのどちらかに一致することで、
        // 「このファイルは素直な2DのDDSである」ことを検算する
        constexpr uint64_t kDdsHeaderSize = 4 + 124;
        constexpr uint64_t kDdsHeaderSizeWithDXT10 = kDdsHeaderSize + 20;

        // ミップmの寸法。DirectXTexと同じく1で下げ止まる
        size_t MipExtent(size_t base, uint32_t mip)
        {
            const size_t value = base >> mip;
            return value != 0 ? value : 1;
        }

        // ミップmの1スライス分のバイト数。ブロック圧縮の端数処理を自前で書かず
        // DirectXTexへ任せる(BC7以外の.dds直読みでも同じ経路が通るようにするため)
        uint64_t MipSliceBytes(DXGI_FORMAT format, size_t baseWidth, size_t baseHeight, uint32_t mip)
        {
            size_t rowPitch = 0;
            size_t slicePitch = 0;
            const HRESULT hr = DirectX::ComputePitch(
                format, MipExtent(baseWidth, mip), MipExtent(baseHeight, mip),
                rowPitch, slicePitch, DirectX::CP_FLAGS_NONE);
            if (FAILED(hr))
            {
                return 0;
            }
            return static_cast<uint64_t>(slicePitch);
        }

        // .ktexの24Bヘッダを読んで検証する。戻ったときストリームはDDSペイロードの先頭を指す。
        // 不正な場合はstd::runtime_errorを投げる
        Assets::PackedTextureHeader ReadPackedTextureHeader(std::istream& in, const std::wstring& filePath)
        {
            Assets::PackedTextureHeader header{};
            in.read(reinterpret_cast<char*>(&header), sizeof(header));
            if (std::memcmp(header.Magic, Assets::kPackedTextureMagic, sizeof(Assets::kPackedTextureMagic)) != 0)
            {
                throw std::runtime_error("パック済みテクスチャのマジックナンバーが不正です: " + WideToUtf8(filePath));
            }
            if (header.Version != Assets::kPackedTextureVersion)
            {
                throw std::runtime_error(
                    "パック済みテクスチャのバージョンが対応していません(ファイル: " +
                    std::to_string(header.Version) + ", ランタイム: " + std::to_string(Assets::kPackedTextureVersion) +
                    "): " + WideToUtf8(filePath));
            }
            if (header.PayloadSize == 0)
            {
                throw std::runtime_error("パック済みテクスチャのペイロードが空です: " + WideToUtf8(filePath));
            }
            return header;
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

        // BC7圧縮専用の、エンジン本体のレンダリングデバイス(DX11/DX12どちらでもよい)とは独立した
        // 小さなD3D11デバイス。GPUのコンピュートシェーダーでBC7を圧縮するDirectXTexの
        // GPU版Compress()はID3D11Deviceしか受け付けないため、DX12バックエンド利用時でも
        // このデバイスだけは常にD3D11で用意する。将来テクスチャキャッシュ生成を独立した
        // ビルドツールへ切り出す際も、この関数はIRHIDeviceに一切依存していないためそのまま移植できる
        //
        // === 計測 ==============================================================
        //
        // フェーズ別の累計をナノ秒の整数で持つ(doubleのfetch_addはC++17に無い)。
        // 複数スレッドから積むためアトミック。計測しているのはKurenaiPackerが通る
        // LoadFromFile経路だけで、ランタイムが使うLoadFromPackedTextureには入れていない
        using StatsClock = std::chrono::steady_clock;

        std::atomic<uint64_t>& StatNanos(int index)
        {
            // 0=Decode 1=Mip 2=BC7Wait 3=BC7Compress 4=DeviceCreate 5=Count
            static std::atomic<uint64_t> counters[6] = {};
            return counters[index];
        }

        void AddStatNanos(int index, const StatsClock::time_point& start)
        {
            StatNanos(index).fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    StatsClock::now() - start).count()),
                std::memory_order_relaxed);
        }

        ID3D11Device* GetCompressionDevice()
        {
            static Microsoft::WRL::ComPtr<ID3D11Device> device = []() -> Microsoft::WRL::ComPtr<ID3D11Device>
            {
                const auto deviceCreateStart = StatsClock::now();
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
                    AddStatNanos(4, deviceCreateStart);
                    return nullptr;
                }
                AddStatNanos(4, deviceCreateStart);
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

        // 【この直列化を外しても速くならない(実測)】DirectX::Compressは呼ばれるたびに
        // GPUCompressBCを作り直してコンピュートシェーダを7本生成するが、その固定費は
        // 0.96msでBC7圧縮全体の1%しかない(Sponza 69枚で66ms / 6559ms)。
        // またGPU側が既に飽和しており、パッカーを2プロセス同時に走らせると
        // テクスチャフェーズは1本6.7秒から2本とも14.4秒へ伸び、合計の壁時計は変わらない。
        // ワーカーを増やす・デバイスを分ける・圧縮器を使い回す、のいずれも効かない。
        // 速くしたいなら「圧縮しない」(既存の.ktexを使う)しかない。
        //
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

            // 【待ちと圧縮を別々に測る】lock_guardのままだと両者が混ざり、
            // 「ワーカーを増やす意味があるか」を判定できない
            const auto waitStart = StatsClock::now();
            std::unique_lock<std::mutex> lock(CompressionDeviceMutex());
            AddStatNanos(2, waitStart);

            const auto compressStart = StatsClock::now();
            const HRESULT hr = DirectX::Compress(
                gpuDevice, srcImages, nimages, metadata, format,
                DirectX::TEX_COMPRESS_DEFAULT, DirectX::TEX_ALPHA_WEIGHT_DEFAULT, compressed);
            AddStatNanos(3, compressStart);
            return hr;
        }
    }

    TextureLoadStats GetTextureLoadStats()
    {
        TextureLoadStats stats;
        stats.DecodeSeconds = static_cast<double>(StatNanos(0).load(std::memory_order_relaxed)) / 1e9;
        stats.MipSeconds = static_cast<double>(StatNanos(1).load(std::memory_order_relaxed)) / 1e9;
        stats.BC7WaitSeconds = static_cast<double>(StatNanos(2).load(std::memory_order_relaxed)) / 1e9;
        stats.BC7CompressSeconds = static_cast<double>(StatNanos(3).load(std::memory_order_relaxed)) / 1e9;
        stats.DeviceCreateSeconds = static_cast<double>(StatNanos(4).load(std::memory_order_relaxed)) / 1e9;
        stats.Count = StatNanos(5).load(std::memory_order_relaxed);
        return stats;
    }

    void ResetTextureLoadStats()
    {
        for (int i = 0; i < 6; ++i)
        {
            StatNanos(i).store(0, std::memory_order_relaxed);
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

        // DDS/TGAは既に圧縮・ミップ済みであることを前提とした配布形式のため、
        // BC7圧縮・ミップ生成は行わずそのまま読み込む
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

        StatNanos(5).fetch_add(1, std::memory_order_relaxed);

        const auto decodeStart = StatsClock::now();
        DirectX::TexMetadata rawMetadata{};
        DirectX::ScratchImage rawImage;
        ThrowIfFailed(
            DirectX::LoadFromWICFile(filePath.c_str(), DirectX::WIC_FLAGS_FORCE_RGB, &rawMetadata, rawImage),
            "テクスチャの読み込みに失敗しました: " + WideToUtf8(filePath));
        AddStatNanos(0, decodeStart);
        if (sRGB)
        {
            rawImage.OverrideFormat(DirectX::MakeSRGB(rawMetadata.format));
        }

        // ミップマップ生成に失敗しても致命的ではないため、失敗時はミップ無しの元画像のまま
        // 圧縮処理へ進む(サンプラーはミップ無しテクスチャも正しく扱える)
        const auto mipStart = StatsClock::now();
        DirectX::ScratchImage mipChain;
        const HRESULT mipHr = DirectX::GenerateMipMaps(rawImage.GetImages(), rawImage.GetImageCount(), rawImage.GetMetadata(), DirectX::TEX_FILTER_DEFAULT, 0, mipChain);
        AddStatNanos(1, mipStart);
        if (FAILED(mipHr))
        {
            Core::Logger::Warning("TextureImage", "ミップマップ生成に失敗したため、ミップ無しのまま使用します: " + WideToUtf8(filePath));
        }
        DirectX::ScratchImage& baseImage = SUCCEEDED(mipHr) ? mipChain : rawImage;

        // BC7のソフトウェア圧縮は非常に重く(実機で1枚あたり数十秒規模になることを確認済み)なため、
        // GPU(コンピュートシェーダー)版のCompress()のみを使う。CPU版フォールバックは持たない
        // (CompressBC7参照)。GPU圧縮用デバイスが作れない/圧縮自体が失敗した場合は、
        // 下のFAILED(compressHr)分岐で非圧縮のまま使用する
        const DXGI_FORMAT compressedFormat = sRGB ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
        DirectX::ScratchImage compressed;
        const HRESULT compressHr = CompressBC7(
            baseImage.GetImages(), baseImage.GetImageCount(), baseImage.GetMetadata(),
            compressedFormat, compressed);
        if (FAILED(compressHr))
        {
            // 圧縮に失敗した場合は非圧縮のまま使用する
            Core::Logger::Warning("TextureImage", "BC7圧縮に失敗したため非圧縮のまま使用します: " + WideToUtf8(filePath));
            result.m_Impl->Metadata = baseImage.GetMetadata();
            result.m_Impl->Image = std::move(baseImage);
            return result;
        }

        result.m_Impl->Metadata = compressed.GetMetadata();
        result.m_Impl->Image = std::move(compressed);
        return result;
    }

    TextureImage TextureImage::LoadFromPackedTexture(const std::wstring& filePath)
    {
        TextureImage result;

        // 既定のstreambufバッファのままだと大きなテクスチャ(BC7圧縮後でも数MB~数十MB)を
        // 細切れのreadで読むことになるため、ModelLoaderの.kmodel読み込みと同様に
        // openより前に大きめ(1MB)のバッファを設定しておく
        std::vector<char> ioBuffer(1 << 20);
        std::ifstream in;
        in.rdbuf()->pubsetbuf(ioBuffer.data(), static_cast<std::streamsize>(ioBuffer.size()));
        in.open(filePath, std::ios::binary);
        if (!in.is_open())
        {
            throw std::runtime_error("パック済みテクスチャを開けませんでした: " + WideToUtf8(filePath));
        }

        try
        {
            in.exceptions(std::ios::failbit | std::ios::badbit);

            const Assets::PackedTextureHeader header = ReadPackedTextureHeader(in, filePath);

            std::vector<uint8_t> payload(header.PayloadSize);
            in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));

            // ヘッダ検証後はDDSペイロードをデコードするだけなので、WICデコード・ミップ生成・
            // BC7圧縮はいずれも発生しない
            ThrowIfFailed(
                DirectX::LoadFromDDSMemory(payload.data(), payload.size(), DirectX::DDS_FLAGS_NONE, &result.m_Impl->Metadata, result.m_Impl->Image),
                "パック済みテクスチャのDDSデコードに失敗しました: " + WideToUtf8(filePath));

            return result;
        }
        catch (const std::ios_base::failure&)
        {
            throw std::runtime_error("パック済みテクスチャの読み込み中に入出力エラーが発生しました: " + WideToUtf8(filePath));
        }
    }

    bool TextureImage::TryReadPackedTextureInfo(const std::wstring& filePath, PackedTextureInfo& outInfo)
    {
        outInfo = PackedTextureInfo{};

        std::ifstream in(filePath, std::ios::binary);
        if (!in.is_open())
        {
            Core::Logger::Error("TextureImage", "パック済みテクスチャを開けませんでした: " + WideToUtf8(filePath));
            return false;
        }

        Assets::PackedTextureHeader header{};
        try
        {
            in.exceptions(std::ios::failbit | std::ios::badbit);
            header = ReadPackedTextureHeader(in, filePath);
        }
        catch (const std::exception& e)
        {
            Core::Logger::Error("TextureImage", std::string("パック済みテクスチャのヘッダを読めませんでした: ") + e.what());
            return false;
        }

        // ここから先は「読めなければfalseを返す」だけなので例外を投げさせない
        in.exceptions(std::ios::goodbit);

        const size_t probeSize = static_cast<size_t>(std::min<uint64_t>(header.PayloadSize, kDdsHeaderProbeBytes));
        std::vector<uint8_t> probe(probeSize);
        in.read(reinterpret_cast<char*>(probe.data()), static_cast<std::streamsize>(probeSize));
        if (in.gcount() != static_cast<std::streamsize>(probeSize))
        {
            Core::Logger::Error("TextureImage", "パック済みテクスチャのDDSヘッダを読み切れませんでした: " + WideToUtf8(filePath));
            return false;
        }

        DirectX::TexMetadata metadata{};
        const HRESULT hr = DirectX::GetMetadataFromDDSMemory(probe.data(), probe.size(), DirectX::DDS_FLAGS_NONE, metadata);
        if (FAILED(hr))
        {
            Core::Logger::Error("TextureImage", "パック済みテクスチャのDDSメタデータを取得できませんでした: " + WideToUtf8(filePath));
            return false;
        }

        outInfo.Width = static_cast<uint32_t>(metadata.width);
        outInfo.Height = static_cast<uint32_t>(metadata.height);
        outInfo.MipLevels = static_cast<uint32_t>(metadata.mipLevels);
        outInfo.Format = static_cast<uint32_t>(metadata.format);
        outInfo.PayloadSize = header.PayloadSize;
        outInfo.SRGB = (header.Flags & Assets::kPackedTextureFlagSRGB) != 0;
        outInfo.SupportsPartialMipLoad =
            metadata.dimension == DirectX::TEX_DIMENSION_TEXTURE2D &&
            metadata.arraySize == 1 &&
            metadata.depth == 1 &&
            (metadata.miscFlags & DirectX::TEX_MISC_TEXTURECUBE) == 0 &&
            metadata.mipLevels > 1;
        return true;
    }

    uint64_t TextureImage::ComputeMipChainBytes(const PackedTextureInfo& info, uint32_t firstMip)
    {
        if (info.MipLevels == 0 || firstMip >= info.MipLevels)
        {
            return 0;
        }

        const auto format = static_cast<DXGI_FORMAT>(info.Format);
        uint64_t total = 0;
        for (uint32_t mip = firstMip; mip < info.MipLevels; ++mip)
        {
            total += MipSliceBytes(format, info.Width, info.Height, mip);
        }
        return total;
    }

    TextureImage TextureImage::LoadFromPackedTexture(const std::wstring& filePath, uint32_t firstMip)
    {
        if (firstMip == 0)
        {
            // 既存経路をそのまま通す(挙動を1ビットも変えない)
            return LoadFromPackedTexture(filePath);
        }

        // 部分読み出しの前提を満たすかはヘッダを見ないと分からない。
        // 満たさない場合は全ミップを読む版へ委譲する ―― 常駐量が減らないだけで絵は正しく出る
        PackedTextureInfo info{};
        if (!TryReadPackedTextureInfo(filePath, info) || !info.SupportsPartialMipLoad)
        {
            Core::Logger::Warning(
                "TextureImage",
                "ミップ単位の部分読み出しに対応しない形式のため全ミップを読み込みます: " + WideToUtf8(filePath));
            return LoadFromPackedTexture(filePath);
        }

        const uint32_t clampedFirstMip = std::min(firstMip, info.MipLevels - 1);
        const uint32_t destMipCount = info.MipLevels - clampedFirstMip;
        const auto format = static_cast<DXGI_FORMAT>(info.Format);

        // 読み飛ばすバイト数と読むバイト数。DDSはミップ0を先頭に降順で連続している
        uint64_t skipBytes = 0;
        uint64_t keepBytes = 0;
        for (uint32_t mip = 0; mip < info.MipLevels; ++mip)
        {
            const uint64_t bytes = MipSliceBytes(format, info.Width, info.Height, mip);
            if (bytes == 0)
            {
                Core::Logger::Warning(
                    "TextureImage",
                    "ミップのバイト数を計算できなかったため全ミップを読み込みます: " + WideToUtf8(filePath));
                return LoadFromPackedTexture(filePath);
            }
            if (mip < clampedFirstMip)
            {
                skipBytes += bytes;
            }
            else
            {
                keepBytes += bytes;
            }
        }

        // 【検算】ペイロード長から全ミップのバイト数を引いた差がDDSヘッダ長に一致すること。
        // 一致しないなら、こちらが想定していない並び(パディング等)のファイルなので触らない
        const uint64_t pixelBytes = skipBytes + keepBytes;
        if (info.PayloadSize < pixelBytes)
        {
            Core::Logger::Warning(
                "TextureImage",
                "ペイロードがミップの合計より小さいため全ミップを読み込みます: " + WideToUtf8(filePath));
            return LoadFromPackedTexture(filePath);
        }
        const uint64_t ddsHeaderBytes = info.PayloadSize - pixelBytes;
        if (ddsHeaderBytes != kDdsHeaderSize && ddsHeaderBytes != kDdsHeaderSizeWithDXT10)
        {
            Core::Logger::Warning(
                "TextureImage",
                "DDSヘッダ長が想定(" + std::to_string(kDdsHeaderSize) + " または " +
                    std::to_string(kDdsHeaderSizeWithDXT10) + ")と異なる(" + std::to_string(ddsHeaderBytes) +
                    ")ため全ミップを読み込みます: " + WideToUtf8(filePath));
            return LoadFromPackedTexture(filePath);
        }

        std::vector<char> ioBuffer(1 << 20);
        std::ifstream in;
        in.rdbuf()->pubsetbuf(ioBuffer.data(), static_cast<std::streamsize>(ioBuffer.size()));
        in.open(filePath, std::ios::binary);
        if (!in.is_open())
        {
            throw std::runtime_error("パック済みテクスチャを開けませんでした: " + WideToUtf8(filePath));
        }

        try
        {
            in.exceptions(std::ios::failbit | std::ios::badbit);

            const uint64_t payloadOffset = sizeof(Assets::PackedTextureHeader) + ddsHeaderBytes + skipBytes;
            in.seekg(static_cast<std::streamoff>(payloadOffset), std::ios::beg);

            std::vector<uint8_t> payload(static_cast<size_t>(keepBytes));
            in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));

            // 読んだバイト列を「firstMipを新しいミップ0とするテクスチャ」として組み立てる。
            // DDSヘッダを作り直すのではなくScratchImageへ直接置くのは、DDS_HEADER/DXT10の
            // フラグを自前で組み立てる誤りを持ち込まないため
            TextureImage result;
            ThrowIfFailed(
                result.m_Impl->Image.Initialize2D(
                    format,
                    MipExtent(info.Width, clampedFirstMip),
                    MipExtent(info.Height, clampedFirstMip),
                    1, destMipCount),
                "ミップを縮めたテクスチャの確保に失敗しました: " + WideToUtf8(filePath));

            uint64_t offset = 0;
            for (uint32_t destMip = 0; destMip < destMipCount; ++destMip)
            {
                const DirectX::Image* destImage = result.m_Impl->Image.GetImage(destMip, 0, 0);
                if (destImage == nullptr || destImage->pixels == nullptr)
                {
                    throw std::runtime_error("ミップの格納先を取得できませんでした: " + WideToUtf8(filePath));
                }

                // ScratchImageの各ミップのバイト数が、DDS上の同じミップのバイト数と一致すること。
                // 一致しなければ並びの前提が崩れているので、黙って壊れた絵を出さずに止める
                const uint64_t sourceBytes = MipSliceBytes(format, info.Width, info.Height, clampedFirstMip + destMip);
                if (destImage->slicePitch != static_cast<size_t>(sourceBytes) ||
                    offset + sourceBytes > payload.size())
                {
                    throw std::runtime_error(
                        "ミップのバイト数がDDS上の並びと一致しません(ミップ" + std::to_string(clampedFirstMip + destMip) +
                        "): " + WideToUtf8(filePath));
                }

                std::memcpy(destImage->pixels, payload.data() + offset, static_cast<size_t>(sourceBytes));
                offset += sourceBytes;
            }

            result.m_Impl->Metadata = result.m_Impl->Image.GetMetadata();
            return result;
        }
        catch (const std::ios_base::failure&)
        {
            throw std::runtime_error("パック済みテクスチャの読み込み中に入出力エラーが発生しました: " + WideToUtf8(filePath));
        }
    }
}
