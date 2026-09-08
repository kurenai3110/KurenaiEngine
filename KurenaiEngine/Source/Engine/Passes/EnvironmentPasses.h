#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <DirectXMath.h>

#include "RHI/IRHIDevice.h"

#include "EnvironmentConstants.h"

namespace Kurenai::Core
{
    class RenderGraph;
}

namespace Kurenai::Rendering
{
    struct RenderFrameContext;
    struct RenderBlackboard;
}

// 環境(空・大気・雲・IBL)の焼き込みパス群(段階6)。
// AtmosphereLUTBake / SkyViewBake / SkyIntegrate / SkyGenerate / BRDFLUTBake /
// CloudNoiseBake / IBLPrefilter / IBLIrradianceBake。
//
// 【この群がグラフの先頭に来ること】RenderGraph の依存解決は登録順の前方走査で、
// あるパスの Reads は**自分より前に登録された書き手**しか見つけられない。
// SkyViewBake を SkyIntegrate より後ろへ動かすと辺が張られず、未初期化の LUT を積分する。
namespace Kurenai
{
    class KurenaiEngine3D;

    namespace Passes
    {
        class EnvironmentPasses
        {
        public:
            explicit EnvironmentPasses(KurenaiEngine3D& engine) : m_Engine(engine) {}

            void Register(
                Core::RenderGraph& graph,
                const Rendering::RenderFrameContext& frame,
                const Rendering::RenderBlackboard& bb);

            // 【エンジン側の元の行位置から呼ぶこと】DX12はディスクリプタ枠を生成順に
            // 割り当てるため、生成の呼び出しを寄せ集めると他のリソースとの前後関係が崩れ、
            // パスマニフェストの採取が一斉に不一致になる。所有権だけをこの群へ移し、
            // 呼び出しは CreateSceneResources() の元あった場所に残してある。
            // **入口が多いのは、間に IBLResources / SkyResources の生成が挟まるため**で、
            // まとめて1本にしてはいけない
            void CreateBRDFLUTScratch(RHI::IRHIDevice& device, uint32_t size);
            void CreateBRDFLUTPipelineStates(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void CreateCloudNoisePipelineStates(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            // **先頭で焼き直し要求を立て直す** ―― 呼び出し元がLUT3枚を作り直した直後に呼ぶため、
            // 「前回焼いたときの条件」を捨てないと一度も焼かれないまま読まれる
            void CreateAtmosphereResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void CreateIrradiancePipelineState(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void CreateSHResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void CreateSkyGenerateResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void CreateSkyIntegrateResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);

            // 【publicにしてある】BRDF積分LUTの確保成否を呼び出し元が判定に使う。
            // 呼び出し元は IBLResources 側の結果と併せて1つのログを出すため、
            // 判定をこちらへ持ってくると条件とメッセージが分かれてしまう
            bool HasBRDFLUTScratch() const { return m_BRDFLUTScratchTexture != nullptr; }

            // 【publicにしてある】焼き上がりの状態を**進める**のはこの群だけだが、
            // 空が変わったときの焼き直し要求はエンジンのRender()とシーン読み込みが立てる。
            // KurenaiEngine3D の同名のアクセサがここへ委譲している
            bool& GetIBLBaked() { return m_IBLBaked; }
            bool& GetIBLIrradianceBaked() { return m_IBLIrradianceBaked; }
            // 空パラメータのバッファに一度でも中身が入ったか。エンジンのRender()が
            // 「最初のフレームは必ず積分を走らせる」判定に使う
            bool IsSkyParametersBufferInitialized() const { return m_SkyParametersBufferInitialized; }

        private:
            KurenaiEngine3D& m_Engine;

            // BRDF積分LUTは2パスで焼く。パス1(CSMain)が(A, B)をスクラッチへ書き、
            // パス2(CSCombineEavg)がそれを読んでEavgを足した float4(A, B, Eavg, 0) を最終LUTへ書く。
            // 同一リソースをSRVとUAVへ同時バインドできないためスクラッチが要る(BRDFLUT.hlsl参照)。
            // 焼き上がりのLUT本体は IBLResources::BRDFLUTTexture が持つ
            std::unique_ptr<RHI::IRHITexture> m_BRDFLUTScratchTexture;
            std::unique_ptr<RHI::IRHIShader> m_BRDFLUTComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_BRDFLUTPipelineState;
            std::unique_ptr<RHI::IRHIShader> m_BRDFLUTCombineComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_BRDFLUTCombinePipelineState;
            // BRDF積分LUTを焼き終えたか(IBLの焼き上がりとは別管理)。このLUTは(NdotV, ラフネス)の
            // 2Dテーブルでスカイボックスにも太陽の位置にも一切依存しないため、起動後に一度焼けば
            // 二度と焼き直す必要がない。プリフィルタ済み鏡面が空の変化に追従して再ベイクされるように
            // なった以降も巻き込まれて焼き直されないよう、専用のフラグとパスに分離してある
            // (128x128 x 1024サンプル = 約1,680万イテレーションあり、毎回焼くと丸損になる)
            bool m_BRDFLUTBaked = false;

            // ボリュメトリック雲の3Dノイズを焼くコンピュート3本。テクスチャ本体は
            // SkyResources が持つ。純粋な手続き生成なので起動後に一度だけ焼く
            std::unique_ptr<RHI::IRHIShader> m_CloudShapeNoiseComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_CloudShapeNoisePipelineState;
            std::unique_ptr<RHI::IRHIShader> m_CloudDetailNoiseComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_CloudDetailNoisePipelineState;
            std::unique_ptr<RHI::IRHIShader> m_CloudWeatherNoiseComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_CloudWeatherNoisePipelineState;
            bool m_CloudNoiseBaked = false;

            // 大気散乱のLUT(Hillaire 2020)を焼くコンピュート3本。LUT本体は SkyResources が持つ。
            // TransmittanceとMultiScatteringは大気パラメータ(濁りを含む)だけの関数なので
            // 濁りが変わらない限り焼き直さない。SkyViewは太陽の位置と濁りで変わる
            std::unique_ptr<RHI::IRHIBuffer> m_AtmosphereConstantBuffer;
            std::unique_ptr<RHI::IRHIShader> m_TransmittanceComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_TransmittancePipelineState;
            std::unique_ptr<RHI::IRHIShader> m_MultiScatteringComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_MultiScatteringPipelineState;
            std::unique_ptr<RHI::IRHIShader> m_SkyViewComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_SkyViewPipelineState;
            // Transmittance/MultiScatteringを焼いたときの濁り。負の値は「まだ一度も焼いていない」。
            // 濁りが変わるとエアロゾルの量が変わるので、この2枚も焼き直す必要がある
            float m_AtmosphereLUTBakedTurbidity = -1.0f;
            // SkyView LUTを最後に焼いたときの太陽の向きと濁り。負の濁りは「まだ一度も焼いていない」。
            // 【なぜ毎フレーム焼かなくてよいのか】CSSkyViewの入力はこの2つだけである
            // (視点位置はkSkyViewHeightKm固定でカメラに依存しない。AtmosphereLUT.hlsl参照)。
            // どちらも動いていなければ、まったく同じ内容のLUTを焼き直しているだけになる。
            // 手続き空の焼き直し(KurenaiEngine3D::m_LastBakedSunPosition)とまったく同じ判定の形だが、
            // あちらは露出・彩度にも依存するため条件を共用はできない
            float m_SkyViewBakedTurbidity = -1.0f;
            DirectX::XMFLOAT3 m_SkyViewBakedSunPosition{ 0.0f, 0.0f, 0.0f };

            // 拡散イラディアンスの畳み込み。結果は IBLResources::IrradianceTexture へ書く。
            // プリフィルタ側のPSOは IBLResources::PrefilterPipelineState が持つ
            std::unique_ptr<RHI::IRHIShader> m_IrradianceComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_IrradiancePipelineState;
            // 拡散イラディアンスの球面調和関数(SH L2)経路。CSIrradianceの高速な代替で、
            // m_IBLSettings.UseSHIrradianceでA/B比較できるようトグルにしてある。
            // 詳細はIBLConvolve.hlsl冒頭のコメントとdocs/Architecture.htmlを参照
            std::unique_ptr<RHI::IRHIShader> m_ProjectSHComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_ProjectSHPipelineState;
            std::unique_ptr<RHI::IRHIShader> m_ProjectSHFinalComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_ProjectSHFinalPipelineState;
            std::unique_ptr<RHI::IRHIShader> m_EvaluateSHComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_EvaluateSHPipelineState;
            // SHの部分和(CSProjectSHのグループごとの出力)と最終係数(CSProjectSHFinalの出力)
            std::unique_ptr<RHI::IRHIBuffer> m_SHPartialSumsBuffer;
            std::unique_ptr<RHI::IRHIBuffer> m_SHCoefficientsBuffer;

            // 手続き空(SkyGenerate.hlsl)。太陽が動くたびに焼き直す。
            // 書き先のキューブマップは SkyResources::ProceduralSkyTexture が持つ
            std::unique_ptr<RHI::IRHIShader> m_SkyGenerateComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_SkyGeneratePipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_SkyBakeConstantBuffer;

            // 空パラメータ(ティント4本+照度正規化済みの天頂輝度)の積分。SkyGenerateより前に
            // 実行し、結果を SkyResources::ParametersBuffer へ書く
            std::unique_ptr<RHI::IRHIShader> m_SkyIntegrateComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_SkyIntegratePipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_SkyIntegrateConstantBuffer;
            // パラメータバッファに一度でも中身が入ったか。**CPUからゼロ初期化できない**ため、
            // 最初のフレームは必ず積分を走らせる必要がある(理由は生成箇所のコメント)
            bool m_SkyParametersBufferInitialized = false;
            // 雲のノイズテクスチャが無くP18(雲込みの空の照度)を積めなかったことを1度だけログへ出す。
            // 毎ベイクで出すとログが埋まるため(m_PlanarReflectionMultipleWaterLoggedと同じ扱い)
            bool m_SkyIntegrateCloudMissingLogged = false;

            // 鏡面プリフィルタを焼き終えたか。空が変わるとエンジンのRender()と
            // シーン読み込みが落とし、この群が焼き直して立て直す
            bool m_IBLBaked = false;
            // 検証用の拡散イラディアンスマップを焼き終えたか(上とは別管理)。既定の描画経路は
            // プリフィルタ済み鏡面の最終ミップなので、こちらは検証を有効にしたときにだけ焼く
            bool m_IBLIrradianceBaked = false;
        };
    }
}
