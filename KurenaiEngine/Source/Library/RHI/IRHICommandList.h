#pragma once

#include <cstddef>
#include <cstdint>

#include "KurenaiTypes.h"

#include "IRHIAccelerationStructure.h"
#include "IRHIBuffer.h"
#include "IRHIPipelineState.h"
#include "IRHISamplerSet.h"
#include "IRHISwapChain.h"
#include "IRHITexture.h"

namespace Kurenai::RHI
{
    struct Viewport
    {
        float TopLeftX = 0.0f;
        float TopLeftY = 0.0f;
        float Width = 0.0f;
        float Height = 0.0f;
        float MinDepth = 0.0f;
        float MaxDepth = 1.0f;
    };

    struct ClearColor
    {
        float R = 0.0f;
        float G = 0.0f;
        float B = 0.0f;
        float A = 1.0f;
    };

    class KURENAI_LIB_API IRHICommandList
    {
    public:
        virtual ~IRHICommandList() = default;

        virtual void SetRenderTarget(IRHISwapChain* swapChain) = 0;
        // depthArraySlice: depthTextureがCreateDepthTextureArrayで作られたテクスチャ配列の場合に、
        // 書き込み先の配列スライスを選ぶ(SetComputeUnorderedAccessTextureのmipLevelと同じ考え方の省略可能引数)。
        // 通常のCreateDepthTextureは1枚しか持たないため既定値の0でよい。
        // ClearDepthはこの呼び出しで確定した現在のDSV(=選んだスライス)に対して働くので、
        // スライスごとのクリアは呼び出し側で追加の指定をせずそのまま成立する
        virtual void SetRenderTargets(
            IRHITexture* const* targets, uint32_t count, IRHITexture* depthTexture, uint32_t depthArraySlice = 0) = 0;
        virtual void ClearRenderTarget(const ClearColor& color) = 0;
        virtual void ClearDepth(float depth) = 0;
        virtual void SetViewport(const Viewport& viewport) = 0;
        virtual void SetPipelineState(IRHIPipelineState* pipelineState) = 0;
        virtual void SetVertexBuffer(IRHIBuffer* buffer) = 0;
        virtual void SetIndexBuffer(IRHIBuffer* buffer) = 0;
        // 定数バッファをb(slot)へバインドする。有効なスロットはb0〜b1(DX12のルートシグネチャの
        // レイアウトに合わせた上限。範囲外はログを出してスキップされる)。
        //
        // 【呼び出し順の注意】同じバッファをUpdateBufferしてからバインドする場合は、必ず
        // UpdateBuffer → SetConstantBuffer の順で呼ぶこと。DX12の定数バッファは1フレームぶんの
        // コマンドをまとめて記録する都合でリング状に複数コピーを持ち、UpdateBufferが書き込み先を
        // 次のスロットへ進めるため、先にバインドすると1つ前のスロット(=更新前の内容)を指したままになる。
        // DX11はイミディエイト実行のためこの制約はないが、両バックエンドで同じ順序で書くこと
        virtual void SetConstantBuffer(uint32_t slot, IRHIBuffer* buffer) = 0;
        // テクスチャをt(slot)へバインドする。有効なスロットはt0〜t11(範囲外はログを出してスキップされる)。
        //
        // 【バインドの寿命】一度バインドしたスロットは、同じスロットを別のリソースで上書きするか、
        // そのテクスチャがレンダーターゲット/深度ターゲットとしてバインドされるまで維持される。
        // Draw/DrawIndexedをまたいでも、SetPipelineStateでパイプラインステートを切り替えても残る。
        // 一度もバインドしていないスロットを読むと0が返る。この寿命はDX11/DX12で完全に同一で、
        // DX12側はDX12CommandListがバインド状態をシャドウコピーとして保持することで実現している
        virtual void SetTexture(uint32_t slot, IRHITexture* texture) = 0;
        // このパスで使うサンプラーの組をまとめてバインドする(セットのi番目がレジスタs(i)になる)。
        // セットの中身は初期化時に決まっていて書き換わらないため、ここで切り替わるのは
        // 「どのセットを見るか」だけ(理由はIRHISamplerSet.h)。
        // SetTextureと同じく、上書きするまでバインドは維持される
        virtual void SetSamplerSet(IRHISamplerSet* samplerSet) = 0;
        // BufferUsage::StructuredReadOnlyで作成したバッファをStructuredBuffer<T>としてピクセルシェーダへ
        // バインドする。スロット空間・バインドの寿命はSetTextureと共通(t0〜t11)なので、
        // 同じ描画内でスロットが衝突しないよう呼び出し側で調整すること
        virtual void SetShaderResourceBuffer(uint32_t slot, IRHIBuffer* buffer) = 0;
        virtual void UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t sizeInBytes) = 0;
        virtual void Draw(uint32_t vertexCount, uint32_t startVertexLocation) = 0;
        virtual void DrawIndexed(uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation) = 0;

        // コンピュートシェーダーの発行。グラフィックス用のSetPipelineState/SetConstantBuffer/SetTextureとは
        // レジスタ空間(DX12ではルートシグネチャ)が独立しているため専用のAPIを用意する
        virtual void SetComputePipelineState(IRHIPipelineState* pipelineState) = 0;
        // 有効なスロットはb0〜b1。UpdateBufferとの呼び出し順の注意もSetConstantBufferと同じ
        virtual void SetComputeConstantBuffer(uint32_t slot, IRHIBuffer* buffer) = 0;
        // 有効なスロットはt0〜t3。SetTextureと同じく、上書きするまでバインドは維持される
        // (SetComputeUnorderedAccessTexture系のUAVだけはDispatch直後に解除される。下記参照)
        virtual void SetComputeTexture(uint32_t slot, IRHITexture* texture) = 0;
        // 構造化バッファをStructuredBuffer<T>としてコンピュートシェーダーへバインドする。
        // スロット空間はSetComputeTextureと共有(t0〜t3)なので、同じディスパッチ内で衝突しないよう
        // 呼び出し側で調整すること。タイルライトカリングがライトリスト(StructuredReadOnly)を
        // 読むのに使う
        virtual void SetComputeShaderResourceBuffer(uint32_t slot, IRHIBuffer* buffer) = 0;
        // TextureCube(スカイボックス)をコンピュートシェーダーからSampleLevelで読む(IBLの畳み込み等)場合に
        // 必要。DX11はステージごとに独立したサンプラースロットを持つため、グラフィックス側のSetSamplerSetとは
        // 別に明示的なバインドが要る(DX12はグラフィックス・コンピュートで同じサンプラーヒープを共有するため
        // 呼び出し不要でも動作するが、DX11との整合のため両バックエンドで同じ呼び出し規約にする)
        virtual void SetComputeSamplerSet(IRHISamplerSet* samplerSet) = 0;
        // RWTexture2D/RWStructuredBufferとしてバインドする(書き込み可能)。有効なスロットはu0〜u3。
        // mipLevelはCreateHiZTextureで作成したミップチェーンテクスチャの特定ミップを指定する場合に使う
        // (通常のCreateUAVTextureは常に1ミップのみのため既定値の0で問題ない)。
        //
        // 【バインドの寿命】SRVと違い、UAVはDispatchの直後に全スロットが自動で解除される
        // (DX11がリソースをSRVとUAVに同時バインドできない制約への対処としてそうしており、
        // DX12もそれに合わせている)。そのためDispatchごとに毎回バインドし直すこと
        virtual void SetComputeUnorderedAccessTexture(uint32_t slot, IRHITexture* texture, uint32_t mipLevel = 0) = 0;
        // CreateUAVTextureCube/CreateMippedUAVTextureCubeで作成したキューブマップの、指定した面・
        // ミップ1枚だけをRWTexture2DArray(要素数1のビュー)としてバインドする。キューブマップの
        // 6面は同一リソース内の配列スライスとして実装されており(D3D11/D3D12ともに)、コンピュート
        // シェーダー側は面ごとに1回ずつディスパッチする必要がある(HLSLがリソースを動的に
        // スライス選択できないため。カスケードシャドウマップのテクスチャ分岐と同種の制約)。
        //
        // cubeIndexはCreateMippedUAVTextureCubeArrayで作成したキューブマップ配列の何枚目に
        // 書き込むかを指定する(単一キューブのテクスチャは常に既定値の0でよい)
        virtual void SetComputeUnorderedAccessTextureCubeFace(
            uint32_t slot, IRHITexture* texture, uint32_t face, uint32_t mipLevel = 0, uint32_t cubeIndex = 0) = 0;
        virtual void SetComputeUnorderedAccessBuffer(uint32_t slot, IRHIBuffer* buffer) = 0;
        // TLASをRaytracingAccelerationStructureとしてt(slot)へバインドする。
        // スロット空間・バインドの寿命はSetComputeTextureと共通なので、同じディスパッチ内で
        // 衝突しないよう呼び出し側で調整すること。
        //
        // 渡せるのはIRHIDevice::CreateTopLevelASで作ったTLASのみ(BLASはSRVを持たないため、
        // 渡すとログを出して無視される)。DX11はレイトレーシング非対応のため、
        // 呼ぶとログを出して何もしない
        virtual void SetComputeAccelerationStructure(uint32_t slot, IRHIAccelerationStructure* accelerationStructure) = 0;
        virtual void Dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) = 0;
    };
}
