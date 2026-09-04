#include "DX12Texture.h"

#include <cstring>
#include <string>
#include <utility>

#include "Core/Logger.h"

#include "DX12Device.h"

namespace Kurenai::RHI
{
    DX12Texture::DX12Texture(
        DX12Device* device,
        DX12DescriptorHeap* srvUavHeap,
        Microsoft::WRL::ComPtr<ID3D12Resource> resource,
        D3D12_RESOURCE_STATES initialState,
        uint32_t srvIndex,
        uint32_t rtvIndex,
        uint32_t dsvIndex,
        uint32_t uavIndex,
        std::vector<uint32_t> mipUavIndices,
        std::vector<uint32_t> sliceDsvIndices,
        uint32_t cubeCount)
        : m_Device(device)
        , m_SrvUavHeap(srvUavHeap)
        , m_Resource(std::move(resource))
        , m_CurrentState(initialState)
        , m_SrvIndex(srvIndex)
        , m_RtvIndex(rtvIndex)
        , m_DsvIndex(dsvIndex)
        , m_UavIndex(uavIndex)
        , m_MipUavIndices(std::move(mipUavIndices))
        , m_SliceDsvIndices(std::move(sliceDsvIndices))
        , m_CubeCount(cubeCount)
    {
        // 生成経路が多く引数では寸法を受け取らないため、リソース記述子から求めて控える
        // (経路ごとに記録すると追加時に漏れる。DX11Textureも同じ方針)
        CaptureDimensionsFromResource();
    }

    DX12Texture::DX12Texture(
        Microsoft::WRL::ComPtr<ID3D12Resource> readbackResource, std::unique_ptr<DX12ReadbackState> readbackState)
        : m_Device(nullptr)
        , m_SrvUavHeap(nullptr)
        , m_Resource(std::move(readbackResource))
        // READBACKヒープのリソースはCOPY_DEST状態から動かせない(D3D12の仕様)
        , m_CurrentState(D3D12_RESOURCE_STATE_COPY_DEST)
        , m_SrvIndex(kInvalid)
        , m_RtvIndex(kInvalid)
        , m_DsvIndex(kInvalid)
        , m_UavIndex(kInvalid)
        , m_Readback(std::move(readbackState))
    {
        // 【CaptureDimensionsFromResourceを呼ばない】実体はバッファリソースなので、
        // GetDescはWidth=バイト数・Height=1を返す。上位層が見る寸法はテクセル単位でなければ
        // ならないため、コピー元から求めておいたFootprintの値を入れる
        if (m_Readback)
        {
            m_Width = m_Readback->Desc.Width;
            m_Height = m_Readback->Desc.Height;
            m_MipLevels = 1;
        }
    }

    TextureReadbackDesc DX12Texture::GetReadbackDesc(uint32_t mipLevel) const
    {
        if (!m_Readback)
        {
            // リードバック用ではないテクスチャ。ElementType::Unknownのまま返し、
            // 呼び出し側に「読めない」と分かる形にする
            return TextureReadbackDesc{};
        }
        if (mipLevel != 0)
        {
            // 受け皿は常に1サブリソースぶん(CreateReadbackTextureの時点でミップを選んである)。
            // ここへ0以外が来るのは呼び出し側の取り違えなので、黙って0を返さずログを出す
            Core::Logger::Error(
                "DX12",
                "GetReadbackDesc: リードバック用テクスチャは常に1サブリソースぶんです "
                "(mipLevel=" + std::to_string(mipLevel) + " が指定されました)");
            return TextureReadbackDesc{};
        }
        return m_Readback->Desc;
    }

    bool DX12Texture::ReadbackData(void* outData, uint32_t sizeInBytes)
    {
        if (!m_Readback)
        {
            Core::Logger::Error("DX12", "ReadbackData: リードバック用ではないテクスチャから読もうとしました");
            return false;
        }
        if (outData == nullptr || sizeInBytes == 0)
        {
            Core::Logger::Error("DX12", "ReadbackData: 出力先がnullptrかサイズが0です");
            return false;
        }
        if (m_Readback->MappedPtr == nullptr)
        {
            Core::Logger::Error("DX12", "ReadbackData: リードバックテクスチャがマップされていません");
            return false;
        }

        const TextureReadbackDesc& desc = m_Readback->Desc;
        // パディングを剥がしたあとの必要バイト数。呼び出し側にはこれを要求する
        const uint32_t tightRowPitch = desc.Width * desc.BytesPerTexel;
        const uint64_t tightTotal = static_cast<uint64_t>(tightRowPitch) * desc.Height;
        if (sizeInBytes < tightTotal)
        {
            Core::Logger::Error(
                "DX12",
                "ReadbackData: 出力先のサイズ(" + std::to_string(sizeInBytes) + ")が必要量(" +
                    std::to_string(tightTotal) + ")に足りません");
            return false;
        }

        // 【行のパディングをここで剥がす】GetCopyableFootprintsのRowPitchは256バイト境界へ
        // 切り上げられているため、行ごとにコピーしてタイトに詰め直す。
        // 上位層はパディングの存在を一切見ない(IRHITexture::ReadbackDataのコメント参照)。
        //
        // 【GPUの完了を待たない】READBACKヒープは作成時から永続マップしてあり、ここは単なるコピー。
        // コピーコマンドがまだ実行されていなければ古い内容が返るが、待って直列化するよりは
        // 呼び出し側に「十分に古いものを読む」責務を持たせるほうがよい
        const auto* src = static_cast<const uint8_t*>(m_Readback->MappedPtr) + m_Readback->Footprint.Offset;
        auto* dst = static_cast<uint8_t*>(outData);
        const uint32_t paddedRowPitch = m_Readback->Footprint.Footprint.RowPitch;
        for (uint32_t y = 0; y < desc.Height; ++y)
        {
            std::memcpy(
                dst + static_cast<size_t>(y) * tightRowPitch,
                src + static_cast<size_t>(y) * paddedRowPitch,
                tightRowPitch);
        }
        return true;
    }

    void DX12Texture::SetDebugName(const char* name)
    {
        if (!m_Resource || name == nullptr || name[0] == '\0')
        {
            return;
        }

        // ID3D12Object::SetNameはワイド文字しか受け付けない。名前は英数字だけの識別子
        // (メンバ名から m_ を外したもの)なので、マルチバイト変換は要らず1文字ずつ広げれば足りる
        std::wstring wide;
        wide.reserve(std::strlen(name));
        for (const char* p = name; *p != '\0'; ++p)
        {
            wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
        }
        m_Resource->SetName(wide.c_str());
    }

    void DX12Texture::CaptureDimensionsFromResource()
    {
        if (!m_Resource)
        {
            m_Width = 0;
            m_Height = 0;
            m_MipLevels = 0;
            return;
        }

        const D3D12_RESOURCE_DESC desc = m_Resource->GetDesc();
        m_Width = static_cast<uint32_t>(desc.Width);
        m_Height = desc.Height;
        m_MipLevels = desc.MipLevels;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> DX12Texture::SwapResource(
        Microsoft::WRL::ComPtr<ID3D12Resource> newResource, D3D12_RESOURCE_STATES newState)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> old = std::move(m_Resource);
        m_Resource = std::move(newResource);
        m_CurrentState = newState;
        CaptureDimensionsFromResource();
        return old;
    }

    DX12Texture::~DX12Texture()
    {
        // bindless区画への登録があれば返却する(DX12Buffer::~DX12Bufferと同じ扱い)
        if (m_Device)
        {
            if (DX12BindlessTable* table = m_Device->GetBindlessTable())
            {
                table->Unregister(m_BindlessIndex);
            }
        }

        if (m_SrvIndex != kInvalid)
        {
            m_SrvUavHeap->Free(m_SrvIndex);
        }
        if (m_RtvIndex != kInvalid)
        {
            m_Device->GetRtvHeap()->Free(m_RtvIndex);
        }
        if (m_DsvIndex != kInvalid)
        {
            m_Device->GetDsvHeap()->Free(m_DsvIndex);
        }
        if (m_UavIndex != kInvalid)
        {
            m_SrvUavHeap->Free(m_UavIndex);
        }
        for (const uint32_t mipUavIndex : m_MipUavIndices)
        {
            m_SrvUavHeap->Free(mipUavIndex);
        }
        // スライスごとのDSVはSRVヒープではなくDSVヒープから確保しているため、解放先も分ける
        // (CreateDepthTextureArrayで作成した場合はm_DsvIndexをkInvalidのままにしてあるので、
        //  上のm_DsvIndex解放と二重に解放されることはない)
        for (const uint32_t sliceDsvIndex : m_SliceDsvIndices)
        {
            m_Device->GetDsvHeap()->Free(sliceDsvIndex);
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetSrvCpuHandle() const
    {
        if (m_SrvIndex == kInvalid)
        {
            // SRVを持たないテクスチャ(深度専用など)をSetTextureへ渡した場合。無効なハンドルを
            // 作ってD3D12へ渡すとデバイス削除に至るため、0を返すnullディスクリプタで代替する
            // (DX11がnullptrのSRVをバインドしてサンプル結果が0になるのと同じ挙動)
            Core::Logger::Error("DX12", "GetSrvCpuHandle: SRVを持たないテクスチャが参照されました。nullディスクリプタで代替します");
            return m_Device->GetNullSrvCpuHandle();
        }
        return m_SrvUavHeap->GetCpuHandle(m_SrvIndex);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetRtvCpuHandle() const
    {
        // RTVには代替できる有効なディスクリプタが無いため、呼び出し側(DX12CommandList::SetRenderTargets)が
        // HasRtv()で事前に弾く。ここへ到達した時点で呼び出し側の判定漏れなので必ずログを残す
        if (m_RtvIndex == kInvalid)
        {
            Core::Logger::Error("DX12", "GetRtvCpuHandle: RTVを持たないテクスチャが参照されました");
            return D3D12_CPU_DESCRIPTOR_HANDLE{};
        }
        return m_Device->GetRtvHeap()->GetCpuHandle(m_RtvIndex);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetDsvCpuHandle(uint32_t arraySlice) const
    {
        if (!m_SliceDsvIndices.empty())
        {
            if (arraySlice >= m_SliceDsvIndices.size())
            {
                Core::Logger::Error(
                    "DX12",
                    "GetDsvCpuHandle: 深度配列スライス" + std::to_string(arraySlice) + "が範囲外です(スライス数: " +
                        std::to_string(m_SliceDsvIndices.size()) + ")");
                return D3D12_CPU_DESCRIPTOR_HANDLE{};
            }
            return m_Device->GetDsvHeap()->GetCpuHandle(m_SliceDsvIndices[arraySlice]);
        }

        if (m_DsvIndex == kInvalid)
        {
            Core::Logger::Error("DX12", "GetDsvCpuHandle: DSVを持たないテクスチャが参照されました");
            return D3D12_CPU_DESCRIPTOR_HANDLE{};
        }
        return m_Device->GetDsvHeap()->GetCpuHandle(m_DsvIndex);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetUavCpuHandle(uint32_t mipLevel) const
    {
        if (!m_MipUavIndices.empty())
        {
            if (mipLevel >= m_MipUavIndices.size())
            {
                Core::Logger::Error(
                    "DX12",
                    "GetUavCpuHandle: ミップレベル" + std::to_string(mipLevel) + "が範囲外です(ミップ数: " +
                        std::to_string(m_MipUavIndices.size()) + ")。nullディスクリプタで代替します");
                return m_Device->GetNullUavCpuHandle();
            }
            return m_SrvUavHeap->GetCpuHandle(m_MipUavIndices[mipLevel]);
        }

        if (m_UavIndex == kInvalid)
        {
            Core::Logger::Error("DX12", "GetUavCpuHandle: UAVを持たないテクスチャが参照されました。nullディスクリプタで代替します");
            return m_Device->GetNullUavCpuHandle();
        }
        return m_SrvUavHeap->GetCpuHandle(m_UavIndex);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DX12Texture::GetCubeUavCpuHandle(uint32_t face, uint32_t mipLevel, uint32_t cubeIndex) const
    {
        // m_MipUavIndicesは (mip * cubeCount + cubeIndex) * kCubeFaceCount + face の順でフラットに
        // 格納されている(キューブマップ配列でない場合はcubeCount=1で従来と同じ並びになる)
        const size_t index = (static_cast<size_t>(mipLevel) * m_CubeCount + cubeIndex) * kCubeFaceCount + face;
        if (face >= kCubeFaceCount || cubeIndex >= m_CubeCount || index >= m_MipUavIndices.size())
        {
            Core::Logger::Error(
                "DX12",
                "GetCubeUavCpuHandle: 面" + std::to_string(face) + " / ミップ" + std::to_string(mipLevel) +
                    " / キューブ" + std::to_string(cubeIndex) + "が範囲外です(キューブ数: " + std::to_string(m_CubeCount) +
                    ", UAV数: " + std::to_string(m_MipUavIndices.size()) + ")。nullディスクリプタで代替します");
            return m_Device->GetNullUavCpuHandle();
        }
        return m_SrvUavHeap->GetCpuHandle(m_MipUavIndices[index]);
    }

    void DX12Texture::TransitionTo(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES newState)
    {
        if (m_CurrentState == newState)
        {
            return;
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_Resource.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = m_CurrentState;
        barrier.Transition.StateAfter = newState;
        commandList->ResourceBarrier(1, &barrier);

        m_CurrentState = newState;
    }
}
