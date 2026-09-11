#pragma once

#include <memory>
#include "PassHost.h"
#include <string>

#include "RHI/IRHIDevice.h"

namespace Kurenai::Core
{
    class RenderGraph;
}

namespace Kurenai::Rendering
{
    struct RenderFrameContext;
}

// シャドウのパス群(段階6)。
//
// 【カスケードとRTシャドウで登録位置が離れている】カスケードシャドウマップは
// グラフのほぼ先頭、RTシャドウはMegaLightsの後ろに登録される。登録順は実行順の
// 一部なので寄せられない。したがって登録の入口を分け、それぞれ元の位置から呼ぶ。
namespace Kurenai
{
    class KurenaiEngine3D;

    namespace Passes
    {
        class ShadowPasses
        {
        public:
            explicit ShadowPasses(IPassHost& engine) : m_Engine(engine) {}

            // カスケードシャドウマップ(Shadow0..Shadow3)
            void RegisterCascades(Core::RenderGraph& graph, const Rendering::RenderFrameContext& frame);

            // RTシャドウ(DXR対応環境のみ)。カスケードとは登録位置が離れている
            void RegisterRaytraced(Core::RenderGraph& graph, const Rendering::RenderFrameContext& frame);

            void CreateMeshletShaders(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void CreateRaytracedResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void CreateCascadePipelineStates(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void CreateCascadeConstantBuffer(RHI::IRHIDevice& device);

            uint32_t GetDrawCalls() const { return m_DrawCallsShadow; }
            void ResetDrawCalls() { m_DrawCallsShadow = 0; }
            bool HasRaytracedPipelineState() const { return m_RTShadowPipelineState != nullptr; }

        private:
            IPassHost& m_Engine;

            // RTシャドウパス: TLASへ太陽の見かけの円盤に向けて影レイを撃ち、可視率(0〜1)を
            // 単チャンネルのテクスチャへ書くコンピュートパス。DirectLighting.hlslがt6で読み、
            // CSMのComputeCascadedShadowFactorの戻り値と同じ位置で使う(26章)。
            // シェーダーとパイプラインステートはm_RenderCapabilities.RaytracingAvailableがtrueのときだけ作る
            std::unique_ptr<RHI::IRHIShader> m_RTShadowComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_RTShadowPipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_RTShadowConstantBuffer;

            std::unique_ptr<RHI::IRHIShader> m_ShadowVertexShader;
            std::unique_ptr<RHI::IRHIShader> m_ShadowPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_ShadowPipelineState;
            std::unique_ptr<RHI::IRHIPipelineState> m_ShadowPipelineStateMirrored;
            // メッシュシェーダー版のシャドウ(Shaders/3D/ShadowMeshlet.hlsl)。
            // 非対応環境ではすべてnullptrのままで、描画側は従来のメッシュ単位経路を使う
            std::unique_ptr<RHI::IRHIShader> m_ShadowAmplificationShader;
            std::unique_ptr<RHI::IRHIShader> m_ShadowMeshShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_ShadowMeshletPipelineState;
            std::unique_ptr<RHI::IRHIPipelineState> m_ShadowMeshletPipelineStateMirrored;
            // アルファカットアウト(glTFのalphaMode=MASK)の影。ピクセルシェーダーは
            // 頂点シェーダー経路とメッシュシェーダー経路で共有する。
            // **DX11でも効く**(bindlessもメッシュシェーダーも要らない)
            std::unique_ptr<RHI::IRHIShader> m_ShadowCutoutVertexShader;
            std::unique_ptr<RHI::IRHIShader> m_ShadowCutoutPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_ShadowCutoutPipelineState;
            std::unique_ptr<RHI::IRHIPipelineState> m_ShadowCutoutPipelineStateMirrored;
            std::unique_ptr<RHI::IRHIPipelineState> m_ShadowMeshletCutoutPipelineState;
            std::unique_ptr<RHI::IRHIPipelineState> m_ShadowMeshletCutoutPipelineStateMirrored;
            // シャドウパスの各カスケード描画で使う専用の定数バッファ(カスケードごとに値を更新して使い回す)
            std::unique_ptr<RHI::IRHIBuffer> m_ShadowCascadeConstantBuffer;

            uint32_t m_DrawCallsShadow = 0;
        };
    }
}
