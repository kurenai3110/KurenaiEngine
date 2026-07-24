#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "RHI/IRHISampler.h"

namespace Kurenai::RHI
{
    class DX11Sampler : public IRHISampler
    {
    public:
        explicit DX11Sampler(Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler);

        ID3D11SamplerState* GetSamplerState() const { return m_Sampler.Get(); }

    private:
        Microsoft::WRL::ComPtr<ID3D11SamplerState> m_Sampler;
    };
}
