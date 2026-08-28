#pragma once

#include <cstdint>
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

#include "RHI/IRHITexture.h"

namespace Kurenai::RHI
{
    class DX12Device;
    class DX12DescriptorHeap;

    // リソース本体に加え、現在のリソース状態(バリア用)とSRV/RTV/DSVの各ディスクリプタインデックスを保持する。
    // RTV/DSVを持たない場合はkInvalidを格納する
    class DX12Texture : public IRHITexture
    {
    public:
        static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

        // srvUavHeapは、srvIndex/uavIndex/mipUavIndicesを確保した非シェーダー可視ヒープ。
        // このヒープはアセット用と描画用の2本に分かれており(DX12Device::GetAssetSrvCpuHeap参照)、
        // どちらから確保したかを覚えておかないとデストラクタで別のヒープへ返してしまうため保持する。
        // rtvIndex/dsvIndex/sliceDsvIndicesは常にデバイスのRTV/DSVヒープなので保持不要
        DX12Texture(
            DX12Device* device,
            DX12DescriptorHeap* srvUavHeap,
            Microsoft::WRL::ComPtr<ID3D12Resource> resource,
            D3D12_RESOURCE_STATES initialState,
            uint32_t srvIndex,
            uint32_t rtvIndex,
            uint32_t dsvIndex,
            uint32_t uavIndex = kInvalid,
            std::vector<uint32_t> mipUavIndices = {},
            std::vector<uint32_t> sliceDsvIndices = {},
            uint32_t cubeCount = 1);
        ~DX12Texture() override;

        ID3D12Resource* GetResource() const { return m_Resource.Get(); }

        // 対応するビューを持っているか。持っていないインデックス(kInvalid)でGetCpuHandleを呼ぶと
        // 「ヒープ先頭 + 0xFFFFFFFF × ディスクリプタサイズ」というでたらめなハンドルができ、
        // それをそのままD3D12へ渡すとデバイス削除(TDR)に至る。DX11は同じ状況でnullptrを
        // バインドして静かに描画をやめるだけなので、呼び出し側はこれで事前に判定する
        bool HasSrv() const { return m_SrvIndex != kInvalid; }
        bool HasRtv() const { return m_RtvIndex != kInvalid; }
        bool HasDsv() const { return m_DsvIndex != kInvalid || !m_SliceDsvIndices.empty(); }

        D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpuHandle() const;
        D3D12_CPU_DESCRIPTOR_HANDLE GetRtvCpuHandle() const;
        // CreateDepthTextureArrayで作成した場合のみスライスごとの個別DSVを持ち、arraySliceで選択する
        // (通常のCreateDepthTextureは単一DSVのため既定値の0でそのDSVが返る)
        D3D12_CPU_DESCRIPTOR_HANDLE GetDsvCpuHandle(uint32_t arraySlice = 0) const;
        // 深度スライス数(0 = テクスチャ配列ではない通常の深度テクスチャ)。範囲外指定時のログ用
        uint32_t GetDepthSliceCount() const { return static_cast<uint32_t>(m_SliceDsvIndices.size()); }
        // CreateUAVTexture/CreateHiZTextureで作成した場合のみ有効(コンピュートシェーダーからのRW用)。
        // CreateHiZTextureのミップチェーンテクスチャはミップごとに個別のUAVを持つためmipLevelで選択する
        // (CreateUAVTextureは常に1ミップのみなので既定値の0で単一UAVが返る)
        D3D12_CPU_DESCRIPTOR_HANDLE GetUavCpuHandle(uint32_t mipLevel = 0) const;

        // キューブマップ(CreateUAVTextureCube/CreateMippedUAVTextureCube/
        // CreateMippedUAVTextureCubeArray)専用。m_MipUavIndicesに
        // (mip*cubeCount + cubeIndex)*kCubeFaceCount + face の順でフラットに格納しているため、
        // この3引数版で個別のキューブ・面・ミップのUAVを取り出す。
        // 範囲外を指定した場合はログを出してハンドル0を返す(呼び出し側で無効判定できるようにする)
        static constexpr uint32_t kCubeFaceCount = 6;
        D3D12_CPU_DESCRIPTOR_HANDLE GetCubeUavCpuHandle(uint32_t face, uint32_t mipLevel = 0, uint32_t cubeIndex = 0) const;

        // 現在の状態と異なる場合のみバリアを発行して遷移する
        void TransitionTo(ID3D12GraphicsCommandList* commandList, D3D12_RESOURCE_STATES newState);

        uint32_t GetWidth() const override { return m_Width; }
        uint32_t GetHeight() const override { return m_Height; }
        uint32_t GetMipLevels() const override { return m_MipLevels; }

        // このテクスチャのSRVが確保されている非シェーダー可視ヒープと、その中の番号。
        // DX12Device::ReplaceTextureContentsが「同じ番号のディスクリプタを作り直す」ために引く
        DX12DescriptorHeap* GetSrvUavHeap() const { return m_SrvUavHeap; }
        uint32_t GetSrvIndex() const { return m_SrvIndex; }
        // UAVを持つか(HasRtv/HasDsvは上で定義済み)。ストリーミングの差し替え対象は
        // SRVしか持たないアセット由来のテクスチャに限るため、その判定に使う
        bool HasUav() const { return m_UavIndex != kInvalid || !m_MipUavIndices.empty(); }

        // リソース実体を差し替え、古いものを返す(呼び出し側が遅延解放キューへ積む)。
        // SRV番号もbindless番号も変えないため、Assets::Meshが持つIRHITexture*の
        // 生ポインタも、シェーダーが定数バッファ経由で持つbindless番号も貼り替えずに済む。
        // ディスクリプタの作り直しとbindlessの再登録は呼び出し側(DX12Device)が行う
        Microsoft::WRL::ComPtr<ID3D12Resource> SwapResource(
            Microsoft::WRL::ComPtr<ID3D12Resource> newResource, D3D12_RESOURCE_STATES newState);

        uint32_t GetBindlessIndex() const override { return m_BindlessIndex; }
        // DX12Device::RegisterBindlessが払い出した番号を控える。
        // デストラクタでこの番号をbindless区画へ返却する
        void SetBindlessIndex(uint32_t index) { m_BindlessIndex = index; }

    private:
        DX12Device* m_Device;
        // m_SrvIndex / m_UavIndex / m_MipUavIndices の確保元(コンストラクタのコメント参照)
        DX12DescriptorHeap* m_SrvUavHeap;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_Resource;
        D3D12_RESOURCE_STATES m_CurrentState;
        uint32_t m_SrvIndex;
        uint32_t m_RtvIndex;
        uint32_t m_DsvIndex;
        uint32_t m_UavIndex;
        std::vector<uint32_t> m_MipUavIndices;
        std::vector<uint32_t> m_SliceDsvIndices;
        // キューブマップ配列の枚数(非キューブマップ・単一キューブは1)。m_MipUavIndicesのフラット添字計算に使う
        uint32_t m_CubeCount = 1;
        // ミップ0のピクセルサイズとミップ段数。生成経路が多く引数からは受け取れないため、
        // コンストラクタでリソース記述子(GetDesc)から求めて控える
        uint32_t m_Width = 0;
        uint32_t m_Height = 0;
        uint32_t m_MipLevels = 0;

        // m_Resourceの記述子からm_Width/m_Height/m_MipLevelsを取り直す。
        // コンストラクタとSwapResourceの両方から呼ぶ(片方だけ更新する事故を防ぐ)
        void CaptureDimensionsFromResource();
        // bindless区画に登録されている場合のみ有効(既定は未登録)。
        // 番号の意味と返却の作法はDX12BindlessTableを参照
        uint32_t m_BindlessIndex = kInvalidBindlessIndex;
    };
}
