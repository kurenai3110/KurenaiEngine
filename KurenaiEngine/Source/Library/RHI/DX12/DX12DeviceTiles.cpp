#include "DX12Device.h"
#include "DX12DeviceInternal.h"

#include <d3dx12.h>
#include <DirectXTex.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "DX12Texture.h"
#include "DX12Util.h"
#include "Core/StringUtil.h"
#include "RHI/DXGIFormatUtil.h"

// 予約リソース(タイル)の常駐管理と、破棄を寝かせる仕組み。
// DX12Device のメンバ関数のまま、翻訳単位だけをここへ分けている
// (宣言は DX12Device.h のまま。IRHIDevice のインターフェースは1行も変えていない)
namespace Kurenai::RHI
{
    using namespace DX12Internal;

    void DX12Device::MapStandardMip(
        ID3D12Resource* resource, const DX12TiledTextureState& state, uint32_t mip,
        const std::vector<DX12TilePool::Tile>& tiles)
    {
        const D3D12_SUBRESOURCE_TILING& tiling = state.SubresourceTiling[mip];

        D3D12_TILED_RESOURCE_COORDINATE start{};
        start.X = 0;
        start.Y = 0;
        start.Z = 0;
        start.Subresource = mip;

        D3D12_TILE_REGION_SIZE region{};
        region.NumTiles = tiling.WidthInTiles * tiling.HeightInTiles * tiling.DepthInTiles;
        region.UseBox = TRUE;
        region.Width = tiling.WidthInTiles;
        region.Height = static_cast<UINT16>(tiling.HeightInTiles);
        region.Depth = static_cast<UINT16>(tiling.DepthInTiles);
        if (region.NumTiles == 0)
        {
            return;
        }

        if (tiles.empty())
        {
            // NULLマッピング(外す)。Tier 2以上は未マップの読み出しが0を返すと保証されている
            const D3D12_TILE_RANGE_FLAGS rangeFlag = D3D12_TILE_RANGE_FLAG_NULL;
            const UINT rangeTileCount = region.NumTiles;
            m_CommandQueue->UpdateTileMappings(
                resource, 1, &start, &region, nullptr, 1, &rangeFlag, nullptr, &rangeTileCount,
                D3D12_TILE_MAPPING_FLAG_NONE);
            return;
        }

        // タイルはヒープをまたいで散らばりうる。UpdateTileMappings は1回の呼び出しで
        // 1つのヒープしか指せないため、同じヒープの連続したタイルごとに区切って呼ぶ。
        // 領域側の座標は「x→y→z の順に数えたときの通し番号」で進む
        uint32_t consumed = 0;
        while (consumed < tiles.size())
        {
            const uint32_t heapIndex = tiles[consumed].HeapIndex;
            const uint32_t run = DX12TilePool::GetContiguousRunLength(tiles, consumed);
            if (run == 0)
            {
                break;
            }

            D3D12_TILED_RESOURCE_COORDINATE runStart = start;
            // 通し番号 consumed をタイル座標へ戻す
            runStart.X = consumed % tiling.WidthInTiles;
            runStart.Y = (consumed / tiling.WidthInTiles) % tiling.HeightInTiles;
            runStart.Z = consumed / (tiling.WidthInTiles * tiling.HeightInTiles);

            D3D12_TILE_REGION_SIZE runRegion{};
            runRegion.NumTiles = run;
            // UseBox=FALSEなら「その座標からNumTiles個ぶん、x→y→zの順に進む」直線的な指定になる。
            // ヒープの連続範囲ごとに切っているためこちらが素直
            runRegion.UseBox = FALSE;

            const D3D12_TILE_RANGE_FLAGS rangeFlag = D3D12_TILE_RANGE_FLAG_NONE;
            const UINT heapStartOffset = tiles[consumed].TileIndex;
            const UINT rangeTileCount = run;
            m_CommandQueue->UpdateTileMappings(
                resource, 1, &runStart, &runRegion, m_TilePool->GetHeap(heapIndex), 1, &rangeFlag,
                &heapStartOffset, &rangeTileCount, D3D12_TILE_MAPPING_FLAG_NONE);

            consumed += run;
        }
    }

    // 【第1段】予約リソースを作り、タイルの形とミップテールを実測して常駐状態を組み立てる。
    // 作れない構成・タイルにする意味が無い構成ではnullptrを返し、呼び出し元が従来経路へ委ねる
    std::unique_ptr<DX12TiledTextureState> DX12Device::CreateReservedTiledResource(
        const TiledTextureDesc& desc, Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
    {
        // --- 初めてタイルリソース化する ---
        D3D12_RESOURCE_DESC resourceDesc{};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resourceDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        resourceDesc.Width = desc.Width;
        resourceDesc.Height = desc.Height;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = static_cast<UINT16>(desc.MipLevels);
        resourceDesc.Format = static_cast<DXGI_FORMAT>(desc.DxgiFormat);
        resourceDesc.SampleDesc.Count = 1;
        // 予約リソースはこのレイアウトでしか作れない(D3D12_TEXTURE_LAYOUTの規定)
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        if (FAILED(m_Device->CreateReservedResource(
                &resourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))))
        {
            Core::Logger::Warning(
                "DX12", "予約リソースを作成できませんでした(このテクスチャは従来経路で扱います)");
            return nullptr;
        }

        auto newState = std::make_unique<DX12TiledTextureState>();
        newState->Format = resourceDesc.Format;
        newState->Width = desc.Width;
        newState->Height = desc.Height;
        newState->MipLevels = desc.MipLevels;

        // 【タイルの形もミップテールの構成も実測する】仕様上どちらもアダプタ依存で、
        // 「64KBタイル = BC7で256x256」は見積もりに過ぎない
        UINT numTiles = 0;
        UINT numSubresourceTilings = desc.MipLevels;
        newState->SubresourceTiling.resize(desc.MipLevels);
        m_Device->GetResourceTiling(
            resource.Get(), &numTiles, &newState->PackedMipInfo, &newState->TileShape,
            &numSubresourceTilings, 0, newState->SubresourceTiling.data());

        if (newState->PackedMipInfo.NumStandardMips == 0)
        {
            // 全部がミップテール = 一括でしか出し入れできない。タイルにする意味が無いので
            // 従来経路(リソースごと作り直す)へ委ねる。512x512以下ではこちらが普通
            return nullptr;
        }

        newState->MappedTiles.resize(newState->PackedMipInfo.NumStandardMips);
        resource->SetName(L"TiledStreamingTexture");

        // ミップテールは一括でしかマップ/アンマップできないため、常に貼りっぱなしにする
        if (newState->PackedMipInfo.NumTilesForPackedMips > 0)
        {
            if (!m_TilePool->Allocate(newState->PackedMipInfo.NumTilesForPackedMips, newState->PackedMipTiles))
            {
                return nullptr;
            }

            D3D12_TILED_RESOURCE_COORDINATE tailStart{};
            tailStart.Subresource = newState->PackedMipInfo.NumStandardMips;
            D3D12_TILE_REGION_SIZE tailRegion{};
            tailRegion.NumTiles = newState->PackedMipInfo.NumTilesForPackedMips;
            tailRegion.UseBox = FALSE;

            // ミップテールのタイルもヒープをまたぎうるので、連続範囲ごとに切って貼る
            uint32_t consumed = 0;
            while (consumed < newState->PackedMipTiles.size())
            {
                const uint32_t heapIndex = newState->PackedMipTiles[consumed].HeapIndex;
                const uint32_t run = DX12TilePool::GetContiguousRunLength(newState->PackedMipTiles, consumed);
                if (run == 0)
                {
                    break;
                }

                D3D12_TILED_RESOURCE_COORDINATE runStart = tailStart;
                runStart.X = consumed;
                D3D12_TILE_REGION_SIZE runRegion{};
                runRegion.NumTiles = run;
                runRegion.UseBox = FALSE;

                const D3D12_TILE_RANGE_FLAGS rangeFlag = D3D12_TILE_RANGE_FLAG_NONE;
                const UINT heapStartOffset = newState->PackedMipTiles[consumed].TileIndex;
                const UINT rangeTileCount = run;
                m_CommandQueue->UpdateTileMappings(
                    resource.Get(), 1, &runStart, &runRegion, m_TilePool->GetHeap(heapIndex), 1, &rangeFlag,
                    &heapStartOffset, &rangeTileCount, D3D12_TILE_MAPPING_FLAG_NONE);
                consumed += run;
            }
        }

        return newState;
    }

    // 【第2段】常駐するミップの範囲をfirstMipへ寄せる。細かくする側はその場でタイルを貼り、
    // 粗くする側は外す予約をpendingへ積むだけにする(いま外すとGPUが読んでいる最中に消える)
    bool DX12Device::UpdateTiledMipResidency(
        const TiledTextureDesc& desc, DX12Texture* texture, const Microsoft::WRL::ComPtr<ID3D12Resource>& resource,
        DX12TiledTextureState* state, uint32_t firstMip, uint32_t oldFirstMip, uint32_t standardMips,
        DX12PendingTextureContents* pending)
    {
        // --- 細かくする方向: [firstMip, oldFirstMip) の標準ミップを貼る ---
        for (uint32_t mip = firstMip; mip < std::min(oldFirstMip, standardMips); ++mip)
        {
            const D3D12_SUBRESOURCE_TILING& tiling = state->SubresourceTiling[mip];
            const uint32_t tileCount = tiling.WidthInTiles * tiling.HeightInTiles * tiling.DepthInTiles;
            if (tileCount == 0)
            {
                continue;
            }
            if (!m_TilePool->Allocate(tileCount, state->MappedTiles[mip]))
            {
                Core::Logger::Error(
                    "DX12",
                    "タイルプール不足のためテクスチャ(SRV " + std::to_string(texture->GetSrvIndex()) + ", " +
                        std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + ", ミップ" +
                        std::to_string(mip) + ")の常駐化を中止します");
                return false;
            }
            MapStandardMip(resource.Get(), *state, mip, state->MappedTiles[mip]);
        }

        // --- 粗くする方向: [oldFirstMip, firstMip) を外す。**ここではまだ外さない** ---
        if (firstMip > oldFirstMip)
        {
            pending->UnmapFirstMip = oldFirstMip;
            pending->UnmapMipCount = std::min(firstMip, standardMips) - std::min(oldFirstMip, standardMips);
            for (uint32_t mip = oldFirstMip; mip < std::min(firstMip, standardMips); ++mip)
            {
                pending->TilesToRelease.insert(
                    pending->TilesToRelease.end(), state->MappedTiles[mip].begin(), state->MappedTiles[mip].end());
                state->MappedTiles[mip].clear();
            }
        }

        return true;
    }

    // 【第3段】新しく貼ったミップへ画像データを流し込む
    bool DX12Device::UploadTiledMipContents(
        const TiledTextureDesc& desc, const TextureImage& image, DX12Texture* texture,
        const Microsoft::WRL::ComPtr<ID3D12Resource>& resource, const DX12TiledTextureState* state,
        uint32_t firstMip, uint32_t oldFirstMip)
    {
        const DirectX::TexMetadata& imageMeta = image.GetMetadata();
        const uint32_t uploadFirst = firstMip;
        const uint32_t uploadCount = std::min<uint32_t>(
            static_cast<uint32_t>(imageMeta.mipLevels), std::min(oldFirstMip, state->MipLevels) - firstMip);
        if (uploadCount > 0)
        {
            std::vector<D3D12_SUBRESOURCE_DATA> subresources(uploadCount);
            for (uint32_t i = 0; i < uploadCount; ++i)
            {
                const DirectX::Image* src = image.GetImage().GetImage(i, 0, 0);
                if (src == nullptr)
                {
                    Core::Logger::Error(
                        "DX12",
                        "テクスチャ(SRV " + std::to_string(texture->GetSrvIndex()) + ", " +
                            std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + ", ミップ" +
                            std::to_string(uploadFirst + i) + ")の画像データを取得できず常駐化を中止します");
                    return false;
                }
                subresources[i].pData = src->pixels;
                subresources[i].RowPitch = static_cast<LONG_PTR>(src->rowPitch);
                subresources[i].SlicePitch = static_cast<LONG_PTR>(src->slicePitch);
            }

            const D3D12_RESOURCE_DESC destDesc = resource->GetDesc();
            UINT64 requiredSize = 0;
            m_Device->GetCopyableFootprints(&destDesc, uploadFirst, uploadCount, 0, nullptr, nullptr, nullptr, &requiredSize);

            const D3D12_RESOURCE_BARRIER toCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
                resource.Get(),
                texture->IsTiled() ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COPY_DEST);
            m_UploadCommandList->ResourceBarrier(1, &toCopyDest);

            Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer = CreateUploadBuffer(requiredSize);
            UpdateSubresources(
                m_UploadCommandList.Get(), resource.Get(), uploadBuffer.Get(), 0, uploadFirst, uploadCount,
                subresources.data());

            const D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_UploadCommandList->ResourceBarrier(1, &toSrv);

            UploadSubmitAndWait();
        }

        return true;
    }

    std::unique_ptr<IRHIPendingTextureContents> DX12Device::PrepareTiledTextureResidency(
        IRHITexture* target, const TiledTextureDesc& desc, const TextureImage& image, uint32_t firstMip)
    {
        if (m_TiledResourcesTier == 0)
        {
            return nullptr;
        }

        auto* texture = static_cast<DX12Texture*>(target);
        if (texture == nullptr || !texture->HasSrv() || texture->HasRtv() || texture->HasDsv() ||
            texture->HasUav() || texture->GetSrvUavHeap() != m_AssetSrvCpuHeap.get())
        {
            return nullptr;
        }
        if (desc.MipLevels == 0 || firstMip >= desc.MipLevels)
        {
            return nullptr;
        }

        // アップロード経路とタイルプールをまとめて直列化する。
        // UpdateTileMappingsはキューの操作で、その後に投入するコマンドリストから見える
        std::lock_guard<std::mutex> uploadLock(m_UploadMutex);

        if (!m_TilePool)
        {
            m_TilePool = std::make_unique<DX12TilePool>(m_Device.Get());
        }

        auto pending = std::make_unique<DX12PendingTextureContents>(
            texture, nullptr, D3D12_SHADER_RESOURCE_VIEW_DESC{}, desc.MipLevels);
        pending->IsTiledResidency = true;
        pending->TiledFirstMip = firstMip;

        DX12TiledTextureState* state = texture->GetTiledState();
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;

        if (state == nullptr)
        {
            auto newState = CreateReservedTiledResource(desc, resource);
            if (!newState)
            {
                return nullptr;
            }

            state = newState.get();
            pending->NewTiledState = std::move(newState);
            pending->Resource = resource;
            // 初回は「常駐なし」から始めて、下の共通処理でfirstMipまで貼る
            state->ResidentFirstMip = state->MipLevels;
        }
        else
        {
            resource = texture->GetResource();
        }

        const uint32_t standardMips = state->PackedMipInfo.NumStandardMips;
        const uint32_t oldFirstMip = state->ResidentFirstMip;

        if (!UpdateTiledMipResidency(desc, texture, resource, state, firstMip, oldFirstMip, standardMips, pending.get()))
        {
            return nullptr;
        }

        if (!UploadTiledMipContents(desc, image, texture, resource, state, firstMip, oldFirstMip))
        {
            return nullptr;
        }

        // SRVは「全ミップを持つが、firstMipより細かい段はサンプルさせない」形にする。
        // マップしていないミップを読ませないための下限がResourceMinLODClamp
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = state->Format;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = state->MipLevels;
        srvDesc.Texture2D.ResourceMinLODClamp = static_cast<float>(firstMip);
        pending->SrvDesc = srvDesc;
        return pending;
    }

    void DX12Device::GetTilePoolUsage(uint64_t& outReservedBytes, uint64_t& outUsedBytes) const
    {
        outReservedBytes = 0;
        outUsedBytes = 0;
        if (!m_TilePool)
        {
            return;
        }
        outReservedBytes = m_TilePool->GetReservedBytes();
        outUsedBytes = static_cast<uint64_t>(m_TilePool->GetUsedTileCount()) * DX12TilePool::kTileSizeBytes;
    }

    void DX12Device::RetireTileMapping(
        Microsoft::WRL::ComPtr<ID3D12Resource> resource, uint32_t firstMip, uint32_t mipCount,
        std::vector<DX12TilePool::Tile> tiles)
    {
        if (!resource || (mipCount == 0 && tiles.empty()))
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_RetiredTileMappingsMutex);
        m_RetiredTileMappings.push_back(
            RetiredTileMapping{ std::move(resource), firstMip, mipCount, std::move(tiles), 0 });
    }

    void DX12Device::CollectRetiredTileMappings(bool releaseAll)
    {
        std::vector<RetiredTileMapping> ready;
        {
            std::lock_guard<std::mutex> lock(m_RetiredTileMappingsMutex);
            if (m_RetiredTileMappings.empty())
            {
                return;
            }

            const uint64_t completed = m_Fence ? m_Fence->GetCompletedValue() : 0;
            for (auto it = m_RetiredTileMappings.begin(); it != m_RetiredTileMappings.end();)
            {
                const bool due = releaseAll || (it->FenceValue != 0 && it->FenceValue <= completed);
                if (due)
                {
                    ready.push_back(std::move(*it));
                    it = m_RetiredTileMappings.erase(it);
                    continue;
                }
                if (it->FenceValue == 0)
                {
                    it->FenceValue = m_FenceValue;
                }
                ++it;
            }
        }

        for (RetiredTileMapping& entry : ready)
        {
            // NULLマッピングで外してからプールへ返す。逆順にすると、返したタイルが
            // 別のテクスチャへ貼られたあとにこちらの古いマッピングが残ることになる
            for (uint32_t i = 0; i < entry.MipCount; ++i)
            {
                const uint32_t mip = entry.FirstMip + i;
                D3D12_TILED_RESOURCE_COORDINATE start{};
                start.Subresource = mip;
                D3D12_TILE_REGION_SIZE region{};
                region.NumTiles = 0;
                region.UseBox = FALSE;

                // 領域のタイル数はリソース側から引き直す(状態を持ち回らずに済ませる)
                UINT numTiles = 0;
                D3D12_PACKED_MIP_INFO packed{};
                D3D12_TILE_SHAPE shape{};
                UINT subresourceCount = 1;
                D3D12_SUBRESOURCE_TILING tiling{};
                m_Device->GetResourceTiling(entry.Resource.Get(), &numTiles, &packed, &shape, &subresourceCount, mip, &tiling);
                region.NumTiles = tiling.WidthInTiles * tiling.HeightInTiles * tiling.DepthInTiles;
                if (region.NumTiles == 0)
                {
                    continue;
                }

                const D3D12_TILE_RANGE_FLAGS rangeFlag = D3D12_TILE_RANGE_FLAG_NULL;
                const UINT rangeTileCount = region.NumTiles;
                m_CommandQueue->UpdateTileMappings(
                    entry.Resource.Get(), 1, &start, &region, nullptr, 1, &rangeFlag, nullptr, &rangeTileCount,
                    D3D12_TILE_MAPPING_FLAG_NONE);
            }

            if (m_TilePool && !entry.Tiles.empty())
            {
                m_TilePool->Free(entry.Tiles);
            }
        }
    }

    void DX12Device::RetireResource(Microsoft::WRL::ComPtr<ID3D12Resource> resource)
    {
        if (!resource)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_RetiredResourcesMutex);
        m_RetiredResources.push_back(RetiredResource{ std::move(resource), 0 });
    }

    void DX12Device::CollectRetiredResources(bool releaseAll)
    {
        std::lock_guard<std::mutex> lock(m_RetiredResourcesMutex);
        if (m_RetiredResources.empty())
        {
            return;
        }

        if (releaseAll)
        {
            // WaitForGPUIdle直後専用。GPUは何も実行していないので無条件に解放してよい
            m_RetiredResources.clear();
            return;
        }

        const uint64_t completed = m_Fence ? m_Fence->GetCompletedValue() : 0;
        auto removeFrom = std::remove_if(
            m_RetiredResources.begin(), m_RetiredResources.end(),
            [completed](const RetiredResource& entry) {
                return entry.FenceValue != 0 && entry.FenceValue <= completed;
            });
        m_RetiredResources.erase(removeFrom, m_RetiredResources.end());

        // 未押印のものへ、直前のフレームがシグナルしたフェンス値を押す。
        // この値が完了した時点で、差し替えを行ったフレームまでのGPU実行はすべて終わっている
        for (RetiredResource& entry : m_RetiredResources)
        {
            if (entry.FenceValue == 0)
            {
                entry.FenceValue = m_FenceValue;
            }
        }
    }
}
