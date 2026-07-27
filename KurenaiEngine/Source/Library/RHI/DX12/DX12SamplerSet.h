#pragma once

#include <cstdint>
#include <d3d12.h>

#include "RHI/IRHISamplerSet.h"

namespace Kurenai::RHI
{
    class DX12Device;

    // シェーダ可視Samplerヒープ上の連続したkSamplerSlotCount個のディスクリプタ(=1つのディスクリプタテーブル)。
    // 中身は生成時に書き込んだきり変更しないため、SetSamplerSetがやることは
    // 「ルートディスクリプタテーブルをこのブロックの先頭へ向ける」だけで済む。
    // ヒープの同じ場所を毎回書き換える方式で起きる上書き問題(IRHISamplerSet.h参照)を、
    // 「そもそも書き換えない」ことで回避している
    class DX12SamplerSet : public IRHISamplerSet
    {
    public:
        DX12SamplerSet(DX12Device* device, uint32_t baseDescriptorIndex);

        // ルートディスクリプタテーブルへ渡す、このセットの先頭ディスクリプタのGPUハンドル
        D3D12_GPU_DESCRIPTOR_HANDLE GetBaseGpuHandle() const;
        // ヒープ上の先頭インデックス。ルートシグネチャ再設定後にテーブルを張り直すため、
        // DX12CommandListが「直近に使ったセット」として保持するのに使う
        uint32_t GetBaseDescriptorIndex() const { return m_BaseDescriptorIndex; }

    private:
        DX12Device* m_Device;
        uint32_t m_BaseDescriptorIndex;
    };
}
