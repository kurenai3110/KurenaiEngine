#include "DX12Device.h"
#include "DX12DeviceInternal.h"

#include <d3dx12.h>
#include <DirectXTex.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "DX12Texture.h"
#include "DX12Util.h"
#include "Core/StringUtil.h"
#include "RHI/DXGIFormatUtil.h"
#include "RHI/TextureImage.h"

// テクスチャの生成とアップロード。
// DX12Device のメンバ関数のまま、翻訳単位だけをここへ分けている
// (宣言は DX12Device.h のまま。IRHIDevice のインターフェースは1行も変えていない)
namespace Kurenai::RHI
{
    using namespace DX12Internal;

    D3D12_SHADER_RESOURCE_VIEW_DESC DX12Device::MakeSrvDesc(const DirectX::TexMetadata& metadata)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = metadata.format;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (metadata.IsCubemap())
        {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.TextureCube.MipLevels = static_cast<UINT>(metadata.mipLevels);
        }
        else
        {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = static_cast<UINT>(metadata.mipLevels);
        }
        return srvDesc;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> DX12Device::CreateAndUploadTextureResource(
        const DirectX::TexMetadata& metadata, const DirectX::ScratchImage& image)
    {
        // 初期データのアップロードはm_CommandList(Renderスレッドが毎フレーム使うコマンドリスト)ではなく
        // m_UploadCommandList専用のコマンドリストで行う(詳細はm_UploadCommandListのコメント参照)。
        // この関数はLoadScene等どのスレッドからも呼ばれ得るため、m_UploadCommandListへの記録から
        // UploadSubmitAndWait()完了までをミューテックスで直列化する
        std::lock_guard<std::mutex> uploadLock(m_UploadMutex);

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(
            DirectX::CreateTextureEx(m_Device.Get(), metadata, D3D12_RESOURCE_FLAG_NONE, DirectX::CREATETEX_DEFAULT, &resource),
            "テクスチャの作成に失敗しました");

        std::vector<D3D12_SUBRESOURCE_DATA> subresources;
        ThrowIfFailed(
            DirectX::PrepareUpload(m_Device.Get(), image.GetImages(), image.GetImageCount(), metadata, subresources),
            "アップロードデータの準備に失敗しました");

        const UINT subresourceCount = static_cast<UINT>(subresources.size());
        const D3D12_RESOURCE_DESC destDesc = resource->GetDesc();
        UINT64 requiredSize = 0;
        m_Device->GetCopyableFootprints(&destDesc, 0, subresourceCount, 0, nullptr, nullptr, nullptr, &requiredSize);

        // DirectX::CreateTextureEx はデスクトップ環境ではリソースをD3D12_RESOURCE_STATE_COMMONで作成するため、
        // コピー先として使う前にCOPY_DESTへ明示的に遷移させる必要がある
        const D3D12_RESOURCE_BARRIER toCopyDestBarrier =
            CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        m_UploadCommandList->ResourceBarrier(1, &toCopyDestBarrier);

        Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer = CreateUploadBuffer(requiredSize);
        UpdateSubresources(m_UploadCommandList.Get(), resource.Get(), uploadBuffer.Get(), 0, 0, subresourceCount, subresources.data());

        const D3D12_RESOURCE_BARRIER toSrvBarrier =
            CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_UploadCommandList->ResourceBarrier(1, &toSrvBarrier);

        // アップロードバッファはこの関数を抜けるまで生存させる必要があるため、ここで同期的に実行完了を待つ
        UploadSubmitAndWait();

        return resource;
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateTextureResourceFromImage(const DirectX::TexMetadata& metadata, const DirectX::ScratchImage& image)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource = CreateAndUploadTextureResource(metadata, image);

        // ファイル/デコード済み画像から作るテクスチャ(マテリアル・スカイボックス・プレースホルダ)は
        // すべてアセット由来。シーン読み込み専用スレッドが確保・解放するためアセット側のヒープを使う
        const uint32_t srvIndex = m_AssetSrvCpuHeap->Allocate();
        const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = MakeSrvDesc(metadata);
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_AssetSrvCpuHeap->GetCpuHandle(srvIndex));

        return std::make_unique<DX12Texture>(
            this, m_AssetSrvCpuHeap.get(), resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, srvIndex, DX12Texture::kInvalid, DX12Texture::kInvalid);
    }

    std::unique_ptr<IRHIPendingTextureContents> DX12Device::PrepareTextureContents(
        IRHITexture* target, const TextureImage& image)
    {
        auto* texture = static_cast<DX12Texture*>(target);
        if (texture == nullptr)
        {
            Core::Logger::Error("DX12", "PrepareTextureContents: テクスチャがnullptrです");
            return nullptr;
        }

        // 差し替えてよいのは、アセット用ヒープから確保したSRVだけを持つテクスチャに限る。
        // レンダーターゲットやUAVを持つものは他のビューとの整合が取れなくなる
        if (!texture->HasSrv() || texture->HasRtv() || texture->HasDsv() || texture->HasUav())
        {
            Core::Logger::Error("DX12", "PrepareTextureContents: SRV以外のビューを持つテクスチャは差し替えられません");
            return nullptr;
        }
        if (texture->GetSrvUavHeap() != m_AssetSrvCpuHeap.get())
        {
            Core::Logger::Error("DX12", "PrepareTextureContents: アセット用ヒープ以外から確保されたテクスチャは差し替えられません");
            return nullptr;
        }

        const DirectX::TexMetadata& metadata = image.GetMetadata();

        Microsoft::WRL::ComPtr<ID3D12Resource> newResource;
        try
        {
            newResource = CreateAndUploadTextureResource(metadata, image.GetImage());
        }
        catch (const std::exception& e)
        {
            // 失敗しても元の中身はそのまま。常駐ミップが減らないだけで絵は出続ける
            Core::Logger::Error("DX12", std::string("PrepareTextureContents: リソースの作成に失敗しました: ") + e.what());
            return nullptr;
        }

        return std::make_unique<DX12PendingTextureContents>(
            texture, std::move(newResource), MakeSrvDesc(metadata), static_cast<uint32_t>(metadata.mipLevels));
    }

    bool DX12Device::CommitTextureContents(IRHIPendingTextureContents* pending)
    {
        auto* entry = static_cast<DX12PendingTextureContents*>(pending);
        if (entry == nullptr || entry->Texture == nullptr)
        {
            Core::Logger::Error("DX12", "CommitTextureContents: 差し替え待ちの内容が不正です");
            return false;
        }

        if (entry->IsTiledResidency)
        {
            // タイルリソース経路。リソースもSRV番号もbindless番号も変わらず、
            // 変えるのはディスクリプタのResourceMinLODClampだけ
            if (entry->NewTiledState)
            {
                // 初めてタイルリソース化する回だけ、リソースそのものも入れ替わる
                entry->Texture->SetTiledState(std::move(entry->NewTiledState));
                RetireResource(entry->Texture->SwapResource(
                    std::move(entry->Resource), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
            }

            DX12TiledTextureState* state = entry->Texture->GetTiledState();
            if (state == nullptr)
            {
                Core::Logger::Error("DX12", "CommitTextureContents: タイル情報が失われています");
                return false;
            }
            state->ResidentFirstMip = entry->TiledFirstMip;

            const uint32_t srvIndex = entry->Texture->GetSrvIndex();
            m_Device->CreateShaderResourceView(
                entry->Texture->GetResource(), &entry->SrvDesc, m_AssetSrvCpuHeap->GetCpuHandle(srvIndex));

            const uint32_t bindlessIndex = entry->Texture->GetBindlessIndex();
            if (bindlessIndex != kInvalidBindlessIndex && m_BindlessTable)
            {
                m_BindlessTable->Rebind(bindlessIndex, m_AssetSrvCpuHeap->GetCpuHandle(srvIndex));
            }

            // 外す側は、直前まで記録されたコマンドリストがそのミップを読み終わるまで待つ
            if (entry->UnmapMipCount > 0 || !entry->TilesToRelease.empty())
            {
                RetireTileMapping(
                    entry->Texture->GetResource(), entry->UnmapFirstMip, entry->UnmapMipCount,
                    std::move(entry->TilesToRelease));
            }
            return true;
        }

        if (!entry->Resource)
        {
            Core::Logger::Error("DX12", "CommitTextureContents: 差し替え待ちのリソースがありません");
            return false;
        }

        // 【同じ番号のディスクリプタを作り直す】新しい番号を払い出さないので、
        // Assets::Meshが持つIRHITexture*も、bindless番号も変わらない
        const uint32_t srvIndex = entry->Texture->GetSrvIndex();
        m_Device->CreateShaderResourceView(
            entry->Resource.Get(), &entry->SrvDesc, m_AssetSrvCpuHeap->GetCpuHandle(srvIndex));

        const uint32_t bindlessIndex = entry->Texture->GetBindlessIndex();
        if (bindlessIndex != kInvalidBindlessIndex && m_BindlessTable)
        {
            m_BindlessTable->Rebind(bindlessIndex, m_AssetSrvCpuHeap->GetCpuHandle(srvIndex));
        }

        // 古いリソースはGPUがまだ読んでいる可能性がある。ここで手放さず遅延解放キューへ積む
        RetireResource(entry->Texture->SwapResource(
            std::move(entry->Resource), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        return true;
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateTextureFromFile(const std::wstring& filePath, bool sRGB)
    {
        return CreateTextureFromImage(TextureImage::LoadFromFile(filePath, sRGB));
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateTextureFromImage(const TextureImage& image)
    {
        return CreateTextureResourceFromImage(image.GetMetadata(), image.GetImage());
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateSolidColorTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        DirectX::TexMetadata metadata{};
        metadata.width = 1;
        metadata.height = 1;
        metadata.depth = 1;
        metadata.arraySize = 1;
        metadata.mipLevels = 1;
        metadata.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        metadata.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;

        DirectX::ScratchImage image;
        ThrowIfFailed(image.Initialize2D(metadata.format, 1, 1, 1, 1), "1x1テクスチャの作成に失敗しました");

        const uint8_t pixel[4] = { r, g, b, a };
        memcpy(image.GetImage(0, 0, 0)->pixels, pixel, sizeof(pixel));

        return CreateTextureResourceFromImage(metadata, image);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateTextureFromMemory(uint32_t width, uint32_t height, const void* pixelsRGBA8)
    {
        DirectX::TexMetadata metadata{};
        metadata.width = width;
        metadata.height = height;
        metadata.depth = 1;
        metadata.arraySize = 1;
        metadata.mipLevels = 1;
        metadata.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        metadata.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;

        DirectX::ScratchImage image;
        ThrowIfFailed(image.Initialize2D(metadata.format, width, height, 1, 1), "テクスチャの作成に失敗しました");

        // 入力(pixelsRGBA8)はタイトパッキング(1行=width*4バイト)だが、ScratchImageの行ピッチは
        // アライメントの都合で異なる場合があるため、行ごとにコピーする
        const DirectX::Image* image0 = image.GetImage(0, 0, 0);
        const uint8_t* src = static_cast<const uint8_t*>(pixelsRGBA8);
        for (uint32_t y = 0; y < height; ++y)
        {
            memcpy(image0->pixels + y * image0->rowPitch, src + static_cast<size_t>(y) * width * 4, static_cast<size_t>(width) * 4);
        }

        return CreateTextureResourceFromImage(metadata, image);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateRenderTexture(uint32_t width, uint32_t height, Format format)
    {
        const DXGI_FORMAT dxgiFormat = ToDXGIFormat(format);

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = dxgiFormat;
        clearValue.Color[0] = clearValue.Color[1] = clearValue.Color[2] = 0.0f;
        clearValue.Color[3] = 1.0f;

        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC resourceDesc =
            CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(
            m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue, IID_PPV_ARGS(&resource)),
            "レンダーテクスチャの作成に失敗しました");

        const uint32_t srvIndex = m_RenderSrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = dxgiFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_RenderSrvCpuHeap->GetCpuHandle(srvIndex));

        const uint32_t rtvIndex = m_RtvHeap->Allocate();
        m_Device->CreateRenderTargetView(resource.Get(), nullptr, m_RtvHeap->GetCpuHandle(rtvIndex));

        return std::make_unique<DX12Texture>(this, m_RenderSrvCpuHeap.get(), resource, D3D12_RESOURCE_STATE_RENDER_TARGET, srvIndex, rtvIndex, DX12Texture::kInvalid);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateUAVTexture(uint32_t width, uint32_t height, Format format)
    {
        const DXGI_FORMAT dxgiFormat = ToDXGIFormat(format);

        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC resourceDesc =
            CD3DX12_RESOURCE_DESC::Tex2D(dxgiFormat, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(
            m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource)),
            "UAVテクスチャの作成に失敗しました");

        const uint32_t srvIndex = m_RenderSrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = dxgiFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_RenderSrvCpuHeap->GetCpuHandle(srvIndex));

        const uint32_t uavIndex = m_RenderSrvCpuHeap->Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = dxgiFormat;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, m_RenderSrvCpuHeap->GetCpuHandle(uavIndex));

        return std::make_unique<DX12Texture>(
            this, m_RenderSrvCpuHeap.get(), resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvIndex, DX12Texture::kInvalid, DX12Texture::kInvalid, uavIndex);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateUAVTexture3D(
        uint32_t width, uint32_t height, uint32_t depth, Format format)
    {
        // CreateUAVTexture(上)の3D版。ビューの次元指定をTEXTURE3Dにすることと、
        // UAVにWSize(書き込み対象の奥行きスライス数)を明示することだけが2Dとの違い。
        // 【WSizeの指定を忘れないこと】0のままだとUAVが奥行き0枚を指すことになり、
        // ディスパッチしても何も書き込まれない(エラーにはならず、テクスチャが黒いままになる)
        const DXGI_FORMAT dxgiFormat = ToDXGIFormat(format);

        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex3D(
            dxgiFormat, width, height, static_cast<UINT16>(depth), 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(
            m_Device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&resource)),
            "3D UAVテクスチャの作成に失敗しました");

        const uint32_t srvIndex = m_RenderSrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = dxgiFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture3D.MipLevels = 1;
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_RenderSrvCpuHeap->GetCpuHandle(srvIndex));

        const uint32_t uavIndex = m_RenderSrvCpuHeap->Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = dxgiFormat;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uavDesc.Texture3D.WSize = depth;
        m_Device->CreateUnorderedAccessView(
            resource.Get(), nullptr, &uavDesc, m_RenderSrvCpuHeap->GetCpuHandle(uavIndex));

        return std::make_unique<DX12Texture>(
            this, m_RenderSrvCpuHeap.get(), resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvIndex,
            DX12Texture::kInvalid, DX12Texture::kInvalid, uavIndex);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateMippedUAVTexture(uint32_t width, uint32_t height, Format format, uint32_t mipLevels)
    {
        const DXGI_FORMAT dxgiFormat = ToDXGIFormat(format);

        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            dxgiFormat, width, height, 1, static_cast<UINT16>(mipLevels), 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(
            m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource)),
            "ミップ付きUAVテクスチャの作成に失敗しました");

        // 全ミップを見るSRV(MipLevels=全指定)。デバッグ表示などでSampleLevelにより任意のミップを読む用
        const uint32_t srvIndex = m_RenderSrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = dxgiFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = mipLevels;
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_RenderSrvCpuHeap->GetCpuHandle(srvIndex));

        // ミップごとに単一ミップのUAVを張り、コンピュートシェーダーがミップ単位で書き込めるようにする
        // (Hi-Zの「前段ミップを読んで次段へ書く」ダウンサンプルだけでなく、IBLプリフィルタ済み鏡面マップの
        // 「ミップごとに異なるラフネスで独立に畳み込む」用途でも同じ仕組みを再利用する)
        std::vector<uint32_t> mipUavIndices;
        mipUavIndices.reserve(mipLevels);
        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            const uint32_t uavIndex = m_RenderSrvCpuHeap->Allocate();
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = dxgiFormat;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uavDesc.Texture2D.MipSlice = mip;
            m_Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, m_RenderSrvCpuHeap->GetCpuHandle(uavIndex));
            mipUavIndices.push_back(uavIndex);
        }

        return std::make_unique<DX12Texture>(
            this, m_RenderSrvCpuHeap.get(), resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvIndex, DX12Texture::kInvalid, DX12Texture::kInvalid,
            DX12Texture::kInvalid, std::move(mipUavIndices));
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateUAVTextureCube(uint32_t size, Format format)
    {
        return CreateMippedUAVTextureCube(size, format, 1);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateMippedUAVTextureCube(uint32_t size, Format format, uint32_t mipLevels)
    {
        // cubeCount=1のときはSRVをTextureCubeArrayではなくTextureCubeとして張る(HLSL側の
        // TextureCube宣言と一致させるため。IBLConvolve.hlsl等)
        return CreateCubeTextureInternal(size, format, mipLevels, 1, false);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateMippedUAVTextureCubeArray(
        uint32_t size, Format format, uint32_t mipLevels, uint32_t cubeCount)
    {
        return CreateCubeTextureInternal(size, format, mipLevels, cubeCount, true);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateCubeTextureInternal(
        uint32_t size, Format format, uint32_t mipLevels, uint32_t cubeCount, bool asArray)
    {
        if (size == 0 || mipLevels == 0 || cubeCount == 0)
        {
            const std::string message =
                "キューブマップUAVテクスチャの作成に失敗しました: サイズ・ミップ数・キューブ数はいずれも1以上である必要があります (size=" +
                std::to_string(size) + ", mipLevels=" + std::to_string(mipLevels) + ", cubeCount=" + std::to_string(cubeCount) + ")";
            Core::Logger::Error("DX12", message);
            throw std::runtime_error(message);
        }

        // D3D12のTexture2D配列は最大2048スライス。キューブマップは1枚あたり6スライス消費する
        const uint32_t arraySize = cubeCount * DX12Texture::kCubeFaceCount;
        if (arraySize > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION)
        {
            const std::string message =
                "キューブマップUAVテクスチャの作成に失敗しました: 配列スライス数が上限を超えています (cubeCount=" +
                std::to_string(cubeCount) + ", 必要スライス数=" + std::to_string(arraySize) +
                ", 上限=" + std::to_string(D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) + ")";
            Core::Logger::Error("DX12", message);
            throw std::runtime_error(message);
        }

        const DXGI_FORMAT dxgiFormat = ToDXGIFormat(format);

        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            dxgiFormat, size, size, static_cast<UINT16>(arraySize), static_cast<UINT16>(mipLevels),
            1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(
            m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource)),
            "キューブマップUAVテクスチャの作成に失敗しました");

        // 全6面・全ミップを1枚のTextureCube(配列版はTextureCubeArray)として読むSRV
        // (サンプリング側、DeferredLighting.hlsl等)
        const uint32_t srvIndex = m_RenderSrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = dxgiFormat;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (asArray)
        {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            srvDesc.TextureCubeArray.MostDetailedMip = 0;
            srvDesc.TextureCubeArray.MipLevels = mipLevels;
            srvDesc.TextureCubeArray.First2DArrayFace = 0;
            srvDesc.TextureCubeArray.NumCubes = cubeCount;
        }
        else
        {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.TextureCube.MostDetailedMip = 0;
            srvDesc.TextureCube.MipLevels = mipLevels;
        }
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_RenderSrvCpuHeap->GetCpuHandle(srvIndex));

        // キューブ×面×ミップの組み合わせごとに単一配列スライス・単一ミップのUAV(Texture2DArray、要素数1)を
        // 張り、コンピュートシェーダーが面ごとに1回ずつディスパッチして書き込めるようにする(HLSL側は
        // RWTexture2DArrayとして宣言する必要がある。IBLConvolve.hlsl参照)。
        // (mip*cubeCount + cubeIndex)*kCubeFaceCount + face の順でフラットに格納する
        // (DX12Texture::GetCubeUavCpuHandle参照)
        std::vector<uint32_t> mipUavIndices;
        mipUavIndices.reserve(static_cast<size_t>(mipLevels) * arraySize);
        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            for (uint32_t cube = 0; cube < cubeCount; ++cube)
            {
                for (uint32_t face = 0; face < DX12Texture::kCubeFaceCount; ++face)
                {
                    const uint32_t uavIndex = m_RenderSrvCpuHeap->Allocate();
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
                    uavDesc.Format = dxgiFormat;
                    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                    uavDesc.Texture2DArray.MipSlice = mip;
                    uavDesc.Texture2DArray.FirstArraySlice = cube * DX12Texture::kCubeFaceCount + face;
                    uavDesc.Texture2DArray.ArraySize = 1;
                    m_Device->CreateUnorderedAccessView(resource.Get(), nullptr, &uavDesc, m_RenderSrvCpuHeap->GetCpuHandle(uavIndex));
                    mipUavIndices.push_back(uavIndex);
                }
            }
        }

        return std::make_unique<DX12Texture>(
            this, m_RenderSrvCpuHeap.get(), resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvIndex, DX12Texture::kInvalid, DX12Texture::kInvalid,
            DX12Texture::kInvalid, std::move(mipUavIndices), std::vector<uint32_t>{}, cubeCount);
    }

    std::unique_ptr<IRHITexture> DX12Device::CreateDepthTexture(uint32_t width, uint32_t height, float clearDepth)
    {
        // 深度テクスチャは後段のライティングパスでサンプリングするためSHADER_RESOURCEも付与し、
        // Typelessフォーマットで作成してDSV/SRVそれぞれに適したビューを個別に張る(DX11実装と同じ方針)。
        // ステンシルは使わないためD32_FLOATにしている(Reverse-Zの精度改善はUNORMでは効果がなく、
        // 浮動小数点フォーマットと組み合わせて初めて意味を持つ)
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = clearDepth;
        clearValue.DepthStencil.Stencil = 0;

        const CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC resourceDesc =
            CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(
            m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(&resource)),
            "深度テクスチャの作成に失敗しました");

        const uint32_t dsvIndex = m_DsvHeap->Allocate();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_Device->CreateDepthStencilView(resource.Get(), &dsvDesc, m_DsvHeap->GetCpuHandle(dsvIndex));

        const uint32_t srvIndex = m_RenderSrvCpuHeap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_Device->CreateShaderResourceView(resource.Get(), &srvDesc, m_RenderSrvCpuHeap->GetCpuHandle(srvIndex));

        return std::make_unique<DX12Texture>(this, m_RenderSrvCpuHeap.get(), resource, D3D12_RESOURCE_STATE_DEPTH_WRITE, srvIndex, DX12Texture::kInvalid, dsvIndex);
    }
}
