#pragma once

#include <cmath> // MakeFullViewportScissorRectのfloor/ceil

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

    // シザー矩形(ピクセル単位。原点は描画先の左上、Y-down)。
    // Right/Bottomは半開区間の外側で、「ピクセルpが残る条件はLeft <= p < Right」という
    // D3D11/D3D12共通の意味論をそのまま使う
    struct ScissorRect
    {
        int32_t Left = 0;
        int32_t Top = 0;
        int32_t Right = 0;
        int32_t Bottom = 0;
    };

    // ビューポート全体を過不足なく覆うシザー矩形。DX11/DX12の両実装が共有する
    // (片方だけ丸め方を直すとバックエンド間で端の1pxがずれるため、必ずここへ寄せる)。
    // レターボックス表示ではTopLeftX/Widthが非整数になるので、左上はfloor・右下はceilで
    // 外側へ丸めて必ずビューポート全体を含める。外側へ丸めても描画範囲は広がらない
    // (正射影/透視投影のクリップ空間が既にビューポート境界で厳密にカリングしているため)
    inline ScissorRect MakeFullViewportScissorRect(const Viewport& viewport)
    {
        ScissorRect rect;
        rect.Left = static_cast<int32_t>(std::floor(viewport.TopLeftX));
        rect.Top = static_cast<int32_t>(std::floor(viewport.TopLeftY));
        rect.Right = static_cast<int32_t>(std::ceil(viewport.TopLeftX + viewport.Width));
        rect.Bottom = static_cast<int32_t>(std::ceil(viewport.TopLeftY + viewport.Height));
        return rect;
    }

    // rectをビューポート全体との積へクランプする。D3D12はレンダーターゲット外や負値を含む
    // シザー矩形を仕様違反として扱うため、RHIの側で必ず正規化してから渡す
    // (ビューポート外はそもそも描画されないので、クランプしても意味は変わらない)。
    // 積が空になった場合はLeft==RightまたはTop==Bottomの空矩形になり、以後の描画は出なくなる
    inline ScissorRect ClampScissorRectToViewport(const ScissorRect& rect, const Viewport& viewport)
    {
        const ScissorRect bounds = MakeFullViewportScissorRect(viewport);
        ScissorRect result;
        result.Left = rect.Left < bounds.Left ? bounds.Left : (rect.Left > bounds.Right ? bounds.Right : rect.Left);
        result.Top = rect.Top < bounds.Top ? bounds.Top : (rect.Top > bounds.Bottom ? bounds.Bottom : rect.Top);
        result.Right = rect.Right > bounds.Right ? bounds.Right : (rect.Right < result.Left ? result.Left : rect.Right);
        result.Bottom = rect.Bottom > bounds.Bottom ? bounds.Bottom : (rect.Bottom < result.Top ? result.Top : rect.Bottom);
        return result;
    }

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
        // 【重要】SetViewportはシザー矩形も「そのビューポート全体」へリセットする。
        // シザーを使わない呼び出し側から見た挙動を従来どおりに保つための仕様なので、
        // SetScissorRectは必ずSetViewportより後に呼ぶこと(先に呼ぶと上書きされる)
        virtual void SetViewport(const Viewport& viewport) = 0;
        // 以後の描画を矩形の内側だけに制限する。矩形はピクセル単位・描画先の左上原点(Y-down)で、
        // 現在のビューポート全体との積へクランプされる(ClampScissorRectToViewport)。
        // 積が空なら以後の描画は1ピクセルも出ない。
        //
        // DX11はこのAPIのためにDX11Device::CreatePipelineStateが全パイプラインステートの
        // ラスタライザをScissorEnable=TRUEで作っており、DX12はD3D12の仕様上シザーが
        // 常時有効なので、両バックエンドで挙動は完全に同一。
        // SetPipelineStateはシザー矩形をリセットしない(D3D11のラスタライザステートも
        // D3D12のPSOもシザー矩形自体は持たない)ため、パイプラインを切り替えても絞ったまま
        virtual void SetScissorRect(const ScissorRect& rect) = 0;
        // SetScissorRectで絞った範囲を、直近のSetViewportで設定したビューポート全体へ戻す
        virtual void ResetScissorRect() = 0;
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
        // SetTextureと同じスロット・同じバインドの寿命だが、**ピクセルシェーダー以外の
        // グラフィックスステージからも読める状態**にしてバインドする。
        //
        // 【なぜ別のAPIが要るのか】DX12のリソース状態はステージ単位で分かれており、
        // SetTextureはD3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCEへしか遷移させない。
        // 増幅シェーダー・メッシュシェーダーはピクセルシェーダーとは別のステージなので、
        // その状態のまま読むとデバッグレイヤーが警告を出し、実際に読める保証も無い。
        // こちらはPIXEL|NON_PIXELの両方を立てて遷移させる。
        //
        // 【DX11では違いが無い】DX11はステージごとにバインド空間が独立しており、
        // リソース状態という概念自体が無いためSetTextureと同じ実装でよい。
        // それでも同じ意味で呼べるよう両バックエンドに用意する。
        //
        // 用途はG-Bufferパスの増幅シェーダーがHi-Zを読むこと(GBufferMeshlet.hlsl)。
        // メッシュシェーダー用ルートシグネチャはSRVテーブルの可視性をALLにしてあるので、
        // ディスクリプタの張り方はSetTextureとまったく同じでよい(DX12Device::CreateMeshRootSignature)
        virtual void SetTextureAllStages(uint32_t slot, IRHITexture* texture) = 0;
        // このパスで使うサンプラーの組をまとめてバインドする(セットのi番目がレジスタs(i)になる)。
        // セットの中身は初期化時に決まっていて書き換わらないため、ここで切り替わるのは
        // 「どのセットを見るか」だけ(理由はIRHISamplerSet.h)。
        // SetTextureと同じく、上書きするまでバインドは維持される
        virtual void SetSamplerSet(IRHISamplerSet* samplerSet) = 0;
        // BufferUsage::StructuredReadOnlyで作成したバッファをStructuredBuffer<T>としてピクセルシェーダへ
        // バインドする。スロット空間・バインドの寿命はSetTextureと共通(t0〜t11)なので、
        // 同じ描画内でスロットが衝突しないよう呼び出し側で調整すること
        virtual void SetShaderResourceBuffer(uint32_t slot, IRHIBuffer* buffer) = 0;
        // BufferUsage::StructuredReadOnlyで作成したバッファをStructuredBuffer<T>として
        // 「頂点シェーダ」へバインドする。上のSetShaderResourceBufferがピクセルシェーダ専用なのに対し、
        // こちらは頂点段からしか見えない独立したレジスタ空間を使う
        // (コンピュート用にSetComputeShaderResourceBufferを別に用意しているのと同じ理由)。
        //
        // 【なぜ専用のAPIなのか】DX12のグラフィックス用ルートシグネチャでは、SetTexture/
        // SetShaderResourceBufferが使うSRVディスクリプタテーブルがD3D12_SHADER_VISIBILITY_PIXELで
        // 宣言されており、頂点シェーダからは1つも見えない。こちらは可視性をVERTEXに限定した
        // 別のルートパラメータ(ルートSRV)へ割り当てるため、同じレジスタ番号を使っても衝突しない。
        // DX11はステージごとにバインド空間が独立しているため元から衝突しない。
        //
        // 用途はドローンショーのように「1回のDrawで大量のインスタンスを頂点シェーダ側で展開する」
        // 描画で、頂点バッファを使わずSV_VertexIDからこのバッファを引く(Shaders/3D/DroneShow.hlsl)。
        // バインドの寿命はSetTextureと同じで、上書きするまで維持される
        virtual void SetVertexShaderResourceBuffer(uint32_t slot, IRHIBuffer* buffer) = 0;
        virtual void UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t sizeInBytes) = 0;
        virtual void Draw(uint32_t vertexCount, uint32_t startVertexLocation) = 0;
        // instanceCount: 同じジオメトリを何体ぶん描くか(ハードウェアインスタンシング)。
        // 既定の1なら従来どおりの単発描画で、発行されるコマンドも同一。
        //
        // 【インスタンスごとの値はどこから来るのか】このRHIは頂点ストリームによるインスタンシング
        // (InputSlotClass = PER_INSTANCE_DATA)を持たない ―― InputElementDescにInputSlotも
        // InputSlotClassも無く、SetVertexBufferもスロットを取らないため、頂点バッファは常に1本きり。
        // そのため、インスタンスごとに変わる値(ワールド行列など)は
        // SetVertexShaderResourceBufferで頂点シェーダーへ渡したStructuredBufferを
        // SV_InstanceIDで引く形で受け取る(Shaders/3D/ObjectConstants.hlsliのFetchModelInstance)。
        //
        // 【SV_InstanceIDは0から始まる】D3D11/D3D12ともにStartInstanceLocationは
        // SV_InstanceIDへ加算されない。バッファ内の開始位置は定数バッファ側の値
        // (ObjectConstants::InstanceBase)で渡すこと。
        //
        // instanceCountが0のときはログを出して何も描かない(両バックエンド共通)
        virtual void DrawIndexed(
            uint32_t indexCount, uint32_t startIndexLocation, int32_t baseVertexLocation,
            uint32_t instanceCount = 1) = 0;

        // メッシュシェーダーパイプライン(CreateMeshPipelineStateで作ったステートをSetPipelineStateで
        // 設定した状態)での描画。増幅シェーダーがある場合はそちらが、無い場合はメッシュシェーダーが
        // 直接この数だけスレッドグループとして起動される。
        //
        // SetVertexBuffer/SetIndexBufferは呼ばない(呼んでも無視される)。ジオメトリは
        // シェーダー自身がbindless経由でバッファから読む。定数バッファ・テクスチャ・
        // サンプラーのバインドは通常の描画とまったく同じAPI・同じ寿命で使える。
        //
        // DX11およびメッシュシェーダー非対応のDX12環境では、ログを出して何もしない
        // (呼び出し側はIRHIDevice::SupportsMeshShader()で事前に確認すること)
        virtual void DispatchMesh(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) = 0;

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
        // BufferUsage::IndirectArgsで作ったバッファのoffsetInBytesの位置にあるuint3を
        // スレッドグループ数として解釈してディスパッチする。offsetInBytesは4の倍数であること。
        // 「発行するグループ数がGPU上でしか分からない」場合に使う(自前ラスタライザの
        // 巨大三角形パスがこれにあたる。個数は直前のディスパッチが数え上げる)。
        //
        // 引数バッファ以外のUsageを渡すとログを出して何もしない。
        // Dispatchと同じく、この呼び出しの直後にUAVスロットは全解除される
        virtual void DispatchIndirect(IRHIBuffer* argsBuffer, uint32_t offsetInBytes) = 0;

        // 増幅シェーダーの間接起動(DX12のExecuteIndirect)。1件ぶんの引数は24バイトで、
        //   +0  : このドローが使う定数バッファ(b1)のGPU仮想アドレス(64bit)
        //   +8  : DispatchMeshのスレッドグループ数X/Y/Z(uint3)
        // という並び。末尾4バイトは詰め物で、アドレスを8バイト境界に載せるために置いてある。
        //
        // 【b1を引数に含める理由】1回のExecuteIndirectで複数のモデルを描くため、
        // 「どのモデルか」をドローごとに渡す手段が要る。ルート定数で番号だけ渡して
        // 構造化バッファから引く手もあるが、それだと増幅/メッシュ/ピクセルの3本すべてで
        // 定数の読み方を書き換えることになる。定数バッファのアドレス自体を差し替えれば
        // シェーダー側は1行も変わらない。
        //
        // countOffsetInBytesの位置には実際に発行する件数(uint32)が入っていること。
        // maxCommandCountはその上限(GPUが書いた件数がこれを超えることは無いと保証する値)。
        // どちらのオフセットも4の倍数であること。
        //
        // 【DX11には無い】メッシュシェーダー自体が無いため、呼ぶとログを出して何もしない。
        // 呼び出し側は IRHIDevice::SupportsIndirectDispatchMesh() で分岐すること
        // 【Shaders/3D/ModelCull.hlsl の KURENAI_INDIRECT_ARG_STRIDE と一致させること】
        // あちらがこの並びで引数を書き込む
        static constexpr uint32_t kDispatchMeshIndirectArgStride = 24;
        virtual void DispatchMeshIndirect(
            IRHIBuffer* argsBuffer, uint32_t argsOffsetInBytes, uint32_t maxCommandCount,
            uint32_t countOffsetInBytes) = 0;

        // UAVを持つバッファ全体を、指定した符号なし整数値で埋める。散布書き込みされる
        // バッファ(自前ラスタライザのvisibility bufferや間接ディスパッチ引数)を毎フレーム
        // 初期値へ戻す用途向け。
        //
        // 【この呼び出しはバインド状態を変えない】SetComputeUnorderedAccess*で張ったスロットには
        // 影響しない(内部で一時的なディスクリプタを使うため)。
        // UAVを持たないバッファを渡すとログを出して何もしない
        virtual void ClearUnorderedAccessBufferUint(IRHIBuffer* buffer, uint32_t value) = 0;

        // GPU上のバッファの内容を、BufferUsage::Readbackのバッファへ写す(GPUのコピーコマンド)。
        // 実際にCPUから読むのは IRHIBuffer::ReadbackData で、**数フレーム後に行うこと**。
        //
        // 【この呼び出しはコマンドを積むだけ】ここでGPUの完了を待ってはいけない。
        // 待つとフレームが直列化し、計測のために計測対象を壊すことになる。
        // 呼び出し側は受け皿をリング状に複数本持ち、十分に古いものを読む。
        //
        // srcはコピー元として読める状態へ遷移させる(DX12)。dstがBufferUsage::Readbackでない、
        // サイズが足りない、いずれかがnullptrならログを出して何もしない
        virtual void CopyBufferToReadback(IRHIBuffer* dst, IRHIBuffer* src, uint32_t sizeInBytes) = 0;
    };
}
