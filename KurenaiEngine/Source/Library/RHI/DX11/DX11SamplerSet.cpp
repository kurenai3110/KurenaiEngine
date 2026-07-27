#include "DX11SamplerSet.h"

#include <utility>

namespace Kurenai::RHI
{
    DX11SamplerSet::DX11SamplerSet(std::vector<Microsoft::WRL::ComPtr<ID3D11SamplerState>> samplers)
        : m_Samplers(std::move(samplers))
    {
        m_RawSamplers.reserve(m_Samplers.size());
        for (const auto& sampler : m_Samplers)
        {
            m_RawSamplers.push_back(sampler.Get());
        }
    }
}
