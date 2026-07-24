#include "DX11Sampler.h"

#include <utility>

namespace Kurenai::RHI
{
    DX11Sampler::DX11Sampler(Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler)
        : m_Sampler(std::move(sampler))
    {
    }
}
