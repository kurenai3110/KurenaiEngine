#pragma once

#include <cstdint>
#include <d3d12.h>
#include <mutex>
#include <vector>
#include <wrl/client.h>

namespace Kurenai::RHI
{
    // タイルリソース(予約リソース)の裏付けになる物理メモリの置き場。
    //
    // 予約リソース(ID3D12Device::CreateReservedResource)は仮想アドレスだけを持ち、
    // 実体を持たない。ここで確保したID3D12Heapのタイルを
    // ID3D12CommandQueue::UpdateTileMappingsで貼り付けて初めて読み書きできるようになる。
    //
    // 【このエンジンで初めてヒープを自前で持つ】これまでのリソースはすべて
    // CreateCommittedResource(リソースごとにヒープが暗黙に作られる)だった。
    // 書き方はDX12DescriptorHeapとDX12BindlessTable(固定容量+フリーリスト+mutex)に倣う。
    //
    // 【ヒープは足りなくなったら足す】1つのヒープで足りないシーンがあるため、
    // 固定サイズのブロックを必要に応じて増やす。タイルは複数のヒープに散らばってよい
    // (D3D12では1つの予約リソースへ複数のヒープからタイルを貼れる。D3D11のタイルプールと違う点)
    class DX12TilePool
    {
    public:
        // D3D12のタイルは64KB固定。BC7なら256x256テクセル、RGBA8なら128x128テクセルを覆う
        // (正確な形はアダプタ依存なのでID3D12Device::GetResourceTilingで問い合わせること)
        static constexpr uint64_t kTileSizeBytes = 64ull * 1024ull;
        // 1ブロック(=1つのID3D12Heap)のタイル数。64MB
        static constexpr uint32_t kTilesPerBlock = 1024;

        // 1タイルの居場所。ヒープをまたいで散らばるため、ヒープ番号とその中のタイル番号で持つ
        struct Tile
        {
            uint32_t HeapIndex = 0;
            uint32_t TileIndex = 0;
        };

        explicit DX12TilePool(ID3D12Device* device);

        DX12TilePool(const DX12TilePool&) = delete;
        DX12TilePool& operator=(const DX12TilePool&) = delete;

        // count個のタイルを確保して末尾へ追加する。足りなければヒープを増やす。
        // 確保できなければログを出してfalseを返し、outTilesは変更しない
        bool Allocate(uint32_t count, std::vector<Tile>& outTiles);
        // Allocateで得たタイルを返却する
        void Free(const std::vector<Tile>& tiles);

        ID3D12Heap* GetHeap(uint32_t heapIndex) const;

        // 統計用。確保済みヒープの総バイト数と、いま貼られているタイル数
        uint64_t GetReservedBytes() const;
        uint32_t GetUsedTileCount() const;
        uint32_t GetHeapCount() const;

    private:
        // ヒープを1つ増やす。m_Mutexを保持した状態で呼ぶこと
        bool GrowLocked();

        ID3D12Device* m_Device = nullptr;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Heap>> m_Heaps;
        std::vector<Tile> m_FreeList;
        uint32_t m_UsedTileCount = 0;
        mutable std::mutex m_Mutex;
    };
}
