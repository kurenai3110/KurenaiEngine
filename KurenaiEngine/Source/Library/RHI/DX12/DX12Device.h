#pragma once

#include <cstdint>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <memory>
#include <mutex>
#include <wrl/client.h>

#include "DX12DescriptorHeap.h"
#include "RHI/IRHIDevice.h"

namespace DirectX
{
    struct TexMetadata;
    class ScratchImage;
}

namespace Kurenai::RHI
{
    class DX12CommandList;

    class DX12Device : public IRHIDevice
    {
    public:
        DX12Device();
        ~DX12Device() override;

        void Initialize();

        std::unique_ptr<IRHISwapChain> CreateSwapChain(void* windowHandle, uint32_t width, uint32_t height) override;
        std::unique_ptr<IRHIBuffer> CreateBuffer(const BufferDesc& desc) override;
        std::unique_ptr<IRHIShader> CreateShader(const ShaderDesc& desc) override;
        std::unique_ptr<IRHIPipelineState> CreatePipelineState(const PipelineStateDesc& desc) override;
        std::unique_ptr<IRHIPipelineState> CreateComputePipelineState(const ComputePipelineStateDesc& desc) override;
        std::unique_ptr<IRHITexture> CreateTextureFromFile(const std::wstring& filePath, bool sRGB) override;
        std::unique_ptr<IRHITexture> CreateTextureFromImage(const TextureImage& image) override;
        std::unique_ptr<IRHITexture> CreateSolidColorTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a) override;
        std::unique_ptr<IRHITexture> CreateTextureFromMemory(uint32_t width, uint32_t height, const void* pixelsRGBA8) override;
        std::unique_ptr<IRHITexture> CreateRenderTexture(uint32_t width, uint32_t height, Format format) override;
        std::unique_ptr<IRHITexture> CreateUAVTexture(uint32_t width, uint32_t height, Format format) override;
        std::unique_ptr<IRHITexture> CreateHiZTexture(uint32_t width, uint32_t height, uint32_t mipLevels) override;
        std::unique_ptr<IRHITexture> CreateDepthTexture(uint32_t width, uint32_t height, float clearDepth = 1.0f) override;
        std::unique_ptr<IRHISampler> CreateDefaultSampler() override;
        IRHICommandList* GetImmediateCommandList() override;

        std::unique_ptr<IRHIImGuiBackend> CreateImGuiBackend(void* windowHandle) override;
        std::unique_ptr<IRHIGPUProfiler> CreateGPUProfiler() override;
        float GetLastFrameGPUWaitTimeMs() const override { return m_LastFrameGPUWaitTimeMs; }

        // DX12実装内部(DX12SwapChain/DX12Texture/DX12Sampler/DX12CommandList)から利用するアクセサ
        ID3D12Device* GetDevice() const { return m_Device.Get(); }
        ID3D12CommandQueue* GetCommandQueue() const { return m_CommandQueue.Get(); }
        ID3D12GraphicsCommandList* GetCommandList() const { return m_CommandList.Get(); }
        ID3D12RootSignature* GetRootSignature() const { return m_RootSignature.Get(); }
        ID3D12RootSignature* GetComputeRootSignature() const { return m_ComputeRootSignature.Get(); }
        DX12DescriptorHeap* GetRtvHeap() const { return m_RtvHeap.get(); }
        DX12DescriptorHeap* GetDsvHeap() const { return m_DsvHeap.get(); }
        DX12DescriptorHeap* GetSrvCpuHeap() const { return m_SrvCpuHeap.get(); }
        DX12DescriptorHeap* GetSamplerCpuHeap() const { return m_SamplerCpuHeap.get(); }
        DX12DescriptorHeap* GetShaderVisibleSrvHeap() const { return m_ShaderVisibleSrvHeap.get(); }
        DX12DescriptorHeap* GetShaderVisibleSamplerHeap() const { return m_ShaderVisibleSamplerHeap.get(); }

        // 1フレーム分のコマンドをすべて記録してから1回だけExecuteCommandListsする設計のため、
        // CopyDescriptorsSimpleによるディスクリプタ書き込みはGPU実行前にすべて完了してしまう。
        // そのため同じヒープスロットを毎回使い回すと、GPUが実際にDrawを処理する時点では
        // そのフレーム最後のSetTexture呼び出しの内容にすべて上書きされてしまう。
        // これを避けるため、t0〜t6の連続したkTextureSlotCount個のブロックを描画のたびに新規に払い出す。
        // インデックスはフレームをまたいでも巻き戻さない(kFrameCountフレーム分の容量を持つ
        // リングとして扱う)ため、GPUがまだ読んでいる可能性のある直近フレームのブロックを
        // 上書きすることはない(詳細はkFrameCountのコメントを参照)
        uint32_t AllocateSrvTableBlock(uint32_t count);

        // AllocateSrvTableBlockのコンピュートシェーダー版。グラフィックス用のSRVテーブル領域とは
        // ヒープ内で別の区画(m_ComputeTableHeapOffset以降)を使うため、払い出しリング・使用量検証を
        // 別カウンタで管理する(詳細はDX12CommandList::FlushPendingComputeWrites参照)
        uint32_t AllocateComputeTableBlock(uint32_t count);

        // コマンドリストを閉じてキューへ実行投入する
        void ExecuteCommandList();
        // フェンスに新しい値をシグナルし、現在のフレームスロット(m_FrameIndex)の完了目印として記録する。
        // ExecuteCommandList()の直後、Presentより前に呼ぶ
        void SignalFrame();
        // 次のフレームスロットへ進み、そのスロットを最後に使ったフレームのGPU実行完了を待ってから
        // (kFrameCountフレーム前の実行なので通常は待たずに完了している)コマンドアロケータ/リストを
        // 開き直す。CPUはGPUの完了を待たずに次フレームの記録を始められるため、Present直後に
        // 毎回完全同期していた旧実装と比べてCPU/GPUがオーバーラップして動作する
        void AdvanceToNextFrame();
        // フェンスでGPUの完全なアイドルを待つ(全フレームスロットの実行完了を保証する)。
        // リサイズやシャットダウンなど、パイプライン化の恩恵が不要な箇所でのみ使う
        void WaitForGPUIdle() override;

    private:
        void CreateRootSignature();
        void CreateComputeRootSignature();
        // 現在のフレームスロット(m_FrameIndex)のコマンドアロケータ/リストを開き直す
        void ResetCommandList();
        Microsoft::WRL::ComPtr<ID3D12Resource> CreateUploadBuffer(uint64_t sizeInBytes);
        // 公開APIのCreateTextureFromImage(const TextureImage&)から、内部のTexMetadata/ScratchImageを
        // 取り出して実際のGPUリソース作成を行う共通処理(CreateTextureFromFile/CreateSolidColorTexture/
        // CreateTextureFromMemoryからも使う)
        std::unique_ptr<IRHITexture> CreateTextureResourceFromImage(const DirectX::TexMetadata& metadata, const DirectX::ScratchImage& image);
        // m_UploadCommandListへ記録した内容をクローズして実行投入し、完了を同期的に待ってから開き直す。
        // CreateBuffer/CreateTextureFromImageの初期データアップロード専用(詳細はm_UploadCommandListの
        // コメント参照)
        void UploadSubmitAndWait();

        // CPUがGPUの完了を待たずに次フレームの記録を始められるようにするための多重バッファリング数。
        // スワップチェインのバッファ数(DX12SwapChain::kBufferCount)と合わせて2にしておく
        static constexpr uint32_t kFrameCount = 2;

        Microsoft::WRL::ComPtr<ID3D12Device> m_Device;
        Microsoft::WRL::ComPtr<IDXGIFactory2> m_Factory;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_CommandQueue;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_CommandAllocators[kFrameCount];
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_CommandList;
        Microsoft::WRL::ComPtr<ID3D12Fence> m_Fence;
        uint64_t m_FenceValue = 0;
        // 各フレームスロットを最後に使ったフレームがシグナルしたフェンス値。
        // AdvanceToNextFrame()でそのスロットを再利用する前にこの値の完了を待つ
        uint64_t m_FrameFenceValues[kFrameCount] = {};
        uint32_t m_FrameIndex = 0;
        HANDLE m_FenceEvent = nullptr;
        // 直前のAdvanceToNextFrame()でWaitForSingleObjectに実際に費やした時間(ms)。
        // フェンスが既に満たされていて待たなかった場合は0になる
        float m_LastFrameGPUWaitTimeMs = 0.0f;

        // CreateBuffer/CreateTextureFromImageの初期データアップロード専用のコマンドリスト/アロケータ/
        // フェンス。m_CommandList(GetImmediateCommandList、毎フレームRenderスレッドが使う)とは
        // 完全に独立させてある。これらを共有していた旧実装では、シーン切り替え(LoadScene)のような
        // Render()呼び出しの外からのリソース作成が、Render()が記録中のコマンドリストを
        // Close/Reset/実行してしまい、設定済みのレンダーターゲット/パイプラインステート等を
        // 破壊するバグがあった(KurenaiEngine2D::BuildFontAtlasをBeginFrame後に呼べない制約の原因もこれ)。
        // 独立させたことで、LoadSceneをRenderスレッド以外(Updateスレッド等)から呼んでも
        // 毎フレームの描画と安全に共存できる
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_UploadCommandAllocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_UploadCommandList;
        Microsoft::WRL::ComPtr<ID3D12Fence> m_UploadFence;
        uint64_t m_UploadFenceValue = 0;
        HANDLE m_UploadFenceEvent = nullptr;
        // 複数スレッドから同時にCreateBuffer/CreateTextureFromImageが呼ばれても
        // m_UploadCommandListへの記録が競合しないようにする
        std::mutex m_UploadMutex;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_RootSignature;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_ComputeRootSignature;

        std::unique_ptr<DX12DescriptorHeap> m_RtvHeap;
        std::unique_ptr<DX12DescriptorHeap> m_DsvHeap;
        std::unique_ptr<DX12DescriptorHeap> m_SrvCpuHeap;
        std::unique_ptr<DX12DescriptorHeap> m_SamplerCpuHeap;
        std::unique_ptr<DX12DescriptorHeap> m_ShaderVisibleSrvHeap;
        std::unique_ptr<DX12DescriptorHeap> m_ShaderVisibleSamplerHeap;

        std::unique_ptr<DX12CommandList> m_ImmediateCommandList;

        uint32_t m_NextSrvTableIndex = 0;
        // 1フレームあたりの払い出しブロック数の検証用(実際のリング位置には影響しない)。
        // kMaxSrvTableBlocksPerFrameを超えて払い出すと、まだGPUが読んでいる可能性のある
        // 前フレームのブロックを上書きしてしまうため、ResetCommandList()のたびに0に戻して検出する
        uint32_t m_SrvTableBlocksUsedThisFrame = 0;

        // コンピュートシェーダー用SRV+UAVテーブルのリング位置・使用量検証用カウンタ。
        // グラフィックス用(m_NextSrvTableIndex/m_SrvTableBlocksUsedThisFrame)とは別区画・別リングで管理する
        uint32_t m_NextComputeTableIndex = 0;
        uint32_t m_ComputeTableBlocksUsedThisFrame = 0;
    };
}
