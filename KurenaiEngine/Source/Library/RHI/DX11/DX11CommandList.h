#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "RHI/IRHICommandList.h"
#include "RHI/RHIBindingLimits.h"

namespace Kurenai::RHI
{
    class DX11CommandList : public IRHICommandList
    {
    public:
        explicit DX11CommandList(Microsoft::WRL::ComPtr<ID3D11DeviceContext> context);

        void SetRenderTarget(IRHISwapChain* swapChain) override;
        void SetRenderTargets(
            IRHITexture* const* targets, uint32_t count, IRHITexture* depthTexture, uint32_t depthArraySlice = 0) override;
        void ClearRenderTarget(const ClearColor& color) override;
        void ClearDepth(float depth) override;
        void SetPipelineState(IRHIPipelineState* pipelineState) override;
        void SetVertexBuffer(IRHIBuffer* buffer) override;
        void SetIndexBuffer(IRHIBuffer* buffer) override;
        void SetConstantBuffer(uint32_t slot, IRHIBuffer* buffer) override;
        void SetTexture(uint32_t slot, IRHITexture* texture) override;
        void SetTextureAllStages(uint32_t slot, IRHITexture* texture) override;
        void SetSamplerSet(IRHISamplerSet* samplerSet) override;
        void SetShaderResourceBuffer(uint32_t slot, IRHIBuffer* buffer) override;
        void SetVertexShaderResourceBuffer(uint32_t slot, IRHIBuffer* buffer) override;
        void UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t sizeInBytes) override;
        void Draw(uint32_t vertexCount, uint32_t startVertexLocation) override;
        void DrawIndexed(
            uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation,
            uint32_t instanceCount) override;
        void DispatchMesh(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) override;

        void SetComputePipelineState(IRHIPipelineState* pipelineState) override;
        void SetComputeConstantBuffer(uint32_t slot, IRHIBuffer* buffer) override;
        void SetComputeTexture(uint32_t slot, IRHITexture* texture) override;
        void SetComputeShaderResourceBuffer(uint32_t slot, IRHIBuffer* buffer) override;
        void SetComputeSamplerSet(IRHISamplerSet* samplerSet) override;
        void SetComputeUnorderedAccessTexture(uint32_t slot, IRHITexture* texture, uint32_t mipLevel = 0) override;
        void SetComputeUnorderedAccessTextureCubeFace(
            uint32_t slot, IRHITexture* texture, uint32_t face, uint32_t mipLevel = 0, uint32_t cubeIndex = 0) override;
        void SetComputeUnorderedAccessBuffer(uint32_t slot, IRHIBuffer* buffer) override;
        void SetComputeAccelerationStructure(uint32_t slot, IRHIAccelerationStructure* accelerationStructure) override;
        void Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) override;
        void DispatchMeshIndirect(
            IRHIBuffer* argsBuffer, uint32_t argsOffsetInBytes, uint32_t maxCommandCount,
            uint32_t countOffsetInBytes) override;

    protected:
        // 具象型でしか答えられない問い。「何を断るか」の判断は IRHICommandList が持つ
        bool IsIndirectArgsBuffer(const IRHIBuffer* buffer) const override;
        bool IsReadbackBuffer(const IRHIBuffer* buffer) const override;
        bool IsReadbackTexture(const IRHITexture* texture) const override;
        bool HasUnorderedAccessView(const IRHIBuffer* buffer) const override;

        // 検証済みの引数を受けて、実際にD3D11のコマンドを発行する
        void ApplyViewport(const Viewport& viewport) override;
        void ApplyScissorRect(const ScissorRect& rect) override;
        void DispatchIndirectImpl(IRHIBuffer* argsBuffer, uint32_t offsetInBytes) override;
        void ClearUnorderedAccessBufferUintImpl(IRHIBuffer* buffer, uint32_t value) override;
        void CopyBufferToReadbackImpl(IRHIBuffer* dst, IRHIBuffer* src, uint32_t sizeInBytes) override;
        void CopyTextureToReadbackImpl(
            IRHITexture* dst, IRHITexture* src, uint32_t mipLevel, uint32_t arraySlice) override;

    private:
        // Dispatch/DispatchIndirectの後始末。バインドしたUAVを全解除する
        void ReleaseComputeUavBindingsAfterDispatch();

        static constexpr uint32_t kMaxRenderTargets = 8;
        // 【スロット数の定義は RHI/RHIBindingLimits.h が唯一の出所】ここに並ぶ3本はその別名で、
        // 値を持たない。以前はDX11・DX12のコマンドリストとDX12のルートシグネチャの3か所へ
        // 生の数値を書き写し、「必ず一致させること」というコメントで手で同期させていた
        // (H3でt21を足したときに実際に踏んだ。DX11だけで確認していると気付けない)。
        // 各スロットの意味と内訳もあちらにある

        // SetComputeUnorderedAccessTexture/Bufferで使えるUAVスロット数(u0〜u4)
        static constexpr uint32_t kComputeUavSlotCount = RHIBindingLimits::kComputeUavSlotCount;
        // SetVertexShaderResourceBufferで使える頂点シェーダのSRVスロット数
        static constexpr uint32_t kVertexShaderSrvSlotCount = RHIBindingLimits::kVertexShaderSrvSlotCount;
        // SetTexture/SetShaderResourceBufferで使えるピクセルシェーダのSRVスロット数(t0〜t22)
        static constexpr uint32_t kTextureSlotCount = RHIBindingLimits::kTextureSlotCount;

        // ピクセルシェーダのSRVスロットに現在バインドされているビュー。
        // UAVバインド時に同一リソースのSRVを外すため(UnbindPixelSrvForResource)に持つ。
        // 生ポインタではなくComPtrで持つのは、テクスチャ/バッファが解像度変更等で作り直された際に
        // 解放済みのビューをGetResourceで触ってしまうのを防ぐため
        void UnbindPixelSrvForResource(ID3D11Resource* resource);
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_BoundPixelSrvs[kTextureSlotCount];

        // 指定リソースが出力(レンダーターゲット/深度)として張られていたら、出力をすべて外す。
        // D3D11は出力に張ったままのリソースをコピー元にできず、そのまま
        // CopySubresourceRegionを呼ぶとデバッグレイヤーが警告を出してコピーが無効になる。
        // UnbindPixelSrvForResourceの出力版で、リードバックのコピー直前に使う
        void UnbindRenderTargetsForResource(ID3D11Resource* resource);

        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;
        ID3D11RenderTargetView* m_CurrentRenderTargetViews[kMaxRenderTargets] = {};
        uint32_t m_CurrentRenderTargetCount = 0;
        ID3D11DepthStencilView* m_CurrentDepthStencilView = nullptr;

        // Dispatch後にUAVを明示的にアンバインドするための、直前のDispatchでバインドしたスロットのビットマスク。
        // DX11はUAVとSRVを同一リソースへ同時バインドできないため、バインドしっぱなしにすると
        // 次にそのリソースをSetTexture(SRV)で読もうとした際にドライバが自動でUAV側を外して警告を出す。
        //
        // これは「UAV→SRV」方向の対処で、逆の「SRV→UAV」方向(前フレームにPSがSRVで読んだリソースを
        // 次フレームのDispatchがUAVで書く。タイルライトカリングのライトグリッドが該当する)は
        // UnbindPixelSrvForResourceで対処している
        uint32_t m_BoundComputeUavSlotMask = 0;
    };
}
