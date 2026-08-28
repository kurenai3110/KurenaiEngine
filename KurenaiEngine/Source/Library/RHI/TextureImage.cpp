#include "TextureImage.h"

#include <Windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <DirectXTex.h>

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
}
