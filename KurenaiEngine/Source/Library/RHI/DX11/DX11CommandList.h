#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "RHI/IRHICommandList.h"

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
        void SetViewport(const Viewport& viewport) override;
        void SetPipelineState(IRHIPipelineState* pipelineState) override;
        void SetVertexBuffer(IRHIBuffer* buffer) override;
        void SetIndexBuffer(IRHIBuffer* buffer) override;
        void SetConstantBuffer(uint32_t slot, IRHIBuffer* buffer) override;
        void SetTexture(uint32_t slot, IRHITexture* texture) override;
        void SetSamplerSet(IRHISamplerSet* samplerSet) override;
        void SetShaderResourceBuffer(uint32_t slot, IRHIBuffer* buffer) override;
        void UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t sizeInBytes) override;
        void Draw(uint32_t vertexCount, uint32_t startVertexLocation) override;
        void DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation) override;

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

    private:
        static constexpr uint32_t kMaxRenderTargets = 8;
        // SetComputeUnorderedAccessTexture/Bufferで使えるUAVスロット数(u0〜u3)
        static constexpr uint32_t kComputeUavSlotCount = 4;
        // SetTexture/SetShaderResourceBufferで使えるピクセルシェーダのSRVスロット数(t0〜t17)。
        // DX12側のDX12CommandList::kTextureSlotCountおよびDX12Device.cppの
        // ルートシグネチャのSRVレンジと同じ値にしておくこと(3か所)。
        //
        // この値が足りないと、反射プローブ(19章)が張るキューブマップ配列・影響範囲バッファ・
        // 距離キューブや、DDGI(22章)のアトラス2枚が下のm_BoundPixelSrvsの追跡から漏れる。
        // そうなるとUnbindPixelSrvForResourceがこれらを外せず、ベイクがUAVで書き込む際の
        // SRVアンバインドがドライバ任せ(警告付きの自動アンバインド)になってしまう
        // (実際に12のまま放置され、t12/t13が漏れていた)。
        //
        // 【現在の22の内訳】最も多く使うDeferredLighting.hlslがt0〜t21をちょうど使い切る:
        //   t0〜t7   G-Buffer一式(アルベド/直接光/マテリアル/深度/スカイボックス/AO/自発光/法線)
        //   t8,t9    グローバルIBL(放射照度・プリフィルタ済み鏡面)
        //   t10      BRDF LUT
        //   t11      空パラメータ(GPUSkyParameters、SkyIntegrate.hlslが書く構造化バッファ)
        //   t12〜t14 反射プローブ(鏡面専任。拡散はDDGIへ一本化した)
        //   t15,t16  DDGIのオクタヘドラルアトラス2枚
        //   t17      bent normalのG-Buffer(34章)
        //   t18,t19  雲の形状/ディテールの3Dノイズ
        //   t20      大気散乱のSkyView LUT
        //   t21      焼いた雲のウェザーマップ(H3。2D。レイマーチの1歩を8倍安くするためのもの)
        static constexpr uint32_t kTextureSlotCount = 22;

        // ピクセルシェーダのSRVスロットに現在バインドされているビュー。
        // UAVバインド時に同一リソースのSRVを外すため(UnbindPixelSrvForResource)に持つ。
        // 生ポインタではなくComPtrで持つのは、テクスチャ/バッファが解像度変更等で作り直された際に
        // 解放済みのビューをGetResourceで触ってしまうのを防ぐため
        void UnbindPixelSrvForResource(ID3D11Resource* resource);
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_BoundPixelSrvs[kTextureSlotCount];

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
