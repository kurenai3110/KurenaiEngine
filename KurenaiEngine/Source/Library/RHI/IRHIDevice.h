#pragma once

#include <memory>
#include <string>

#include "KurenaiTypes.h"

#include "IRHIAccelerationStructure.h"
#include "IRHIBuffer.h"
#include "IRHICommandList.h"
#include "IRHIGPUProfiler.h"
#include "IRHIImGuiBackend.h"
#include "IRHIPipelineState.h"
#include "IRHISamplerSet.h"
#include "IRHIShader.h"
#include "IRHISwapChain.h"
#include "IRHITexture.h"
#include "RHIDesc.h"
#include "TextureImage.h"

namespace Kurenai::RHI
{
    class KURENAI_LIB_API IRHIDevice
    {
    public:
        virtual ~IRHIDevice() = default;

        virtual std::unique_ptr<IRHISwapChain> CreateSwapChain(void* windowHandle, uint32_t width, uint32_t height) = 0;
        virtual std::unique_ptr<IRHIBuffer> CreateBuffer(const BufferDesc& desc) = 0;
        virtual std::unique_ptr<IRHIShader> CreateShader(const ShaderDesc& desc) = 0;
        virtual std::unique_ptr<IRHIPipelineState> CreatePipelineState(const PipelineStateDesc& desc) = 0;
        // コンピュートシェーダー(ShaderStage::Computeで作成したIRHIShader)用のパイプラインステート
        virtual std::unique_ptr<IRHIPipelineState> CreateComputePipelineState(const ComputePipelineStateDesc& desc) = 0;
        virtual std::unique_ptr<IRHITexture> CreateTextureFromFile(const std::wstring& filePath, bool sRGB) = 0;
        // デコード済みのTextureImageからGPUリソースを作成する。デコード(CPU処理、TextureImage::LoadFromFile)は
        // GPUデバイスを必要としないためワーカースレッドで並列に行えるが、GPUリソース作成自体は
        // デバイスに紐づく処理なので直列に行う必要がある。呼び出し側(ModelLoader::TextureLoader::Prefetch)は
        // この2つを分離することで、大量のテクスチャを持つモデルの読み込みを並列化する
        virtual std::unique_ptr<IRHITexture> CreateTextureFromImage(const TextureImage& image) = 0;
        virtual std::unique_ptr<IRHITexture> CreateSolidColorTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a) = 0;
        // RGBA8(1ピクセル4バイト、行間パディングなしのタイトパッキング)のピクセルデータからテクスチャを
        // 作成する。実行時に生成したデータ(フォントアトラス等)をアップロードする用途向け
        virtual std::unique_ptr<IRHITexture> CreateTextureFromMemory(uint32_t width, uint32_t height, const void* pixelsRGBA8) = 0;
        virtual std::unique_ptr<IRHITexture> CreateRenderTexture(uint32_t width, uint32_t height, Format format) = 0;
        // コンピュートシェーダーから書き込み可能なUAV+SRVテクスチャ(RWTexture2D)
        virtual std::unique_ptr<IRHITexture> CreateUAVTexture(uint32_t width, uint32_t height, Format format) = 0;
        // CreateUAVTextureの3D版(RWTexture3D / Texture3D、単一ミップ)。ボリュームテクスチャを
        // コンピュートシェーダーで生成してピクセルシェーダーから読む用途向け
        // (現在の利用者はボリュメトリック雲の形状ノイズ。Shaders/3D/CloudNoiseGenerate.hlsl)。
        //
        // 【サンプラーに注意】この種のテクスチャはワールド空間の座標で無限にタイリングして引くのが
        // 前提なので、必ずWrapのサンプラー(Shaders/3D/Samplers.hlsliのs3 VolumeSampler)で読むこと。
        // Clampで読むと周期の境界でトライリニア補間のタップが端のテクセルに張り付き、
        // 一定間隔で継ぎ目が出る。シェーダー側でfrac()してもこの継ぎ目は消せない
        // (補間そのものがテクスチャの端を跨げないため)。
        //
        // 書き込みは既存のSetComputeUnorderedAccessTexture(mipLevel=0)、読み出しは通常の
        // SetTexture/SetComputeTextureがそのまま使える(3D専用のバインドAPIは増やしていない)
        virtual std::unique_ptr<IRHITexture> CreateUAVTexture3D(
            uint32_t width, uint32_t height, uint32_t depth, Format format) = 0;
        // Hi-Zミップチェーン用のテクスチャ。単チャンネル(R32_Float)でwidth/heightから1x1までの
        // フルミップチェーンを持ち、各ミップに個別のUAV(RWTexture2D、SetComputeUnorderedAccessTextureの
        // mipLevel引数で指定)を張る。コンピュートシェーダーで「ミップNを読んでミップN+1へ2x2ブロックの
        // 最小値(Reverse-Zのため最も遠い深度)を書き込む」ダウンサンプルを1ミップずつ繰り返せるようにするための、
        // 通常のCreateUAVTexture(常に1ミップ)とは別の専用ファクトリ
        virtual std::unique_ptr<IRHITexture> CreateHiZTexture(uint32_t width, uint32_t height, uint32_t mipLevels) = 0;
        // CreateHiZTextureの汎用版。フォーマットを指定できるフルミップチェーンのUAV+SRVテクスチャを作る。
        // IBLのプリフィルタ済み鏡面マップ(ラフネスに応じてミップごとに異なる畳み込みを書き込む、
        // HDRのためR16G16B16A16_Float)のように、Hi-Z以外の用途でもミップ単位のUAV書き込みが必要な場合に使う
        virtual std::unique_ptr<IRHITexture> CreateMippedUAVTexture(uint32_t width, uint32_t height, Format format, uint32_t mipLevels) = 0;
        // コンピュートシェーダーから面ごとに書き込み可能なキューブマップ(6面、単一ミップ)。
        // IBLの拡散イラディアンス(IBLConvolve.hlsl CSIrradiance)のように、畳み込み結果を
        // 本物のTextureCubeとして保持したい場合に使う(SetComputeUnorderedAccessTextureCubeFaceで
        // 面を選んで書き込み、通常のSetTexture/SetComputeTextureでTextureCubeとして読める)
        virtual std::unique_ptr<IRHITexture> CreateUAVTextureCube(uint32_t size, Format format) = 0;
        // CreateUAVTextureCubeのフルミップチェーン版。IBLのプリフィルタ済み鏡面(ミップごとに
        // 異なるラフネスで畳み込む)のように、面×ミップの組み合わせごとに個別のUAV書き込みが
        // 必要な場合に使う
        virtual std::unique_ptr<IRHITexture> CreateMippedUAVTextureCube(uint32_t size, Format format, uint32_t mipLevels) = 0;
        // CreateMippedUAVTextureCubeのキューブマップ配列版(SRVはTextureCubeArray)。
        // リフレクションプローブのように「同じ解像度のキューブマップを複数枚持ち、シェーダー側で
        // ピクセルごとに異なる番号のキューブを選んでサンプルしたい」場合に使う。HLSLは別々の
        // TextureCubeリソースを動的に添字参照できないため(カスケードシャドウマップが
        // ShadowMap0〜3を個別スロットに分けているのと同じ制約)、複数プローブを1つの
        // TextureCubeArrayにまとめる必要がある。
        //
        // 単一キューブのCreateMippedUAVTextureCubeとはSRVの次元が異なる(TextureCube /
        // TextureCubeArray)ため、HLSL側の宣言と一致する方を選んで使い分けること。
        // 書き込みはSetComputeUnorderedAccessTextureCubeFaceのcubeIndex引数でキューブを選ぶ
        virtual std::unique_ptr<IRHITexture> CreateMippedUAVTextureCubeArray(
            uint32_t size, Format format, uint32_t mipLevels, uint32_t cubeCount) = 0;
        // clearDepth: このテクスチャの最適クリア値(DX12のD3D12_CLEAR_VALUE用)。実際のクリア値は
        // IRHICommandList::ClearDepthで毎回明示的に指定するが、DX12は生成時に宣言した値と
        // 一致しないと高速クリアパスが使えないため、Reverse-Zで0.0fクリアするテクスチャはここも合わせる
        virtual std::unique_ptr<IRHITexture> CreateDepthTexture(uint32_t width, uint32_t height, float clearDepth = 1.0f) = 0;
        // CreateDepthTextureのテクスチャ配列版。arraySize枚のスライスを1つのリソースとして持ち、
        // 書き込みはスライスごとの個別DSV(IRHICommandList::SetRenderTargetsのdepthArraySliceで選ぶ)、
        // 読み取りは全スライスをまとめた1本のTexture2DArray SRVで行う。
        //
        // カスケードシャドウマップのように「スライスごとに別の深度を描き、サンプル時は動的に
        // スライスを選びたい」用途で使う。CreateDepthTextureを枚数分並べる方式と違い、HLSL側が
        // ShadowMapArray.Sample(s, float3(uv, index))と書けるのが利点(HLSLはリソースそのものを
        // 動的添字で選べないが、配列スライスは選べるため、カスケードごとの分岐が不要になる)。
        // clearDepthの意味はCreateDepthTextureと同じ
        virtual std::unique_ptr<IRHITexture> CreateDepthTextureArray(
            uint32_t width, uint32_t height, uint32_t arraySize, float clearDepth = 1.0f) = 0;
        // 1パスがまとめてバインドするサンプラーの組を作る。descs[i]がレジスタs(i)に対応する。
        // countがバックエンドのスロット数(DX12のkSamplerSlotCount)に満たない場合、残りのスロットは
        // 既定のサンプラーで埋められる(DX12は未初期化のディスクリプタがテーブルに含まれると動作が未定義になるため)。
        //
        // 【重要】描画開始前(初期化時)にのみ呼ぶこと。DX12実装はシェーダ可視ヒープへ直接書き込むため、
        // 描画中に呼ぶとGPUがまだ読んでいる可能性のあるディスクリプタを壊す(詳細はIRHISamplerSet.h)
        virtual std::unique_ptr<IRHISamplerSet> CreateSamplerSet(const SamplerDesc* descs, uint32_t count) = 0;
        virtual IRHICommandList* GetImmediateCommandList() = 0;

        // ImGui連携。ImGuiはバックエンド(DX11/DX12)ごとに専用の実装が必要なため、
        // このRHI抽象化層でも他のAPIと同様にバックエンド実装側(DX11Deviceなど)に委譲する
        virtual std::unique_ptr<IRHIImGuiBackend> CreateImGuiBackend(void* windowHandle) = 0;

        // GPUタイムスタンプクエリによる区間計測。DX11/DX12でクエリの仕組みが異なるため
        // バックエンド実装側(DX11Deviceなど)に委譲する
        virtual std::unique_ptr<IRHIGPUProfiler> CreateGPUProfiler() = 0;

        // 直前のPresent呼び出しでCPUがGPUの完了を待つのに費やした時間(ms)。
        // フレームパイプライン化(多重バッファリング)を行わないDX11では常に0を返す。
        // これは実際のCPU負荷ではなくGPU側の処理時間を反映した待ち時間なので、
        // CPU時間の表示からはこの値を差し引いて実質的なCPU負荷のみを示す
        virtual float GetLastFrameGPUWaitTimeMs() const = 0;

        // GPUが投入済みのコマンドをすべて処理し終えるまでCPU側でブロックする。DX12はCPUがGPUの
        // 完了を待たずに次フレームの記録を始める多重バッファリング設計のため、直前数フレームぶんの
        // 描画コマンドがまだGPU上で実行中の可能性がある。LoadScene等で既存のGPUリソース
        // (頂点/インデックスバッファ・テクスチャ)を破棄する前にこれを呼ばないと、GPUがまだ
        // 参照しているメモリを解放してしまい、ヒープ破損やクラッシュを引き起こし得る。
        // DX11は同期的なリソース管理のため実質的に即座に返る
        virtual void WaitForGPUIdle() = 0;

        // --- bindless -------------------------------------------------------------------------

        // HLSLのResourceDescriptorHeap(シェーダーモデル6.6)が使えるか。
        // DX11には存在しないため常にfalse。DX12でもシェーダーモデル・dxcのバージョン・
        // リソースバインディングTierのいずれかが足りなければfalseになる。
        //
        // 上位層はbindlessを前提にした経路へ入る前に必ずこれを確認すること。
        // falseの場合、下のRegisterBindlessは常にkInvalidBindlessIndexを返す
        // (SupportsRaytracing()と同じ扱い方。詳細はRHIBindless.h)
        virtual bool SupportsBindless() const = 0;

        // テクスチャ/バッファのSRVをbindlessヒープへ登録し、シェーダーが使う番号を返す。
        // 非対応環境・登録失敗時はkInvalidBindlessIndexを返す(例外は投げない)。
        //
        // 登録したディスクリプタはリソースが破棄されるまで生き続け、破棄時に自動で返却される。
        // 同じリソースに対して複数回呼ぶと別々の番号が払い出されて区画を無駄に消費するため、
        // 呼び出し側は結果を保持して使い回すこと(IRHITexture::GetBindlessIndexで取り出せる)
        virtual uint32_t RegisterBindless(IRHITexture* texture) = 0;
        virtual uint32_t RegisterBindless(IRHIBuffer* buffer) = 0;

        // bindless区画の使用数と容量。非対応バックエンドは両方0を返す。
        //
        // 【なぜ見えるようにするのか】区画が満杯になったときRegisterBindlessは例外を投げず、
        // エラーログを出してkInvalidBindlessIndexを返す。消費側はそれを「テクスチャ無し」と
        // 解釈して白1x1へ落とすので、**絵はそれらしく出たまま静かに間違う**。
        // 上限に近づいていることを事前に見えるようにしておかないと、
        // 「なぜかこのモデルだけ真っ白」の形でしか気づけない
        virtual uint32_t GetBindlessUsedCount() const { return 0; }
        virtual uint32_t GetBindlessCapacity() const { return 0; }

        // --- メッシュシェーダー ---------------------------------------------------------------

        // 増幅シェーダー/メッシュシェーダーによる描画(DispatchMesh)が使えるか。
        // DX11には存在しないため常にfalse。DX12でもメッシュシェーダーTier 1未満、
        // あるいはbindless非対応の環境ではfalseになる
        // (このエンジンのメッシュシェーダーはジオメトリをbindlessで読むため)。
        //
        // falseの環境では従来の頂点シェーダー + DrawIndexedで描くこと
        virtual bool SupportsMeshShader() const = 0;

        // 増幅シェーダー(任意)+ メッシュシェーダー + ピクセルシェーダーのパイプラインステート。
        // 入力レイアウトを持たない点以外はCreatePipelineStateと同じ扱いができる。
        // 非対応環境・作成失敗時はログを出してnullptrを返す(例外は投げない)
        virtual std::unique_ptr<IRHIPipelineState> CreateMeshPipelineState(const MeshPipelineStateDesc& desc) = 0;

        // --- ソフトウェアラスタライザ ---------------------------------------------------------

        // コンピュートシェーダーによる自前ラスタライザ(SoftwareRaster.hlsl)が動く環境か。
        // 必要なのは次の2つで、どちらもDX11には無いため常にfalse:
        //   (1) bindless(SupportsBindless)。頂点/インデックスをResourceDescriptorHeapで引くため。
        //       この時点でシェーダーモデル6.6とリソースバインディングTier 3が保証される
        //   (2) 64bit整数のアトミック(D3D12_FEATURE_DATA_D3D12_OPTIONS1::Int64ShaderOps)。
        //       深度と三角形IDを1ワードへ詰めてInterlockedMaxで解決するため
        //
        // 上位層はこの経路へ入る前に必ず確認すること(SupportsRaytracing()と同じ扱い方)
        virtual bool SupportsSoftwareRaster() const = 0;

        // --- レイトレーシング -----------------------------------------------------------------

        // インラインレイトレーシング(HLSLのRayQuery、DXR 1.1)が使えるか。
        // DX11にはレイトレーシングAPIが存在しないため常にfalse。DX12でも、アダプタが
        // D3D12_RAYTRACING_TIER_1_1に満たない場合はfalseになる。
        //
        // 上位層はレイトレーシングを使う経路へ入る前に必ずこれを確認し、falseなら
        // 従来のスクリーンスペース手法(SSR・CSM・SSAO)へフォールバックすること。
        // 下のCreate*ASもfalseの環境では常にnullptrを返す
        virtual bool SupportsRaytracing() const = 0;

        // 1フレームのあいだに安全に発行できる描画(Draw/DrawIndexed)の回数。
        //
        // 【なぜ上位層が知る必要があるのか】DX12は描画のたびにSRVディスクリプタテーブルの
        // ブロックを新規に払い出す(同じスロットを使い回すと、1フレーム分をまとめて実行する設計上、
        // GPU実行時には最後のSetTextureの内容へ全描画が上書きされてしまうため)。
        // ヒープは有限なので1フレームの払い出し回数にも上限があり、超えると例外になる。
        //
        // 描画回数がシーンの内容で決まるパス ―― DDGIのラスタ経路は
        // 「プローブ数 × 6面 × 不透明メッシュ数」だけ描く ―― は、自分の仕事量が
        // この上限に収まるかを確かめ、収まらないなら仕事量の側を減らさなければならない。
        //
        // DX11はディスクリプタテーブルという概念を持たないため上限が無い(UINT32_MAX)
        virtual uint32_t GetMaxDrawsPerFrame() const = 0;

        // モデル1つ分のジオメトリからBLAS(Bottom Level AS)を構築する。
        // 構築はGPU上で行われるが、この関数を抜けた時点で完了が保証される(内部で同期する)ため、
        // 戻り値をそのままTLASの入力として使ってよい。
        // 非対応環境・構築失敗時はログを出してnullptrを返す(例外は投げない)
        virtual std::unique_ptr<IRHIAccelerationStructure> CreateBottomLevelAS(const BottomLevelASDesc& desc) = 0;

        // BLASへの参照とワールド変換の一覧からTLAS(Top Level AS)を構築する。
        // シェーダーへバインドできるのはこちらだけ(IRHICommandList::SetComputeAccelerationStructure)。
        // desc.Instancesの各要素が指すBLASは、TLASより長く生存させること。
        // 非対応環境・構築失敗時はログを出してnullptrを返す
        virtual std::unique_ptr<IRHIAccelerationStructure> CreateTopLevelAS(const TopLevelASDesc& desc) = 0;
    };

    KURENAI_LIB_API std::unique_ptr<IRHIDevice> CreateDX11Device();
    KURENAI_LIB_API std::unique_ptr<IRHIDevice> CreateDX12Device();
}
