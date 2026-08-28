#pragma once

#include <cstdint>
#include <d3d12.h>
#include <d3d12sdklayers.h> // ID3D12InfoQueue(デバッグレイヤーのメッセージ引き取り用)
#include <dxgi1_4.h>
#include <memory>
#include <mutex>
#include <vector>
#include <wrl/client.h>

#include "Assets/ShaderLoader.h"
#include "DX12BindlessTable.h"
#include "DX12DescriptorHeap.h"
#include "DX12TilePool.h"
#include "RHI/IRHIDevice.h"

namespace DirectX
{
    struct TexMetadata;
    class ScratchImage;
}

namespace Kurenai::RHI
{
    class DX12CommandList;
    struct DX12TiledTextureState;

    class DX12Device : public IRHIDevice
    {
    public:
        DX12Device();
        ~DX12Device() override;

        void Initialize();

        // CPUがGPUの完了を待たずに次フレームの記録を始められるようにするための多重バッファリング数。
        // スワップチェインのバッファ数(DX12SwapChain::kBufferCount)と合わせて2にしておく。
        // DX12Bufferがリングの1フレームあたり書き込み上限を求めるのにも使うためpublicに置く
        static constexpr uint32_t kFrameCount = 2;

        std::unique_ptr<IRHISwapChain> CreateSwapChain(void* windowHandle, uint32_t width, uint32_t height) override;
        std::unique_ptr<IRHIBuffer> CreateBuffer(const BufferDesc& desc) override;
        std::unique_ptr<IRHIShader> CreateShader(const ShaderDesc& desc) override;
        void ReleaseShaderPackages() override { m_ShaderPackages.Clear(); }
        std::unique_ptr<IRHIPipelineState> CreatePipelineState(const PipelineStateDesc& desc) override;
        std::unique_ptr<IRHIPipelineState> CreateComputePipelineState(const ComputePipelineStateDesc& desc) override;
        std::unique_ptr<IRHIPipelineState> CreateMeshPipelineState(const MeshPipelineStateDesc& desc) override;
        std::unique_ptr<IRHITexture> CreateTextureFromFile(const std::wstring& filePath, bool sRGB) override;
        std::unique_ptr<IRHITexture> CreateTextureFromImage(const TextureImage& image) override;
        std::unique_ptr<IRHIPendingTextureContents> PrepareTextureContents(
            IRHITexture* target, const TextureImage& image) override;
        bool CommitTextureContents(IRHIPendingTextureContents* pending) override;
        bool GetVideoMemoryUsage(uint64_t& outUsedBytes, uint64_t& outBudgetBytes) const override;
        uint32_t GetTiledResourcesTier() const override { return m_TiledResourcesTier; }
        std::unique_ptr<IRHIPendingTextureContents> PrepareTiledTextureResidency(
            IRHITexture* target, const TiledTextureDesc& desc, const TextureImage& image, uint32_t firstMip) override;
        void GetTilePoolUsage(uint64_t& outReservedBytes, uint64_t& outUsedBytes) const override;
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
        float GetLastFrameGPUWaitTimeMs() const override { return m_LastFrameGPUWaitTimeMs; }

        // DX12実装内部(DX12SwapChain/DX12Texture/DX12Sampler/DX12CommandList)から利用するアクセサ
        ID3D12Device* GetDevice() const { return m_Device.Get(); }
        ID3D12CommandQueue* GetCommandQueue() const { return m_CommandQueue.Get(); }
        ID3D12GraphicsCommandList* GetCommandList() const { return m_CommandList.Get(); }
        ID3D12RootSignature* GetRootSignature() const { return m_RootSignature.Get(); }
        ID3D12RootSignature* GetComputeRootSignature() const { return m_ComputeRootSignature.Get(); }
        DX12DescriptorHeap* GetRtvHeap() const { return m_RtvHeap.get(); }
        DX12DescriptorHeap* GetDsvHeap() const { return m_DsvHeap.get(); }
        // 非シェーダー可視のCBV_SRV_UAVヒープは、確保するスレッドで2本に分けてある。
        // DX12DescriptorHeapはロックを持たないため、1本を複数スレッドから確保・解放すると
        // フリーリストが壊れる。スレッドごとに別のヒープを使うことで、ロックなしのまま安全にする
        // (詳細はdocs/Architecture.html 23章)。
        //
        // アセット由来(モデル・テクスチャ・シーンジオメトリ)のリソース用。シーン読み込み専用の
        // Loaderスレッドだけが確保・解放する(初期化時を除く)
        DX12DescriptorHeap* GetAssetSrvCpuHeap() const { return m_AssetSrvCpuHeap.get(); }
        // レンダーターゲット・コンピュート用中間バッファなど、描画側のリソース用。
        // Renderスレッドだけが確保・解放する(初期化時を除く)
        DX12DescriptorHeap* GetRenderSrvCpuHeap() const { return m_RenderSrvCpuHeap.get(); }
        DX12DescriptorHeap* GetShaderVisibleSrvHeap() const { return m_ShaderVisibleSrvHeap.get(); }
        DX12DescriptorHeap* GetShaderVisibleSamplerHeap() const { return m_ShaderVisibleSamplerHeap.get(); }
        // 上位層が一度もSetSamplerSetを呼ばないままDrawした場合に使う、既定サンプラーで埋めたブロックの先頭。
        // ルートディスクリプタテーブルが未初期化のディスクリプタを指さないようにするための保険
        uint32_t GetFallbackSamplerSetBase() const { return m_FallbackSamplerSetBase; }

        // 一度もバインドされていないSRV/UAVスロットを埋めるためのnullディスクリプタ。
        // DX11では未バインドのスロットを読むと0が返るが、DX12は描画のたびに払い出す
        // ディスクリプタテーブルのブロックが未初期化(リングの前世代の使い回し)のままになるため、
        // そのままではシェーダが破棄済みリソースのディスクリプタを読みうる。
        // DX12CommandListはシャドウ配列の全スロットをこれで初期化しておくことで、
        // 「未バインドのスロットは0を返す」というDX11と同じ挙動を構造的に保証する
        D3D12_CPU_DESCRIPTOR_HANDLE GetNullSrvCpuHandle() const { return m_RenderSrvCpuHeap->GetCpuHandle(m_NullSrvIndex); }
        D3D12_CPU_DESCRIPTOR_HANDLE GetNullUavCpuHandle() const { return m_RenderSrvCpuHeap->GetCpuHandle(m_NullUavIndex); }

        // フレームごとに1ずつ増える通し番号。DX12Bufferがリングへの書き込み回数を
        // 「同一フレーム内で何回目か」として数えるために参照する(ResetCommandList()で進む)
        uint64_t GetFrameStamp() const { return m_FrameStamp; }

        // 1フレーム分のコマンドをすべて記録してから1回だけExecuteCommandListsする設計のため、
        // CopyDescriptorsSimpleによるディスクリプタ書き込みはGPU実行前にすべて完了してしまう。
        // そのため同じヒープスロットを毎回使い回すと、GPUが実際にDrawを処理する時点では
        // そのフレーム最後のSetTexture呼び出しの内容にすべて上書きされてしまう。
        // これを避けるため、t0〜t14の連続したkTextureSlotCount個のブロックを描画のたびに新規に払い出す。
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
        // 開き直す。CPUはGPUの完了を待たずに次フレームの記録を始められるため、
        // CPU/GPUがオーバーラップして動作する(Present直後に毎回完全同期すると直列になる)
        void AdvanceToNextFrame();
        // フェンスでGPUの完全なアイドルを待つ(全フレームスロットの実行完了を保証する)。
        // リサイズやシャットダウンなど、パイプライン化の恩恵が不要な箇所でのみ使う
        void WaitForGPUIdle() override;

        bool SupportsRaytracing() const override { return m_SupportsRaytracing; }
        // 実装はDX12Device.cpp(上限の定数kMaxSrvTableBlocksPerFrameが匿名名前空間にあるため)
        uint32_t GetMaxDrawsPerFrame() const override;
        std::unique_ptr<IRHIAccelerationStructure> CreateBottomLevelAS(const BottomLevelASDesc& desc) override;
        std::unique_ptr<IRHIAccelerationStructure> CreateTopLevelAS(const TopLevelASDesc& desc) override;

        bool SupportsBindless() const override { return m_SupportsBindless; }
        uint32_t RegisterBindless(IRHITexture* texture) override;
        uint32_t RegisterBindless(IRHIBuffer* buffer) override;
        uint32_t RegisterBindlessUAV(IRHIBuffer* buffer) override;
        bool SupportsMeshShader() const override { return m_SupportsMeshShader; }
        bool SupportsSoftwareRaster() const override { return m_SupportsSoftwareRaster; }

        // DX12Texture/DX12Bufferがデストラクタでbindless番号を返却するのに使う。
        // bindless非対応の場合はnullptrを返す
        DX12BindlessTable* GetBindlessTable() const { return m_BindlessTable.get(); }
        ID3D12RootSignature* GetMeshRootSignature() const { return m_MeshRootSignature.Get(); }
        // DX12CommandList::DispatchIndirectがExecuteIndirectへ渡すコマンドシグネチャ。
        // 引数はuint3(スレッドグループ数)1個だけで、内容がパスによらず不変のため
        // デバイス初期化時に1つ作って使い回す
        ID3D12CommandSignature* GetDispatchCommandSignature() const { return m_DispatchCommandSignature.Get(); }

    private:
        // CreateBottomLevelAS/CreateTopLevelASの共通部分。組み立て済みの構築入力を受け取り、
        // 必要なサイズを問い合わせてASバッファとスクラッチバッファを確保し、
        // m_UploadCommandList4へ構築コマンドを積んで完了を同期的に待つ。
        // createSrvがtrueの場合はTLAS用のSRVも作る。失敗時はログを出してnullptrを返す
        std::unique_ptr<IRHIAccelerationStructure> BuildAccelerationStructure(
            const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs, bool createSrv, const char* debugName);
        // ID3D12Device5 / ID3D12GraphicsCommandList4の取得とレイトレーシングティアの判定を行い、
        // 結果をm_SupportsRaytracingへ記録する。判定結果は必ずログへ残す
        // (非対応環境では上位層が黙って従来手法へフォールバックするため、ログが唯一の手がかりになる)
        void DetectRaytracingSupport();
        // デバイスが対応する最上位のシェーダーモデルを実測してm_HighestShaderModelへ記録し、
        // 使う.kshaderのバリアントをm_ShaderVariantへ決める。CreateShaderより前に呼ぶ必要がある
        void DetectShaderModelAndSelectVariant();
        // 出力フォルダの.kshaderを1つ読んで、そこに焼かれているバリアントのビット集合を返す。
        // 読めない場合は0を返してログを出す(以降SM 5.0へ縮退する)
        uint32_t ReadShaderVariantMask();
        // bindless(ResourceDescriptorHeap)が使えるかを判定してm_SupportsBindlessへ記録する。
        // シェーダーモデル判定の後、かつルートシグネチャ作成より前に呼ぶこと
        // (CreateRootSignatureがこの結果でフラグを変えるため)
        void DetectBindlessSupport();
        // メッシュシェーダーが使えるかを判定してm_SupportsMeshShaderへ記録する。
        // このエンジンのメッシュシェーダーはジオメトリをbindlessで読むため、
        // DetectBindlessSupportより後に呼ぶこと
        void DetectMeshShaderSupport();
        // コンピュートシェーダーによる自前ラスタライザ(SoftwareRaster.hlsl)が動くかを判定して
        // m_SupportsSoftwareRasterへ記録する。頂点/インデックスをbindlessで引くため
        // DetectBindlessSupportより後に呼ぶこと
        void DetectSoftwareRasterSupport();
        // D3D12_FEATURE_DATA_D3D12_OPTIONS::TiledResourcesTier を引く。
        // Tier 1 は未マップタイルの読み出しが未定義なので採らず、0扱いにする
        void DetectTiledResourcesSupport();

        void CreateRootSignature();
        void CreateComputeRootSignature();
        // 間接ディスパッチ(ExecuteIndirect)用のコマンドシグネチャを1つだけ作る
        void CreateDispatchCommandSignature();
        // メッシュシェーダーパイプライン専用のルートシグネチャ。非対応環境では何もしない
        void CreateMeshRootSignature();
        // 3つのルートシグネチャが共通で足すbindless関連のフラグ。対応環境でのみ
        // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXEDを返す
        D3D12_ROOT_SIGNATURE_FLAGS GetBindlessRootSignatureFlags() const;
        // CreateMippedUAVTextureCube(単一キューブ、SRVはTextureCube)と
        // CreateMippedUAVTextureCubeArray(配列、SRVはTextureCubeArray)の共通実装。
        // 両者はSRVの次元とキューブ枚数以外まったく同じ手順のため1箇所にまとめている
        std::unique_ptr<IRHITexture> CreateCubeTextureInternal(
            uint32_t size, Format format, uint32_t mipLevels, uint32_t cubeCount, bool asArray);
        // 現在のフレームスロット(m_FrameIndex)のコマンドアロケータ/リストを開き直す
        void ResetCommandList();
        // デバッグレイヤーが溜めたメッセージを引き取ってエンジンのログ(KurenaiEngine_DX12.log)へ
        // 出す(デバッグビルドのみ有効)。
        // そのままではデバッガの出力ウィンドウにしか出ず、デバッガを繋がない実行で気付けないため
        void DrainDebugMessages();
        Microsoft::WRL::ComPtr<ID3D12Resource> CreateUploadBuffer(uint64_t sizeInBytes);
        // 公開APIのCreateTextureFromImage(const TextureImage&)から、内部のTexMetadata/ScratchImageを
        // 取り出して実際のGPUリソース作成を行う共通処理(CreateTextureFromFile/CreateSolidColorTexture/
        // CreateTextureFromMemoryからも使う)
        std::unique_ptr<IRHITexture> CreateTextureResourceFromImage(const DirectX::TexMetadata& metadata, const DirectX::ScratchImage& image);
        // 上のうち「ID3D12Resourceを作って全ミップをアップロードする」ところだけ。
        // ReplaceTextureContentsが同じ処理を使うため切り出してある
        Microsoft::WRL::ComPtr<ID3D12Resource> CreateAndUploadTextureResource(
            const DirectX::TexMetadata& metadata, const DirectX::ScratchImage& image);
        // metadataに対応するSRV記述子を組み立てる(2Dとキューブマップの分岐)
        static D3D12_SHADER_RESOURCE_VIEW_DESC MakeSrvDesc(const DirectX::TexMetadata& metadata);
        // GPUがまだ参照しているかもしれないリソースを、フェンスの完了を待ってから解放するための
        // 積み込み口。ストリーミングは毎フレーム少しずつテクスチャを差し替えるため、
        // 既存の「破棄の前にWaitForGPUIdle()を呼ぶ」運用では止まってしまう。
        // どのスレッドから呼んでもよい(mutexで守る)
        void RetireResource(Microsoft::WRL::ComPtr<ID3D12Resource> resource);
        // 積まれたリソースのうち、GPUが使い終わったものを解放する。
        // AdvanceToNextFrame(フレーム境界)から呼ぶ。releaseAll=trueならフェンスを見ずに全部解放する
        // (WaitForGPUIdle直後専用)
        void CollectRetiredResources(bool releaseAll);
        // タイルの貼り替えのうち「外す」側は、GPUがそのミップを読み終わるまで実行できない。
        // リソースの遅延解放と同じ仕組みでフレーム境界まで遅らせてから外し、プールへ返す
        void RetireTileMapping(
            Microsoft::WRL::ComPtr<ID3D12Resource> resource, uint32_t firstMip, uint32_t mipCount,
            std::vector<DX12TilePool::Tile> tiles);
        void CollectRetiredTileMappings(bool releaseAll);
        // 標準ミップ1段ぶんのタイルを貼る/外す。tilesが空ならNULLマッピング(外す)
        void MapStandardMip(
            ID3D12Resource* resource, const DX12TiledTextureState& state, uint32_t mip,
            const std::vector<DX12TilePool::Tile>& tiles);
        // m_UploadCommandListへ記録した内容をクローズして実行投入し、完了を同期的に待ってから開き直す。
        // CreateBuffer/CreateTextureFromImageの初期データアップロード専用(詳細はm_UploadCommandListの
        // コメント参照)
        void UploadSubmitAndWait();
        // 実際に使われているDXGIアダプタ(GPU名・メモリ量)をログへ出す。
        // 性能計測の値が「どのGPUで測ったものか」を後から追えるようにするための診断出力
        void LogAdapterInfo();

        Microsoft::WRL::ComPtr<ID3D12Device> m_Device;
        // DXR用のインタフェース。m_DeviceからQueryInterfaceで取得する。
        // GetRaytracingAccelerationStructurePrebuildInfoを呼ぶのに必要で、
        // 取得に失敗する(＝OS/ドライバがDXR世代でない)場合はm_SupportsRaytracingもfalseになる
        Microsoft::WRL::ComPtr<ID3D12Device5> m_Device5;
        // メッシュシェーダーPSOの作成に使うインタフェース(パイプラインステートストリームを
        // 受け取るCreatePipelineStateはID3D12Device2で追加された)。
        // 取得に失敗する、またはメッシュシェーダー非対応の場合はnullptrのまま
        Microsoft::WRL::ComPtr<ID3D12Device2> m_Device2;
        // AS構築コマンド(BuildRaytracingAccelerationStructure)を積むためのインタフェース。
        // m_UploadCommandList(リソースアップロード専用)からQueryInterfaceで取得する。
        // 毎フレーム用のm_CommandListではなくこちらを使うのは、AS構築がLoadScene(Renderスレッド外)から
        // 呼ばれるため。m_CommandListを触るとRender()が記録中のコマンドリストを壊す
        // (m_UploadCommandListのコメント参照)
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> m_UploadCommandList4;
        // D3D12_FEATURE_D3D12_OPTIONS5のRaytracingTierがTier 1.1以上か。
        // インラインレイトレーシング(HLSLのRayQuery)はTier 1.1で追加された機能のため、
        // Tier 1.0止まりのアダプタではfalseにする。
        // 加えてRayQueryを含むシェーダーはSM 6.5でしかコンパイルできないため、
        // dxcが使えない/シェーダーモデルが6.5未満の環境でもfalseにする
        bool m_SupportsRaytracing = false;
        // HLSLのResourceDescriptorHeap(SM 6.6)が使えるか。シェーダーモデル・dxcのバージョン・
        // リソースバインディングTier 3のすべてを満たしたときだけtrue(DetectBindlessSupport)
        bool m_SupportsBindless = false;
        // 増幅シェーダー/メッシュシェーダーによる描画が使えるか(DetectMeshShaderSupport)
        bool m_SupportsMeshShader = false;
        // コンピュートシェーダーによる自前ラスタライザが使えるか(DetectSoftwareRasterSupport)。
        // bindlessと64bit整数アトミック(Int64ShaderOps)の両方が要る
        bool m_SupportsSoftwareRaster = false;
        // 0 = 使わない(非対応、またはTier 1)。2以上のときだけタイルリソース経路が動く
        uint32_t m_TiledResourcesTier = 0;
        // D3D12_FEATURE_SHADER_MODELで実測した、このデバイスが対応する最上位のシェーダーモデル。
        // 取得できなかった場合はD3D_SHADER_MODEL_5_1相当として扱う(0のまま)
        D3D_SHADER_MODEL m_HighestShaderModel = static_cast<D3D_SHADER_MODEL>(0);
        // CreateShaderが読む.kshaderのキャッシュ。ReleaseShaderPackages()で明示的に捨てる
        Assets::ShaderPackageCache m_ShaderPackages;
        // .kshaderに焼かれているバリアントのビット集合(ShaderPackageHeader::VariantMask)
        uint32_t m_ShaderVariantMask = 0;
        // このデバイスで使うバリアント。DetectShaderModelAndSelectVariantが起動時に1つ決め、
        // CreateShaderは以降これしか見ない。bindless・メッシュシェーダー・レイトレーシングの
        // 可否もすべてこの値から決まる
        Assets::ShaderVariant m_ShaderVariant = Assets::ShaderVariant::Dxbc50;
        // デバッグビルドでのみ取得する(リリースビルドではnullptrのままDrainDebugMessagesが即座に返る)
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> m_InfoQueue;
        Microsoft::WRL::ComPtr<IDXGIFactory2> m_Factory;
        // 実際に使われているアダプタ。VRAM使用量(QueryVideoMemoryInfo)を引くために控える
        Microsoft::WRL::ComPtr<IDXGIAdapter3> m_Adapter;
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

        // 【遅延解放キュー】GPUがまだ参照しているかもしれないリソースの墓場。
        //
        // このエンジンの既定の作法は「破棄の前にWaitForGPUIdle()を呼ぶ」(IRHIDevice.h参照)だが、
        // テクスチャストリーミングは毎フレーム少しずつテクスチャを差し替えるため、
        // そのたびにGPUを空にしていたら描画が止まる。ここへ積んでフレーム境界で回収する。
        //
        // FenceValue==0 は「まだフレーム境界を跨いでいない(未押印)」の意味。
        // CollectRetiredResourcesがフレーム境界でその時点のm_FenceValueを押し、
        // そのフェンスが完了した次の回収で解放する。押印をRenderスレッド側で行うことで、
        // どのスレッドから積まれてもm_FenceValueを競合なく読める
        struct RetiredResource
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
            uint64_t FenceValue = 0;
        };
        std::vector<RetiredResource> m_RetiredResources;
        std::mutex m_RetiredResourcesMutex;

        // 外す予定のタイルマッピング。m_RetiredResourcesと同じ押印の仕組みで遅延させる
        struct RetiredTileMapping
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
            uint32_t FirstMip = 0;
            uint32_t MipCount = 0;
            std::vector<DX12TilePool::Tile> Tiles;
            uint64_t FenceValue = 0;
        };
        std::vector<RetiredTileMapping> m_RetiredTileMappings;
        std::mutex m_RetiredTileMappingsMutex;
        // タイルリソースの裏付けになる物理メモリ。タイルリソースを使うシーンでだけ作られる
        std::unique_ptr<DX12TilePool> m_TilePool;
        std::mutex m_TilePoolMutex;
        // 直前のAdvanceToNextFrame()でWaitForSingleObjectに実際に費やした時間(ms)。
        // フェンスが既に満たされていて待たなかった場合は0になる
        float m_LastFrameGPUWaitTimeMs = 0.0f;

        // CreateBuffer/CreateTextureFromImageの初期データアップロード専用のコマンドリスト/アロケータ/
        // フェンス。m_CommandList(GetImmediateCommandList、毎フレームRenderスレッドが使う)とは
        // 完全に独立させてある。**共有してはいけない** ―― シーン切り替え(LoadScene)のような
        // Render()呼び出しの外からのリソース作成が、Render()が記録中のコマンドリストを
        // Close/Reset/実行してしまい、設定済みのレンダーターゲット/パイプラインステート等を
        // 破壊する(KurenaiEngine2D::BuildFontAtlasをBeginFrame後に呼べない制約の原因もこれ)。
        // 独立させてあるため、LoadSceneをRenderスレッド以外(Updateスレッド等)から呼んでも
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
        // メッシュシェーダーパイプライン用。非対応環境ではnullptrのまま(CreateMeshRootSignature)
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_MeshRootSignature;
        // 間接ディスパッチ用コマンドシグネチャ(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH 1個)。
        // ルートシグネチャに触らない引数のみのためpRootSignatureはnullptrでよい
        Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_DispatchCommandSignature;

        std::unique_ptr<DX12DescriptorHeap> m_RtvHeap;
        std::unique_ptr<DX12DescriptorHeap> m_DsvHeap;
        // 非シェーダー可視のCBV_SRV_UAVヒープ。触るスレッドで2本に分けてある
        // (GetAssetSrvCpuHeap/GetRenderSrvCpuHeapのコメント参照)
        std::unique_ptr<DX12DescriptorHeap> m_AssetSrvCpuHeap;
        std::unique_ptr<DX12DescriptorHeap> m_RenderSrvCpuHeap;
        std::unique_ptr<DX12DescriptorHeap> m_ShaderVisibleSrvHeap;
        std::unique_ptr<DX12DescriptorHeap> m_ShaderVisibleSamplerHeap;
        // m_ShaderVisibleSrvHeapの末尾に切り出したbindless区画の管理。
        // bindless非対応の環境でも「区画は確保するが誰も登録しない」状態で作られる
        // (RegisterBindlessがm_SupportsBindlessを見て早期に弾くため無駄は生じない)
        std::unique_ptr<DX12BindlessTable> m_BindlessTable;
        uint32_t m_FallbackSamplerSetBase = 0;
        // 未バインドスロット埋め用のnullディスクリプタ(m_RenderSrvCpuHeap上に1個ずつ確保する)。
        // デバイスと寿命を共にするため解放は行わない
        uint32_t m_NullSrvIndex = 0;
        uint32_t m_NullUavIndex = 0;

        // ResetCommandList()のたびに進むフレーム通し番号(GetFrameStamp参照)
        uint64_t m_FrameStamp = 0;

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
