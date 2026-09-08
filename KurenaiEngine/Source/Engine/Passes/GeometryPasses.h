#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "RHI/IRHIDevice.h"

#include "GeometryConstants.h"

namespace Kurenai::Core
{
    class RenderGraph;
}

namespace Kurenai::Rendering
{
    struct RenderFrameContext;
    struct RenderBlackboard;
}

// ジオメトリのパス群(段階6)。
// 深度プリパス / モデル単位GPUカリング(動的なパス名) / G-Buffer /
// ModelCullReadback / ソフトウェアラスタライザ / Hi-Z。
//
// 【プリパスとG-Bufferは同じ組を描かなければならない】片方だけ間引くと
// 「深度はあるのに色が無い」穴が開く。組を揃える保証は、両者が
// Rendering/GeometryDrawLoop.h の同じ列挙(ForEachGeometryDraw)を通ることにある。
namespace Kurenai
{
    class KurenaiEngine3D;

    namespace Passes
    {
        class GeometryPasses
        {
        public:
            explicit GeometryPasses(KurenaiEngine3D& engine) : m_Engine(engine) {}

            void Register(
                Core::RenderGraph& graph,
                const Rendering::RenderFrameContext& frame,
                Rendering::RenderBlackboard& bb);

            // 【エンジン側の元の行位置から呼ぶこと】DX12はディスクリプタ枠を生成順に
            // 割り当てるため、生成の呼び出しを寄せ集めると他のリソースとの前後関係が崩れ、
            // パスマニフェストの採取が一斉に不一致になる。所有権だけをこの群へ移し、
            // 呼び出しは元あった場所に残してある
            void CreateHiZResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            // メッシュシェーダー経路のG-Buffer PSOが作れたか。
            // KurenaiEngine3D::ShouldUseMeshletPath が実行可否の判定に使う
            bool HasMeshletPipelineState() const { return m_GBufferMeshletPipelineState != nullptr; }
            // 深度プリパスを走らせられるか(通常とカットアウトの両方が要る)
            bool CanRunDepthPrepass() const
            {
                return m_DepthPrepassPipelineState != nullptr && m_DepthPrepassCutoutPipelineState != nullptr;
            }
            // G-Buffer・深度プリパスのシェーダー。**PSOはここでは作らない** ――
            // 出力先のフォーマットがバッファ精度に依存するため
            // CreatePrecisionDependentPipelineStates が作る
            void CreateGeometryShaders(RHI::IRHIDevice& device, const std::wstring& shaderDirectory, bool meshShaderAvailable);
            // バッファ精度が変わるたびに作り直す。**呼び出し元のtry内から呼ぶこと**
            // (確保失敗時のログと投げ直しは KurenaiEngine3D 側が持つ)。
            // meshShaderAvailable が偽ならメッシュレット経路のPSOはnullptrのままになる
            void CreatePrecisionDependentPipelineStates(
                RHI::IRHIDevice& device, RHI::Format emissiveFormat,
                const std::vector<RHI::InputElementDesc>& modelInputLayout);

        private:
            KurenaiEngine3D& m_Engine;

            // G-Bufferパス。頂点シェーダーは水面パスとも共有する
            std::unique_ptr<RHI::IRHIShader> m_GBufferVertexShader;
            std::unique_ptr<RHI::IRHIShader> m_GBufferPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_GBufferPipelineState;
            // 負のスケールを持つインスタンス用にFrontCounterClockwiseを反転したPSO
            std::unique_ptr<RHI::IRHIPipelineState> m_GBufferPipelineStateMirrored;
            // 水面。頂点シェーダーはm_GBufferVertexShaderをそのまま共有する
            std::unique_ptr<RHI::IRHIShader> m_GBufferWaterPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_GBufferWaterPipelineState;
            std::unique_ptr<RHI::IRHIPipelineState> m_GBufferWaterPipelineStateMirrored;

            // --- 深度プリパス(41.22節。Shaders/3D/DepthPrepass.hlsl) ---
            //
            // G-Bufferを描く前に不透明ジオメトリの深度だけを埋め、G-Buffer側の深度比較を
            // GREATER_EQUALにして最前面の断片だけを通す。隠れる画素のピクセルシェーダー
            // (6テクスチャ + 6レンダーターゲット書き込み)がまるごと省ける。
            //
            // 【頂点シェーダーはm_GBufferVertexShaderを共有する】プリパスとG-Bufferで頂点の
            // 変換結果が1ulpでもずれると深度が一致せず、GREATER_EQUALのテストを通らずに
            // その面がまるごと消える。写して2本にすると最適化の差で容易にずれる。
            // 不透明マテリアル用はピクセルシェーダーを持たない(nullptr = 段ごと省く)
            std::unique_ptr<RHI::IRHIShader> m_DepthPrepassCutoutPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_DepthPrepassPipelineState;
            std::unique_ptr<RHI::IRHIPipelineState> m_DepthPrepassPipelineStateMirrored;
            std::unique_ptr<RHI::IRHIPipelineState> m_DepthPrepassCutoutPipelineState;
            std::unique_ptr<RHI::IRHIPipelineState> m_DepthPrepassCutoutPipelineStateMirrored;
            // メッシュシェーダー経路。非対応環境では4本ともnullptrのままで、
            // 描画側は従来のメッシュ単位のループを使う
            std::unique_ptr<RHI::IRHIPipelineState> m_DepthPrepassMeshletPipelineState;
            std::unique_ptr<RHI::IRHIPipelineState> m_DepthPrepassMeshletPipelineStateMirrored;
            std::unique_ptr<RHI::IRHIPipelineState> m_DepthPrepassMeshletCutoutPipelineState;
            std::unique_ptr<RHI::IRHIPipelineState> m_DepthPrepassMeshletCutoutPipelineStateMirrored;

            // --- メッシュシェーダー版のジオメトリパス(Shaders/3D/GBufferMeshlet.hlsl) ---
            //
            // 増幅シェーダーがメッシュレット単位で錐台・法線コーンのカリングを行い、
            // 生き残った塊だけをメッシュシェーダーがラスタライザへ流す。書き込む先も内容も
            // 通常パスとまったく同じG-Bufferで、ピクセルシェーダーも共有している
            // (m_GBufferPixelShader)。そのため切り替えても見た目は一致するのが正しい。
            //
            // 非対応環境(DX11、メッシュシェーダーTier 1未満、bindless非対応)では
            // すべてnullptrのままになり、描画側は自動的に従来経路を使う
            std::unique_ptr<RHI::IRHIShader> m_GBufferAmplificationShader;
            std::unique_ptr<RHI::IRHIShader> m_GBufferMeshShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_GBufferMeshletPipelineState;
            std::unique_ptr<RHI::IRHIPipelineState> m_GBufferMeshletPipelineStateMirrored;
            // メッシュレットのデバッグ表示(塊ごとに色を変える)
            std::unique_ptr<RHI::IRHIShader> m_GBufferMeshletDebugPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_GBufferMeshletDebugPipelineState;
            std::unique_ptr<RHI::IRHIPipelineState> m_GBufferMeshletDebugPipelineStateMirrored;

            // 階層深度(Hi-Z)。深度をミップチェーンへ落としてオクルージョン判定に使う。
            // テクスチャ本体は RenderTargets::HiZTexture が持つ
            std::unique_ptr<RHI::IRHIShader> m_HiZCopyComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_HiZCopyPipelineState;
            std::unique_ptr<RHI::IRHIShader> m_HiZDownsampleComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_HiZDownsamplePipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_HiZConstantBuffer;

            // モデル単位GPUカリングの候補。**この群が持ち主である。**
            // 間接描画のパスを区画ごとに複数登録し、そのExecuteラムダが揃って
            // これを参照捕捉するため、登録関数のローカルにすると寿命が足りない。
            // 中身はフレームごとに clear() して作り直す(容量は使い回す)
            std::vector<ModelCullDrawCandidate> m_ModelCullDraws;
        };
    }
}
