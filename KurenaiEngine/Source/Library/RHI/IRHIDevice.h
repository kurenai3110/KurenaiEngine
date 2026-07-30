#pragma once

#include <memory>
#include <string>

#include "KurenaiTypes.h"

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
    };

    KURENAI_LIB_API std::unique_ptr<IRHIDevice> CreateDX11Device();
    KURENAI_LIB_API std::unique_ptr<IRHIDevice> CreateDX12Device();
}
