#pragma once

#include <cstdint>
#include <d3d11.h>
#include <vector>
#include <wrl/client.h>

#include "RHI/IRHISamplerSet.h"

namespace Kurenai::RHI
{
    class DX11SamplerSet : public IRHISamplerSet
    {
    public:
        explicit DX11SamplerSet(std::vector<Microsoft::WRL::ComPtr<ID3D11SamplerState>> samplers);

        // PSSetSamplers/CSSetSamplersへそのまま渡せる生ポインタの連続配列。
        // ComPtrの配列はレイアウト互換を仕様上保証されないため、生ポインタの配列を別途持っておく
        ID3D11SamplerState* const* GetSamplers() const { return m_RawSamplers.data(); }
        uint32_t GetCount() const { return static_cast<uint32_t>(m_RawSamplers.size()); }

    private:
        std::vector<Microsoft::WRL::ComPtr<ID3D11SamplerState>> m_Samplers;
        std::vector<ID3D11SamplerState*> m_RawSamplers;
    };
}
