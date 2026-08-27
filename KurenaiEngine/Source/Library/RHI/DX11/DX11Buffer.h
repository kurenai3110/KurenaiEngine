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
        // BufferUsage::Structured用(uavのみ)。BufferUsage::IndirectArgsも
        // raw UAVを1つだけ持つ同じ構造のため共用し、isIndirectArgsで区別する
        // (DispatchIndirectがUsageを検証するため)
        DX11Buffer(
            Microsoft::WRL::ComPtr<ID3D11Buffer> buffer,
            uint32_t strideInBytes,
            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav = nullptr,
            bool isIndirectArgs = false);

        // BufferUsage::StructuredReadOnly用: D3D11_USAGE_DYNAMICで作成し、UpdateBufferがMap/Unmap経由で
        // 書き込む。srvは読み取り専用バインド(PSSetShaderResources)用。
        // isImmutableはBufferUsage::StructuredImmutable(D3D11_USAGE_IMMUTABLE)で作った場合にtrue。
        // このUsageはMapもUpdateSubresourceも受け付けないため、UpdateBufferが弾くのに使う
        DX11Buffer(
            Microsoft::WRL::ComPtr<ID3D11Buffer> buffer,
            uint32_t strideInBytes,
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv,
            bool isDynamic,
            bool isImmutable = false);

        // BufferUsage::StructuredRW用: コンピュートがUAVで書き、ピクセルシェーダがSRVで読むため
        // 両方のビューを持つ。CPUからは書き込まないのでD3D11_USAGE_DEFAULT(isDynamicはfalse)
        DX11Buffer(
            Microsoft::WRL::ComPtr<ID3D11Buffer> buffer,
            uint32_t strideInBytes,
            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav,
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv);

        ID3D11Buffer* GetBuffer() const { return m_Buffer.Get(); }
        uint32_t GetStride() const { return m_StrideInBytes; }
        // BufferUsage::Structured / StructuredRWで作成した場合のみ非nullptr(コンピュートシェーダーからのRW用)
        ID3D11UnorderedAccessView* GetUnorderedAccessView() const { return m_Uav.Get(); }
        // BufferUsage::StructuredReadOnly / StructuredRWで作成した場合のみ非nullptr(シェーダからの読み取り用)
        ID3D11ShaderResourceView* GetShaderResourceView() const { return m_Srv.Get(); }
        // D3D11_USAGE_DYNAMICで作成されたか(UpdateBufferの分岐に使う)
        bool IsDynamic() const { return m_IsDynamic; }
        // D3D11_USAGE_IMMUTABLEで作成されたか(UpdateBufferが更新を弾くのに使う)
        bool IsImmutable() const { return m_IsImmutable; }
        // BufferUsage::IndirectArgsで作成されたか(DispatchIndirectが引数バッファを検証するのに使う)
        bool IsIndirectArgs() const { return m_IsIndirectArgs; }

    private:
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_Buffer;
        uint32_t m_StrideInBytes;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_Uav;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_Srv;
        bool m_IsDynamic = false;
        bool m_IsImmutable = false;
        bool m_IsIndirectArgs = false;
    };
}
