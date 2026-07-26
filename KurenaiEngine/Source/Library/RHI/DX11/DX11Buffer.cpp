#include "DX11Buffer.h"

#include <utility>

namespace Kurenai::RHI
{
    DX11Buffer::DX11Buffer(
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer,
        uint32_t strideInBytes,
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav)
        : m_Buffer(std::move(buffer))
        , m_StrideInBytes(strideInBytes)
        , m_Uav(std::move(uav))
    {
    }

    DX11Buffer::DX11Buffer(
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer,
        uint32_t strideInBytes,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv,
        bool isDynamic)
        : m_Buffer(std::move(buffer))
        , m_StrideInBytes(strideInBytes)
        , m_Srv(std::move(srv))
        , m_IsDynamic(isDynamic)
    {
    }
}
