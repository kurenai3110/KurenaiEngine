#include "DX11Buffer.h"

#include <utility>

namespace Kurenai::RHI
{
    DX11Buffer::DX11Buffer(Microsoft::WRL::ComPtr<ID3D11Buffer> buffer, uint32_t strideInBytes)
        : m_Buffer(std::move(buffer))
        , m_StrideInBytes(strideInBytes)
    {
    }
}
