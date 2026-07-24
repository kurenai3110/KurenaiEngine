#pragma once

#include <cstdint>
#include <d3d12.h>

#include "RHI/IRHISampler.h"

namespace Kurenai::RHI
{
    class DX12Device;

    // サンプラーはCBV_SRV_UAVヒープとは別のSampler専用CPUヒープに恒久的なディスクリプタを1つ持つ。
    // 描画時にDX12CommandList::SetSamplerがこれをシェーダ可視Samplerヒープへコピーする
    class DX12Sampler : public IRHISampler
    {
    public:
        DX12Sampler(DX12Device* device, uint32_t descriptorIndex);
        ~DX12Sampler() override;

        D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle() const;

    private:
        DX12Device* m_Device;
        uint32_t m_DescriptorIndex;
    };
}
