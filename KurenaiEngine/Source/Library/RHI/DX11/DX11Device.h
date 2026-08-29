#pragma once

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "Assets/ShaderLoader.h"
#include "RHI/IRHIDevice.h"

namespace Kurenai::RHI
{
    class DX11CommandList;

    class DX11Device : public IRHIDevice
    {
    public:
        DX11Device();
        ~DX11Device() override;

        void Initialize();

        std::unique_ptr<IRHISwapChain> CreateSwapChain(void* windowHandle, uint32_t width, uint32_t height) override;
        std::unique_ptr<IRHIBuffer> CreateBuffer(const BufferDesc& desc) override;
        std::unique_ptr<IRHIShader> CreateShader(const ShaderDesc& desc) override;
        void ReleaseShaderPackages() override { m_ShaderPackages.Clear(); }
        std::unique_ptr<IRHIPipelineState> CreatePipelineState(const PipelineStateDesc& desc) override;
        std::unique_ptr<IRHIPipelineState> CreateComputePipelineState(const ComputePipelineStateDesc& desc) override;
        std::unique_ptr<IRHITexture> CreateTextureFromFile(const std::wstring& filePath, bool sRGB) override;
        std::unique_ptr<IRHITexture> CreateTextureFromImage(const TextureImage& image) override;
        std::unique_ptr<IRHIPendingTextureContents> PrepareTextureContents(
            IRHITexture* target, const TextureImage& image) override;
        bool CommitTextureContents(IRHIPendingTextureContents* pending) override;
        bool GetVideoMemoryUsage(uint64_t& outUsedBytes, uint64_t& outBudgetBytes) const override;
        // D3D11.2のTiled Resourcesは使わない(IRHIDevice::GetTiledResourcesTierのコメント参照)
        uint32_t GetTiledResourcesTier() const override { return 0; }
        // DX11はタイルリソースを使わないため常にnullptr(呼ばれないのが正常)
        std::unique_ptr<IRHIPendingTextureContents> PrepareTiledTextureResidency(
            IRHITexture* target, const TiledTextureDesc& desc, const TextureImage& image, uint32_t firstMip) override;
        void GetTilePoolUsage(uint64_t& outReservedBytes, uint64_t& outUsedBytes) const override
        {
            outReservedBytes = 0;
            outUsedBytes = 0;
        }
        std::unique_ptr<IRHITexture> CreateSolidColorTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a) override;
        std::unique_ptr<IRHITexture> CreateTextureFromMemory(uint32_t width, uint32_t height, const void* pixelsRGBA8) override;
        std::unique_ptr<IRHITexture> CreateRenderTexture(uint32_t width, uint32_t height, Format format) override;
        std::unique_ptr<IRHITexture> CreateUAVTexture(uint32_t width, uint32_t height, Format format) override;
        std::unique_ptr<IRHITexture> CreateUAVTexture3D(
            uint32_t width, uint32_t height, uint32_t depth, Format format) override;
        std::unique_ptr<IRHITexture> CreateHiZTexture(uint32_t width, uint32_t height, uint32_t mipLevels) override;
        std::unique_ptr<IRHITexture> CreateMippedUAVTexture(uint32_t width, uint32_t height, Format format, uint32_t mipLevels) override;
        std::unique_ptr<IRHITexture> CreateUAVTextureCube(uint32_t size, Format format) override;
        std::unique_ptr<IRHITexture> CreateMippedUAVTextureCube(uint32_t size, Format format, uint32_t mipLevels) override;
        std::unique_ptr<IRHITexture> CreateMippedUAVTextureCubeArray(
            uint32_t size, Format format, uint32_t mipLevels, uint32_t cubeCount) override;
        std::unique_ptr<IRHITexture> CreateDepthTexture(uint32_t width, uint32_t height, float clearDepth = 1.0f) override;
        std::unique_ptr<IRHITexture> CreateDepthTextureArray(
            uint32_t width, uint32_t height, uint32_t arraySize, float clearDepth = 1.0f) override;
        std::unique_ptr<IRHISamplerSet> CreateSamplerSet(const SamplerDesc* descs, uint32_t count) override;
        IRHICommandList* GetImmediateCommandList() override;

        std::unique_ptr<IRHIImGuiBackend> CreateImGuiBackend(void* windowHandle) override;
        std::unique_ptr<IRHIGPUProfiler> CreateGPUProfiler() override;
        // DX11はDX12のようなフレームパイプライン化(フェンスによる多重バッファリング)を行っていないが、
        // 代わりにDX11SwapChain::Present()がブロッキング呼び出しの実測時間をここへ報告する
        // (vsync有効時、GPUの描画完了待ち+次のvblankまでの待ちがこの呼び出しに現れるため)
        float GetLastFrameGPUWaitTimeMs() const override { return m_LastFrameGPUWaitTimeMs; }

        // DX11SwapChain::Present()から、実測したPresent呼び出し時間を報告してもらうためのAPI
        // (IRHIDeviceの公開インタフェースではなく、DX11実装内部でのみ使う)
        void SetLastFrameGPUWaitTimeMs(float ms) { m_LastFrameGPUWaitTimeMs = ms; }

        // DX11はDX12のような多重バッファリングを行わず、ID3D11DeviceContextがリソースの
        // 使用状況を暗黙に追跡してくれるため、DX12ほど厳密なフェンス待ちは不要だが、
        // Flushで発行済みコマンドをGPUへ送り切ってから完了を待つことで、LoadScene等が
        // 直前まで参照されていたリソースを破棄する前に安全マージンを確保する
        void WaitForGPUIdle() override;

        // DX11にはレイトレーシングAPIそのものが存在しない(DXRはD3D12の機能)。
        // 上位層はSupportsRaytracing()を見て従来のスクリーンスペース手法へフォールバックする設計のため、
        // 下の2つは呼ばれないのが正常。呼ばれた場合はエラーログを残してnullptrを返す
        bool SupportsRaytracing() const override { return false; }
        // DX11はディスクリプタテーブルを持たないため、1フレームの描画回数に上限が無い
        uint32_t GetMaxDrawsPerFrame() const override { return UINT32_MAX; }
        std::unique_ptr<IRHIAccelerationStructure> CreateBottomLevelAS(const BottomLevelASDesc& desc) override;
        std::unique_ptr<IRHIAccelerationStructure> CreateTopLevelAS(const TopLevelASDesc& desc) override;

        // bindless(HLSLのResourceDescriptorHeap)もメッシュシェーダーも、レイトレーシングと同じく
        // D3D12だけの機能でDX11には存在しない。上位層はSupports*()で分岐する設計のため、
        // 下の登録・作成関数は呼ばれないのが正常
        bool SupportsBindless() const override { return false; }
        uint32_t RegisterBindless(IRHITexture* texture) override;
        uint32_t RegisterBindless(IRHIBuffer* buffer) override;
        uint32_t RegisterBindlessUAV(IRHIBuffer* buffer) override;
        bool SupportsMeshShader() const override { return false; }
        std::unique_ptr<IRHIPipelineState> CreateMeshPipelineState(const MeshPipelineStateDesc& desc) override;
        // コンピュートシェーダーによる自前ラスタライザもD3D12専用。DX11のコンピュートシェーダーは
        // cs_5_0固定で、必要な64bitアトミック(SM 6.6)もbindlessも原理的に持てない
        bool SupportsSoftwareRaster() const override { return false; }

    private:
        // CreateMippedUAVTextureCube(単一キューブ、SRVはTextureCube)と
        // CreateMippedUAVTextureCubeArray(配列、SRVはTextureCubeArray)の共通実装。
        // 両者はSRVの次元とキューブ枚数以外まったく同じ手順のため1箇所にまとめている
        std::unique_ptr<IRHITexture> CreateCubeTextureInternal(
            uint32_t size, Format format, uint32_t mipLevels, uint32_t cubeCount, bool asArray);

        // CreateShaderが読む.kshaderのキャッシュ。1つのパッケージは複数のエントリから
        // 引かれる(GBuffer.kshaderはVSMain/PSMain/PSMainCutout)ため、開き直しを避ける。
        // ReleaseShaderPackages()で明示的に捨てる
        Assets::ShaderPackageCache m_ShaderPackages;

        Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
        // VRAM使用量(QueryVideoMemoryInfo)を引くためのアダプタ。Initializeで一度だけ取る
        Microsoft::WRL::ComPtr<IDXGIAdapter3> m_Adapter;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;
        Microsoft::WRL::ComPtr<IDXGIFactory2> m_Factory;
        std::unique_ptr<DX11CommandList> m_ImmediateCommandList;
        float m_LastFrameGPUWaitTimeMs = 0.0f;
    };
}
