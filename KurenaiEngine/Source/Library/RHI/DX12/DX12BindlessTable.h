#pragma once

#include <cstdint>
#include <d3d12.h>
#include <mutex>
#include <vector>

namespace Kurenai::RHI
{
    class DX12DescriptorHeap;

    // シェーダ可視CBV_SRV_UAVヒープの中に確保した「bindless区画」を管理する。
    //
    // HLSLのResourceDescriptorHeap[i]は、そのとき束ねられているシェーダ可視ヒープの
    // 先頭からi番目のディスクリプタを指す。つまりbindlessで引けるようにするとは
    // 「シェーダ可視ヒープ上の恒久的な場所へディスクリプタを1つ置き、その番号を覚える」
    // だけのことで、ディスクリプタテーブルもルートパラメータも介さない。
    //
    // 【なぜ専用クラスにするのか】このエンジンのシェーダ可視SRVヒープは、描画のたびに
    // テーブル用のブロックを払い出すリングとして使われている(DX12Device::AllocateSrvTableBlock)。
    // リングは古い領域を上書きし続けるため、恒久的に生かしておきたいbindlessの
    // ディスクリプタをそこへ置くことはできない。ヒープ末尾に固定長の区画を切り出し、
    // リングとは別の寿命管理(登録/解放のフリーリスト)をこのクラスが受け持つ。
    //
    // 【DX12DescriptorHeapのAllocateを使わない理由】シェーダ可視SRVヒープの
    // 内部インデックス(m_NextIndex)はリング側が一切使っておらず、ここで
    // Allocate/Freeを混ぜると「ヒープ全体の番号空間」と「区画内の番号空間」が
    // 混ざって分かりにくくなる。区画の先頭と容量だけ受け取り、番号の管理は自前で持つ。
    //
    // 【ロックを持つ】このエンジンの非シェーダー可視ヒープは「触るスレッドごとに別ヒープ」に
    // することでロック無しを実現しているが(DX12Device::GetAssetSrvCpuHeap参照)、
    // bindless区画は番号空間を1本にしないと意味がないため分割できない。
    // 登録はリソース生成時にしか起きず頻度が低いので、素直にmutexで守る
    class DX12BindlessTable
    {
    public:
        // heapは区画を切り出す先のシェーダ可視ヒープ、baseIndexはその中での区画先頭、
        // capacityは区画のディスクリプタ数。
        // このヒープの番号割り当てはDX12Device側の定数計算だけで決まっており
        // (DX12DescriptorHeap内部のAllocate/AllocateBlockは使っていない)、
        // baseIndexにはリング2区画の総容量、つまりヒープ末尾に切り出した区画の先頭が渡される
        DX12BindlessTable(ID3D12Device* device, DX12DescriptorHeap* heap, uint32_t baseIndex, uint32_t capacity);

        DX12BindlessTable(const DX12BindlessTable&) = delete;
        DX12BindlessTable& operator=(const DX12BindlessTable&) = delete;

        // 非シェーダー可視ヒープ上のSRV(またはUAV)をbindless区画へコピーし、
        // シェーダーが使う「ヒープ先頭からの絶対番号」を返す。
        // 区画が満杯の場合はログを出してkInvalidBindlessIndexを返す(例外は投げない。
        // bindlessが引けないだけで描画は従来経路で続行できるため)
        uint32_t Register(D3D12_CPU_DESCRIPTOR_HANDLE sourceCpuHandle);

        // 既に払い出されている番号の中身だけを、新しいディスクリプタで上書きする。
        // 番号は変わらないため、シェーダーが定数バッファ経由で持っている番号を貼り替えずに済む。
        //
        // 【何のためにあるか】テクスチャストリーミングは常駐ミップを変えるたびに
        // ID3D12Resourceを作り直すが、そのたびにUnregister/Registerすると
        // フリーリスト経由で番号が変わりうる。ここで同じ番号へ差し替える。
        //
        // 【GPUが実行中のディスクリプタを書き換えることになる】D3D12は実行中の
        // コマンドリストが参照しているディスクリプタの変更を許さない。ただしここで
        // 指す先は生きている新しいリソース(古い方はDX12Device::RetireResourceで
        // Nフレーム生かす)なので、最悪でも「1フレーム早く新しい方をサンプルする」に留まる。
        // GPUベース検証がこれを咎める場合は、呼び出し側でフレーム境界まで遅らせること
        bool Rebind(uint32_t heapIndex, D3D12_CPU_DESCRIPTOR_HANDLE sourceCpuHandle);

        // Registerが返した番号を返却する。kInvalidBindlessIndexを渡した場合は何もしない
        // (呼び出し側が「登録したかどうか」を分岐せずにデストラクタから呼べるようにするため)。
        //
        // 【返却してもディスクリプタは消さない】解放直後にGPUがまだそのスロットを
        // 読んでいる可能性があるが、番号が再利用されるのは次のRegisterのときで、
        // そこで新しいディスクリプタが上書きされる。テクスチャの破棄は
        // IRHIDevice::WaitForGPUIdle()の後に行う運用(Assets::RaytracingScene::Resetの
        // 注意書きと同じ)なので、この扱いで足りる
        void Unregister(uint32_t heapIndex);

        uint32_t GetBaseIndex() const { return m_BaseIndex; }
        uint32_t GetCapacity() const { return m_Capacity; }
        // 現在登録されている数(ImGuiの表示・ログ用)
        uint32_t GetUsedCount() const;

    private:
        ID3D12Device* m_Device = nullptr;
        DX12DescriptorHeap* m_Heap = nullptr;
        uint32_t m_BaseIndex = 0;
        uint32_t m_Capacity = 0;
        // 区画内で次に払い出す相対番号(フリーリストが空のときに使う)
        uint32_t m_NextOffset = 0;
        std::vector<uint32_t> m_FreeList;
        mutable std::mutex m_Mutex;
    };
}
