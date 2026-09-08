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

            void CreateModelCullResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            // 生成に失敗したときに呼ぶ。カリングは無効のままラスタ経路で動く
            void ResetModelCullResources();
            // 候補数に足りる大きさのバッファを用意する。**足りていれば何もしない**。
            // 失敗しても致命的ではない(このフレームはGPUカリングを使わないだけ)
            void EnsureModelCullCapacity(RHI::IRHIDevice& device, uint32_t candidateCount);
            // 間接描画で1区画ぶんを発行する。区画が空、またはPSOが無ければ何もせずfalseを返す。
            // currentPipelineState は呼び出し側のPSOキャッシュで、切り替えたら書き換える
            bool IssueModelCullIndirect(
                RHI::IRHICommandList* cmd, uint32_t region, RHI::IRHIPipelineState* pipelineState,
                RHI::IRHIPipelineState*& currentPipelineState, RHI::IRHIBuffer* frameConstantBuffer,
                RHI::IRHISamplerSet* materialSamplers);

            // 【publicにしてある】GPUカリングの結果を読み戻すのはエンジンのRender()で、
            // そこが「今フレームCPUが何を数えたか」を比較の相手として要る
            uint32_t GetModelCullCandidateCount() const { return m_ModelCullCandidateCount; }
            uint32_t GetModelCullPrepassCandidateCount() const { return m_ModelCullPrepassCandidateCount; }
            uint32_t GetModelCullCpuFrustumCulled() const { return m_ModelCullCpuFrustumCulled; }
            // 読み戻した値をログへ出すのは Diagnostics/RenderDumpService.cpp で、
            // 「どの経路で描いたか」と「判定を何件ずつに分けたか」を添える
            bool WasModelCullIndirectActiveLastFrame() const { return m_ModelCullIndirectActiveLastFrame; }
            uint32_t GetModelCullDispatchCount(uint32_t index) const { return m_ModelCullDispatchCounts[index]; }

        private:
            KurenaiEngine3D& m_Engine;

            // モデル単位のGPUカリング(ModelCull.hlsl)。増幅シェーダーへ渡す候補を
            // コンピュートで間引き、生き残りだけをExecuteIndirectで描く
            std::unique_ptr<RHI::IRHIShader> m_ModelCullComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_ModelCullPipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_ModelCullConstantBuffer;
            // 候補の配列。毎フレームCPUから書き直すのでStructuredReadOnly
            std::unique_ptr<RHI::IRHIBuffer> m_ModelCullInstanceBuffer;
            // [判定, 視錐台で間引き, オクルージョンで間引き, 生き残り] + 区画ごとの発行数。
            // 読み戻すのは KurenaiEngine3D::Render()(受け皿のリングはそちらが持つ)。
            //
            // 【前の4つはモデル数】数えるのはG-Bufferぶんの候補だけで、そこは1モデル1件になる
            // (m_ModelCullPrepassCandidateCount のコメント参照)。深度プリパスぶんも数えると
            // 1モデルを2回数えてしまい、CPU側の判定と単位が合わなくなる
            std::unique_ptr<RHI::IRHIBuffer> m_ModelCullCounterBuffer;
            // ExecuteIndirectへそのまま渡すバッファ。先頭に区画ごとの発行数が並び、
            // kModelCullArgsBaseOffset から先が区画ごとの引数配列
            std::unique_ptr<RHI::IRHIBuffer> m_ModelCullDrawArgsBuffer;
            // 区画1つぶんのバイト数(ComputeModelCullRegionStride)。描画パスが
            // 自分の区画の先頭オフセットを求めるのに使う
            uint32_t m_ModelCullRegionStride = 0;
            // 区画ごとの候補数。ExecuteIndirectへ渡すmaxCommandCount(GPUが書く発行数の上限)
            uint32_t m_ModelCullRegionCandidates[kModelCullRegionCount]{};
            // GPUへ載せる直前の候補配列。毎フレームの確保を避けるため使い回す
            std::vector<GpuModelCullInstance> m_ModelCullUploadScratch;
            // m_ModelCullInstanceBuffer / m_ModelCullDrawArgsBuffer が収まる候補数。
            // 1インスタンスがLODのクロスディザで最大2件の候補を出すため、インスタンス数の2倍で確保する
            uint32_t m_ModelCullCapacity = 0;
            // このフレームにCPUが積んだ候補数(プリパスぶん + G-Bufferぶん)
            uint32_t m_ModelCullCandidateCount = 0;
            // そのうち深度プリパスぶんの数。候補配列の前半を占め、G-Bufferぶんが後半に続く。
            //
            // 【この境目が2つの役目を持つ】判定を2回に分けるときの区切りであり、
            // 統計を数え始める位置でもある。**統計はG-Bufferぶんだけで数える** ――
            // 両方数えると1モデルを2回数え、CPU側の数と単位が合わなくなる
            uint32_t m_ModelCullPrepassCandidateCount = 0;
            // 同じフレームでCPU側が視錐台で間引いた数。GPUの「視錐台で間引き」と突き合わせる。
            // 突き合わせのためにこの値を遅らせて積むリングは KurenaiEngine3D 側にある
            // (GPUの数値が2フレーム遅れて返るため)
            uint32_t m_ModelCullCpuFrustumCulled = 0;
            // 判定を2回に分けたときの、それぞれが受け持った候補数(プリパスぶん / G-Bufferぶん)
            uint32_t m_ModelCullDispatchCounts[2]{};
            // 上の値がどの経路のものか。ログで「間接描画で描いた」と「数えただけ」を区別する
            bool m_ModelCullIndirectActiveLastFrame = false;

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
