#include "DX11Buffer.h"

#include <cstring>
#include <string>
#include <utility>

#include "Core/Logger.h"

namespace Kurenai::RHI
{
    DX11Buffer::DX11Buffer(
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer,
        uint32_t strideInBytes,
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav,
        bool isIndirectArgs)
        : m_Buffer(std::move(buffer))
        , m_StrideInBytes(strideInBytes)
        , m_Uav(std::move(uav))
        , m_IsIndirectArgs(isIndirectArgs)
    {
    }

    DX11Buffer::DX11Buffer(
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer,
        uint32_t strideInBytes,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv,
        bool isDynamic,
        bool isImmutable)
        : m_Buffer(std::move(buffer))
        , m_StrideInBytes(strideInBytes)
        , m_Srv(std::move(srv))
        , m_IsDynamic(isDynamic)
        , m_IsImmutable(isImmutable)
    {
    }

    DX11Buffer::DX11Buffer(
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer,
        uint32_t strideInBytes,
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv)
        : m_Buffer(std::move(buffer))
        , m_StrideInBytes(strideInBytes)
        , m_Uav(std::move(uav))
        , m_Srv(std::move(srv))
    {
    }

    DX11Buffer::DX11Buffer(
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer,
        uint32_t sizeInBytes,
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context)
        : m_Buffer(std::move(buffer))
        , m_StrideInBytes(0)
        , m_IsReadback(true)
        , m_Context(std::move(context))
        , m_SizeInBytes(sizeInBytes)
    {
    }

    bool DX11Buffer::ReadbackData(void* outData, uint32_t sizeInBytes)
    {
        if (!m_IsReadback)
        {
            Core::Logger::Error("DX11", "ReadbackData: BufferUsage::Readback以外のバッファから読もうとしました");
            return false;
        }
        if (outData == nullptr || sizeInBytes == 0)
        {
            Core::Logger::Error("DX11", "ReadbackData: 出力先がnullptrかサイズが0です");
            return false;
        }
        if (sizeInBytes > m_SizeInBytes)
        {
            Core::Logger::Error(
                "DX11",
                "ReadbackData: 要求サイズ(" + std::to_string(sizeInBytes) + ")がバッファサイズ(" +
                    std::to_string(m_SizeInBytes) + ")を超えています");
            return false;
        }
        if (!m_Context)
        {
            Core::Logger::Error("DX11", "ReadbackData: デバイスコンテキストがありません");
            return false;
        }

        // 【DO_NOT_WAITでGPUを待たない】待つとCPUがGPUに追いつくまで止まり、
        // 計測のために計測対象を変えてしまう(IRHIBuffer::ReadbackDataのコメント参照)。
        // まだコピーが終わっていなければDXGI_ERROR_WAS_STILL_DRAWINGが返るので、
        // そのフレームは諦めて呼び出し側に判断を返す。**これはエラーではない**ので
        // ログは出さない ―― 毎フレーム出るとログが埋まる
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = m_Context->Map(m_Buffer.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        {
            return false;
        }
        if (FAILED(hr))
        {
            Core::Logger::Error("DX11", "ReadbackData: リードバックバッファのMapに失敗しました");
            return false;
        }

        std::memcpy(outData, mapped.pData, sizeInBytes);
        m_Context->Unmap(m_Buffer.Get(), 0);
        return true;
    }
}
