#include "../KurenaiEngine3D.h"

#include <algorithm>

#include "Core/RenderGraph.h"
#include "ShadowPasses.h"
#include "../Rendering/GeometryDrawLoop.h"
#include "../Rendering/ObjectConstants.h"
#include "../Rendering/RenderFrameContext.h"
#include "../ShaderInterop/CascadeConstants.h"
#include "../ShaderInterop/GroupSizes.h"

namespace Kurenai::Passes
{
    namespace
    {
        using Rendering::FrustumPlanes;
        using Rendering::ExtractFrustumPlanes;
        using ShaderInterop::CascadeConstants;
    }

    void ShadowPasses::RegisterCascades(
        Core::RenderGraph& graph, const Rendering::RenderFrameContext& frame)
    {
        // 【フレームの値をここで写し取る】以下はRender()から機械的に移した登録コードなので、
        // 参照している名前を変えずに済むよう同じ名前で受け直す。
        //
        // cascadeViewProj はRender()のローカル配列を指す。実体は graph.Execute() が
        // 終わるまで生きているので、ラムダへはポインタを値で捕捉すればよい
        const RHI::Viewport shadowViewport = frame.ShadowViewport;
        const DirectX::XMMATRIX* const cascadeViewProj = frame.CascadeViewProj;

        // --- シャドウパス: ライト視点から深度のみを描画する(常に固定のシャドウマップ解像度)。
        //     カスケードごとに1回ずつ、同じメッシュ群を異なるライト正射影で描き直す ---
        for (uint32_t cascade = 0; cascade < KurenaiEngine3D::kCascadeCount; ++cascade)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "Shadow" + std::to_string(cascade),
                .DepthTarget = m_Engine.m_ShadowCascadeArray.get(),
                .DepthTargetArraySlice = cascade,
                .Execute = [this, shadowViewport, cascade, cascadeViewProj](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(shadowViewport);
                    // 深度1.0(最遠)にクリアしておく。無効時はこの後の描画をスキップするため、
                    // シェーダー側は深度比較で常に「影なし」と判定する(ComputeShadowFactor参照)
                    cmd->ClearDepth(1.0f);

                    // RTシャドウ選択時もここは描く。半透明(Transparent.hlsl)と反射プローブの
                    // キャプチャ(ProbeCapture.hlsl)はカメラ視点の可視率テクスチャを使えず、
                    // カスケードシャドウマップを必要とするため(26章)
                    if (m_Engine.m_ShadowSettings.Mode != ShadowMode::Off)
                    {
                        CascadeConstants cascadeConstants{};
                        DirectX::XMStoreFloat4x4(&cascadeConstants.ViewProj, DirectX::XMMatrixTranspose(cascadeViewProj[cascade]));
                        cmd->UpdateBuffer(m_Engine.m_ShadowCascadeConstantBuffer.get(), &cascadeConstants, sizeof(cascadeConstants));

                        cmd->SetPipelineState(m_Engine.m_ShadowPipelineState.get());
                        cmd->SetConstantBuffer(0, m_Engine.m_ShadowCascadeConstantBuffer.get());

                        // ミラーリングされたインスタンスは表裏が入れ替わるため、GBufferパスと同じく
                        // 表裏判定を反転したパイプラインへ切り替える(切り替え時はb0も張り直す)
                        RHI::IRHIPipelineState* currentPipelineState = m_Engine.m_ShadowPipelineState.get();
                        const auto bindShadowPipelineState = [&](RHI::IRHIPipelineState* wanted)
                        {
                            if (!wanted || wanted == currentPipelineState)
                            {
                                return;
                            }
                            cmd->SetPipelineState(wanted);
                            cmd->SetConstantBuffer(0, m_Engine.m_ShadowCascadeConstantBuffer.get());
                            // カットアウトのピクセルシェーダーがベースカラーを引くためサンプラーが要る。
                            // 不透明用のPSOはピクセルシェーダーを持たないので無害
                            cmd->SetSamplerSet(m_Engine.m_MaterialSamplers.get());
                            currentPipelineState = wanted;
                        };
                        // アルファカットアウトのマテリアルは切り抜きを反映して深度を書く。
                        // PSOが作れていない場合は従来どおり切り抜きを見ない(影が板のままになる)
                        const auto selectShadowPipelineState = [&](bool mirrored, bool cutout)
                        {
                            if (cutout && m_Engine.m_ShadowCutoutPipelineState)
                            {
                                return mirrored ? m_Engine.m_ShadowCutoutPipelineStateMirrored.get()
                                                : m_Engine.m_ShadowCutoutPipelineState.get();
                            }
                            return mirrored ? m_Engine.m_ShadowPipelineStateMirrored.get() : m_Engine.m_ShadowPipelineState.get();
                        };

                        // このカスケードのライト正射影に対して視錐台カリングする。
                        // カメラではなくライト側の錐台なので、画面外でも影を落とすものは残る
                        const FrustumPlanes cascadeFrustum = ExtractFrustumPlanes(cascadeViewProj[cascade]);

                        // インスタンシングのバッチと、まとめられなかった1体を同じ形で回す。
                        // 【影は常に最も粗い段】影はテクスチャを読まないので詳細な段を描く
                        // 意味が無い。ストリーミング中で未読み込みなら描かない。
                        // バッチはどの段を描くかを既に決めてある(全員が同じ段であることが
                        // バッチの条件そのもの)
                        KurenaiEngine3D::GeometryDrawLoopDesc shadowLoop;
                        shadowLoop.Frustum = &cascadeFrustum;
                        shadowLoop.LODMode = KurenaiEngine3D::GeometryLODMode::Coarsest;
                        shadowLoop.MeshFilter = KurenaiEngine3D::GeometryMeshFilter::All;

                        m_Engine.ForEachGeometryDraw(
                            shadowLoop,
                            [&](const KurenaiEngine3D::InstanceDrawUnit& unit, const Assets::Model& coarsestModel, float)
                            {
                                const Assets::ModelInstance& instance = *unit.Instance;

                                // G-Bufferが1ドローで描くモデルは、シャドウも1ドローで描く。
                                //
                                // 【半透明は落とさない】このパスは従来から、BLENDのメッシュも
                                // 実体のまま影を落としている。ここでふるい分けると影の出方が変わって
                                // しまうため、意図的に何も落とさない(カットアウトの切り抜きだけは
                                // 下で反映する ―― そちらは板ポリゴンの影が出る明確な不具合だった)。
                                //
                                // 【カットアウトを持つモデルだけ2回に分ける】不透明ぶんは
                                // ピクセルシェーダーを持たないPSOで描きたいので、
                                // 切り抜きが要るぶんとは同じドローにまとめられない
                                if (!m_Engine.m_ShadowMeshletPipelineState
                                    || !m_Engine.ShouldUseModelMeshletPath(instance, coarsestModel))
                                {
                                    return false;
                                }

                                const uint32_t groupCount =
                                    (coarsestModel.TotalMeshletCount
                                     + ShaderInterop::kAmplificationGroupSize - 1)
                                    / ShaderInterop::kAmplificationGroupSize;

                                const auto dispatchShadowMeshlets =
                                    [&](RHI::IRHIPipelineState* pipelineState, uint32_t rejectMask,
                                        uint32_t requireMask)
                                {
                                    if (!pipelineState)
                                    {
                                        return;
                                    }
                                    bindShadowPipelineState(pipelineState);

                                    const ObjectConstants objectConstants = MakeModelObjectConstants(
                                        instance, coarsestModel, m_Engine.m_EmissiveLightSettings.Intensity, m_Engine.m_AmbientOcclusionSettings.OcclusionMapEnabled, rejectMask,
                                        requireMask, m_Engine.m_MeshletLODFrame);
                                    cmd->UpdateBuffer(
                                        m_Engine.m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                                    cmd->SetConstantBuffer(1, m_Engine.m_ObjectConstantBuffer.get());
                                    cmd->DispatchMesh(groupCount, 1, 1);
                                    ++m_Engine.m_DrawCallsShadow;
                                };

                                // カットアウト用のPSOが作れていない場合は、従来どおり
                                // 切り抜きを見ずに全部を1回で描く(影が板のままになる)
                                const bool splitCutout =
                                    coarsestModel.HasCutoutMaterial && m_Engine.m_ShadowMeshletCutoutPipelineState;

                                dispatchShadowMeshlets(
                                    instance.IsMirrored ? m_Engine.m_ShadowMeshletPipelineStateMirrored.get()
                                                        : m_Engine.m_ShadowMeshletPipelineState.get(),
                                    splitCutout ? Assets::kGpuMaterialFlagCutout : 0u, 0u);

                                if (splitCutout)
                                {
                                    dispatchShadowMeshlets(
                                        instance.IsMirrored ? m_Engine.m_ShadowMeshletCutoutPipelineStateMirrored.get()
                                                            : m_Engine.m_ShadowMeshletCutoutPipelineState.get(),
                                        0u, Assets::kGpuMaterialFlagCutout);
                                }
                                return true;
                            },
                            [&](const KurenaiEngine3D::InstanceDrawUnit& unit, const Assets::Model& coarsestModel,
                                const Assets::Mesh& mesh, float)
                            {
                                const Assets::ModelInstance& instance = *unit.Instance;

                                // アルファカットアウトは切り抜きを反映して深度を書く。
                                // 見ないままだと、葉や柵のようにテクスチャで抜く前提の
                                // マテリアルが板ポリゴンのまま影を落とす
                                const bool cutout = mesh.AlphaCutoff > 0.0f;
                                bindShadowPipelineState(selectShadowPipelineState(instance.IsMirrored, cutout));

                                // シャドウパスはWorld以外を使わないが、GBufferパスと同じルートシグネチャ/
                                // 定数バッファ(b1)を共有しているため必ずバインドする必要がある
                                ObjectConstants objectConstants =
                                    MakeObjectConstants(instance, coarsestModel, mesh, m_Engine.m_EmissiveLightSettings.Intensity, m_Engine.m_AmbientOcclusionSettings.OcclusionMapEnabled, m_Engine.m_MeshletLODFrame);
                                objectConstants.InstanceBase = unit.InstanceBase;
                                objectConstants.InstancingEnabled = unit.IsBatch() ? 1u : 0u;
                                cmd->UpdateBuffer(m_Engine.m_ObjectConstantBuffer.get(), &objectConstants, sizeof(objectConstants));
                                cmd->SetConstantBuffer(1, m_Engine.m_ObjectConstantBuffer.get());

                                if (cutout)
                                {
                                    cmd->SetTexture(0, mesh.BaseColorTexture);
                                }

                                // 【毎回張り直す】頂点シェーダー用SRVはt0の1本しかなく、
                                // ドローンショーが同じスロットを使う。上書きされたまま描くと
                                // 全インスタンスがドローンの座標を行列として読んで画面外へ飛ぶ
                                if (unit.IsBatch())
                                {
                                    cmd->SetVertexShaderResourceBuffer(0, m_Engine.m_ModelInstanceBuffer.get());
                                }

                                cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                                cmd->SetIndexBuffer(mesh.IndexBuffer.get());
                                cmd->DrawIndexed(mesh.IndexCount, 0, 0, unit.InstanceCount);
                                ++m_Engine.m_DrawCallsShadow;
                                return true;
                            });
                    }
                },
            });
        }
    }
}
