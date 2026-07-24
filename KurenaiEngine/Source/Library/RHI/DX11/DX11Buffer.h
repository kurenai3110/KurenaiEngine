#pragma once

#include <cstdint>
#include <d3d11.h>
#include <wrl/client.h>

#include "RHI/IRHIBuffer.h"

namespace Kurenai::RHI
{
    class DX11Buffer : public IRHIBuffer
    {
    public:
        DX11Buffer(Microsoft::WRL::ComPtr<ID3D11Buffer> buffer, uint32_t strideInBytes);

        ID3D11Buffer* GetBuffer() const { return m_Buffer.Get(); }
        uint32_t GetStride() const { return m_StrideInBytes; }

    private:
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_Buffer;
        uint32_t m_StrideInBytes;
    };
}
