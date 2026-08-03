#pragma once

#include <cstdint>
#include <d3d12.h>

#include "RHI/IRHIBuffer.h"
#include "RHI/IRHICommandList.h"
#include "RHI/IRHIPipelineState.h"
#include "RHI/IRHISamplerSet.h"
#include "RHI/IRHISwapChain.h"
#include "RHI/IRHITexture.h"

namespace Kurenai::RHI
{
    class DX12Device;

    class DX12CommandList : public IRHICommandList
    {
    public:
        explicit DX12CommandList(DX12Device* device);

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

        DX12Device* m_Device;
        D3D12_CPU_DESCRIPTOR_HANDLE m_CurrentRenderTargetViews[kMaxRenderTargets]{};
        uint32_t m_CurrentRenderTargetCount = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE m_CurrentDepthStencilView{};
        bool m_HasDepthStencilView = false;

        // --- バインド状態のシャドウコピー -------------------------------------------------
        // DX11はイミディエイトコンテキストがステートフルで、PSSetShaderResources/PSSetConstantBuffers
        // で張ったバインドは上書きするまで(Drawやパイプラインステート切り替えをまたいで)残る。
        // DX12はSetGraphicsRootSignatureがルート引数をすべて無効化し、かつディスクリプタテーブルは
        // 描画ごとに新しいブロックを払い出す必要があるため、放っておくと寿命の意味がDX11と食い違う。
        // そこで「DX11のコンテキストが持っているのと同じ状態」をここにシャドウコピーとして保持し、
        // ルート引数が無効化されるたびに自動で張り直すことで、両バックエンドの挙動を一致させる。
        // (この方式はサンプラーテーブルで先に導入したもの。m_CurrentSamplerSetBase参照)

        // ルート定数バッファビュー(b0/b1)のGPU仮想アドレス。0は未設定を表す
        static constexpr uint32_t kConstantBufferSlotCount = 2;
        D3D12_GPU_VIRTUAL_ADDRESS m_CurrentRootCbv[kConstantBufferSlotCount]{};
        D3D12_GPU_VIRTUAL_ADDRESS m_CurrentComputeRootCbv[kConstantBufferSlotCount]{};

        // SRVスロット(t0〜t17)のシャドウ。SetTexture/SetShaderResourceBufferはCopyDescriptorsを
        // その場では呼ばず、コピー元ハンドルをここへ記録するだけにして、Draw直前の
        // FlushPendingSrvWrites()でまとめて1回のCopyDescriptorsに反映する(メッシュごとに
        // テクスチャの数だけCopyDescriptorsSimpleを呼ぶドライバ呼び出しコストの削減)。
        // 全スロットはコンストラクタでnullディスクリプタに初期化してあり、以降は必ず有効な
        // ディスクリプタを指す。そのため「どのスロットが設定済みか」を区別する必要がなく、
        // 未バインドのスロットを読むと0が返るというDX11と同じ挙動になる。
        // 反射プローブ(19章)がDeferredLighting.hlslでt11〜t14(イラディアンス配列・プリフィルタ配列・
        // 影響範囲バッファ・距離キューブ配列)を、DDGI(22章)がt15〜t16(イラディアンスアトラス・
        // 距離モーメントアトラス)を使い、さらに空パラメータの構造化バッファ・bent normalの
        // G-Buffer(34章)・雲の3Dノイズ2枚・大気散乱のSkyView LUTを使うため21スロット必要。
        // 内訳はDX11CommandList.hの同名の定数のコメントに1枚ずつ書いてある。
        // DX12Device.cpp側の同名の定数(ルートシグネチャのSRVレンジ幅)およびDX11CommandList
        // 側の同名の定数と必ず一致させること
        static constexpr uint32_t kTextureSlotCount = 21;
        D3D12_CPU_DESCRIPTOR_HANDLE m_PendingSrvHandles[kTextureSlotCount]{};
        // 現在の描画で使うSRVテーブルの割り当て済みブロック先頭インデックス
        uint32_t m_CurrentSrvTableBase = 0;
        void FlushPendingSrvWrites();
        // レンダーターゲット/深度としてバインドされるテクスチャのSRVがシャドウに残っていたら
        // nullディスクリプタへ戻す。D3D11ドライバが同一リソースのSRVとRTVの同時バインドを
        // 検出して自動で解除するのと同じ挙動を再現し、あわせて「RENDER_TARGET状態のリソースを
        // 指すディスクリプタがテーブルに残る」というDX12固有の危険も断つ
        void UnbindSrvSlotsBoundTo(IRHITexture* texture);

        // 直前にDraw/DrawIndexedへ実際に反映した(コピー済みの)テクスチャの組み合わせ。
        // 同じマテリアルを使う連続したメッシュではテクスチャの組み合わせが変わらないため、
        // 次の描画がこれと完全に一致する場合はSRVテーブルの新規割り当て・CopyDescriptors・
        // ルートテーブルの再バインドをまるごと省略し、既存のブロックをそのまま使い回す。
        // SetPipelineState()はSetGraphicsRootSignatureを呼び直すたびにルート引数を無効化するため、
        // その直後は必ずm_HasLastDrawをfalseにして使い回しを禁止する
        D3D12_CPU_DESCRIPTOR_HANDLE m_LastDrawSrvHandles[kTextureSlotCount]{};
        bool m_HasLastDraw = false;

        // 直近にSetSamplerSet/SetComputeSamplerSetで指定されたセットの、ヒープ上のブロック先頭。
        // SetPipelineState/SetComputePipelineStateはルートシグネチャを設定し直してルート引数を
        // 無効化するため、そのたびにここを見てサンプラーテーブルを張り直す。
        // 初期値はDX12Deviceが既定サンプラーで埋めたフォールバックのブロック(コンストラクタで設定)で、
        // 上位層がSetSamplerSetを呼び忘れても未初期化のディスクリプタを指さないようにしている。
        // SRVテーブルと違いブロックを毎回払い出さないのは、セットの中身が初期化後に不変だから
        // (=同じブロックをフレーム中いくつのパスが参照しても上書きが起きない。詳細はIRHISamplerSet.h)
        uint32_t m_CurrentSamplerSetBase = 0;
        uint32_t m_CurrentComputeSamplerSetBase = 0;

        // コンピュートシェーダー用SRV(t0〜)+UAV(u0〜)テーブル。グラフィックスのSRVテーブルと同様、
        // Set*の時点ではコピー元だけ溜めておき、Dispatch直前のFlushPendingComputeWrites()でまとめて
        // CopyDescriptors・ルートテーブルの再バインドを行う。
        // SRVはDX11と同じく上書きするまで維持され、UAVはDX11がDispatch直後に
        // CSSetUnorderedAccessViewsでnullを張るのに合わせてDispatch直後にnullへ戻す
        // SRVが17あるのはレイトレーシングのパス(RT反射)がTLAS・G-Buffer・シーンジオメトリに加えて
        // bent normal(t16、34章)を1回のディスパッチで同時に読むため。DX12Device.cpp側の同名の定数
        // (ルートシグネチャのSRVレンジ幅)と必ず一致させること
        static constexpr uint32_t kComputeSrvSlotCount = 17;
        static constexpr uint32_t kComputeUavSlotCount = 4;
        D3D12_CPU_DESCRIPTOR_HANDLE m_PendingComputeSrvHandles[kComputeSrvSlotCount]{};
        D3D12_CPU_DESCRIPTOR_HANDLE m_PendingComputeUavHandles[kComputeUavSlotCount]{};
        // 今回のDispatchでUAVとしてバインドされているリソース。Dispatch直後にUAVバリアを発行し、
        // 後続のDispatch/描画がこのDispatchの書き込み完了を確実に見えるようにするため保持する
        ID3D12Resource* m_BoundComputeUavResources[kComputeUavSlotCount]{};
        void FlushPendingComputeWrites();
    };
}
