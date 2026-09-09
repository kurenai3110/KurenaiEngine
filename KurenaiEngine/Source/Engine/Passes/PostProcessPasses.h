#pragma once

#include <cstdint>
#include "PassHost.h"
#include <memory>
#include <string>

#include "RHI/IRHIDevice.h"

#include "PostProcessConstants.h"

namespace Kurenai::Core
{
    class RenderGraph;
}

namespace Kurenai::RHI
{
    class IRHICommandList;
}

namespace Kurenai::Rendering
{
    struct RenderFrameContext;
    struct RenderBlackboard;
}

// ポストプロセスのパス群(段階6)。
// 大気遠近 / ドローショー / TAA / 自動露出 / ブルーム / トーンマップ / 超解像(EASU・RCAS)。
//
// 【この群が Blackboard へ書く】TAA の蓄積結果を指す HdrSceneColor と、
// 超解像を走らせたかの UpscaleActive は、後ろの Present が読む。
// そのため bb は非 const で受ける(読むだけの群は const& で受けること)。
namespace Kurenai
{
    class KurenaiEngine3D;

    namespace Passes
    {
        class PostProcessPasses
        {
        public:
            explicit PostProcessPasses(IPassHost& engine) : m_Engine(engine) {}

            void Register(
                Core::RenderGraph& graph,
                const Rendering::RenderFrameContext& frame,
                Rendering::RenderBlackboard& bb);

            // 【エンジン側の元の行位置から呼ぶこと】DX12はディスクリプタ枠を生成順に
            // 割り当てるため、生成の呼び出しを寄せ集めると他のリソースとの前後関係が崩れ、
            // パスマニフェストの採取が一斉に不一致になる。所有権だけをこの群へ移し、
            // 呼び出しは CreateSceneResources() の元あった場所に残してある
            void CreateAerialPerspectivePipelineState(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void CreateTAAPipelineState(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void CreateTonemapPipelineState(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void CreateUpscalePipelineStates(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            // **露出テクスチャ(2x1)はここでは作らない** ―― フレームをまたいで順応を保つため
            // 持ち主が RenderTargets(ExposureTexture)にあり、その生成はこの直後に続く
            void CreateAutoExposureResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void CreateBloomPipelineStates(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);

            // 【publicにしてある】次のAutoExposureパスで順応を飛ばす要求。立てるのは
            // シーン読み込みで、消費する(falseへ戻す)のはこの群だけ。
            // KurenaiEngine3D は状態を持たず、同名のアクセサでここへ委譲している
            bool& GetAutoExposureResetRequested() { return m_AutoExposureResetRequested; }

        private:
            IPassHost& m_Engine;

            // --- 大気遠近(height fog / aerial perspective) ---
            // 反射パス(SSR/RT反射)の後、TAAパスの直前に置くフルスクリーン三角形+ピクセルシェーダー。
            // Lightingパスの中へ入れない理由・TAAより前へ置く理由はShaders/3D/AerialPerspective.hlsl
            // 冒頭のコメント参照。書き先(RenderTargets::AerialPerspectiveTexture)は
            // レンダー解像度に追従して作り直すため RenderTargets 側が持つ
            std::unique_ptr<RHI::IRHIShader> m_AerialPerspectiveVertexShader;
            std::unique_ptr<RHI::IRHIShader> m_AerialPerspectivePixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_AerialPerspectivePipelineState;

            // TAA(Temporal Anti-Aliasing)パス: SSRの後、露出/ブルーム/トーンマップの前に置く。
            // 毎フレーム投影行列を1ピクセル未満だけずらして(ジッター)サンプル位置を散らし、
            // モーションベクターで前フレームの結果を今フレームの画素へ再投影して蓄積する。
            // 静止していれば十数フレームで収束し、実質的なスーパーサンプリングになる。
            // 詳細な原理と各工夫の理由はArchitecture.htmlのTAAの章を参照。
            // **履歴バッファ2枚とその添字・有効フラグはエンジンが持つ** ――
            // 添字はRender()の末尾で入れ替わり、有効フラグはシーン読み込み(別スレッド)も落とす
            std::unique_ptr<RHI::IRHIShader> m_TAAVertexShader;
            std::unique_ptr<RHI::IRHIShader> m_TAAPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_TAAPipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_TAAConstantBuffer;

            // Tonemapパス: SceneColor(SSR有効時はRenderTargets::SSRTexture)のHDR値をReinhardトーンマッピング+
            // ガンマ補正でLDRへ変換し、Presentパスへ渡す。SSR等のHDR演算より後、Present直前の
            // 独立したステージとして置くことで、反射やブルーム/露出制御がトーンマップの
            // 影響を受けないHDR値の上に成立できるようにする
            std::unique_ptr<RHI::IRHIShader> m_TonemapVertexShader;
            std::unique_ptr<RHI::IRHIShader> m_TonemapPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_TonemapPipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_TonemapConstantBuffer;

            // 超解像パス(Upscale.hlsl): Tonemapが出したLDR画像を、EASUで出力解像度へ再構成し、
            // RCASでシャープ化してからPresentへ渡す。出力2枚と実寸(RenderTargets::UpscaleTexture /
            // UpscaleSharpTexture / UpscaleTargetWidth / Height)はPresentPassも読むため
            // 持ち主がRenderTargetsにある。作り直しはCreateRenderTargets()とは別の契機で走る
            std::unique_ptr<RHI::IRHIShader> m_UpscaleEASUComputeShader;
            std::unique_ptr<RHI::IRHIShader> m_UpscaleRCASComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_UpscaleEASUPipelineState;
            std::unique_ptr<RHI::IRHIPipelineState> m_UpscaleRCASPipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_UpscaleConstantBuffer;

            // 自動露出(eye adaptation)パス: SceneColorの輝度ヒストグラムをGPUで作り、
            // 低/高パーセンタイルを除外した加重平均から目標EV100を求めて時間方向に追従させる。
            // 結果はRenderTargets::ExposureTextureへ書かれ、Tonemapパスが読んで露出倍率に変換する。
            //
            // 露出そのものはCPU側でライト強度へ事前乗算されている(プリ露出方式)。
            // 自動露出の結果をライト強度へ戻すとフィードバックループになり、
            // かつGPU→CPUのリードバック(同期待ち)が要るため、プリ露出は固定のままにして
            // 「プリ露出EVと自動露出EVの差」だけをTonemapで掛ける構成にしている
            // (詳細はAutoExposure.hlsl冒頭)
            std::unique_ptr<RHI::IRHIShader> m_AutoExposureClearComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_AutoExposureClearPipelineState;
            std::unique_ptr<RHI::IRHIShader> m_AutoExposureHistogramComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_AutoExposureHistogramPipelineState;
            std::unique_ptr<RHI::IRHIShader> m_AutoExposureResolveComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_AutoExposureResolvePipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_ExposureHistogramBuffer;
            std::unique_ptr<RHI::IRHIBuffer> m_AutoExposureConstantBuffer;

            // 次のAutoExposureパスで順応を飛ばして測光値へ即座に合わせる要求。シーン読み込みが立て、
            // パスを積んだ時点で消費する。
            //
            // 順応の状態はGPU側のExposureTexture(2x1)に入っており、初回だけ順応を飛ばすための
            // フラグもそこのテクセル(1,0)にある(UAVがゼロ初期化されることを利用している)。
            // つまりCPU側からは「初回に戻す」手段が無く、シーンを切り替えても前のシーンの露出から
            // 順応が続いてしまう。シーン切り替えは視点の移動ではなく場面の切り替わりなので、
            // 目の順応を模す理由が無い(AutoExposure.hlslのCSResolveのコメントもそう宣言している)
            bool m_AutoExposureResetRequested = false;

            // ブルームパス(Bloom.hlsl): 半解像度から始まるピラミッドを段階的にダウンサンプルし、
            // 3x3テントで戻しながら加算することで広く滑らかな光の裾を作る。
            //
            // ピラミッドをミップチェーン1枚ではなくレベルごとの独立テクスチャで持っているのは、
            // 同一リソースのSRV/UAV同時バインドを避けるため(理由の詳細はBloom.hlsl冒頭)。
            // ピラミッド本体(RenderTargets::BloomDownTextures / BloomUpTextures / BloomLevelSizes)は
            // PresentPassのデバッグ表示も読むため、持ち主がRenderTargetsにある
            std::unique_ptr<RHI::IRHIShader> m_BloomDownsampleComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_BloomDownsamplePipelineState;
            std::unique_ptr<RHI::IRHIShader> m_BloomUpsampleComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_BloomUpsamplePipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_BloomConstantBuffer;
        };
    }
}
