
#include <algorithm>
#include <vector>

#include "Core/RenderGraph.h"
#include "ShadowPasses.h"
#include "../Rendering/ShadowConstants.h"
#include "../Rendering/GeometryDrawLoop.h"
#include "../Rendering/ObjectConstants.h"
#include "../Rendering/RenderFrameContext.h"
#include "LightingConstants.h"
#include "../ShaderInterop/CascadeConstants.h"
#include "../ShaderInterop/FrameConstants.h"
#include "../ShaderInterop/GroupSizes.h"

namespace Kurenai::Passes
{
    namespace
    {
        using Rendering::FrustumPlanes;
        using Rendering::ExtractFrustumPlanes;
        using ShaderInterop::CascadeConstants;
        using ShaderInterop::FrameConstants;
    }

    void ShadowPasses::CreateMeshletShaders(RHI::IRHIDevice& device, const std::wstring& shaderDirectory)
    {
        // シャドウパスのメッシュシェーダー版。G-Buffer版と分けているのは、
        // シャドウのb0がFrameConstantsではなくCascadeConstantsで、cbufferの
        // レイアウトが違うため(ShadowMeshlet.hlsl冒頭のコメント参照)
        RHI::ShaderDesc shadowAsDesc;
        shadowAsDesc.Stage = RHI::ShaderStage::Amplification;
        shadowAsDesc.FilePath = shaderDirectory + L"ShadowMeshlet.kshader";
        shadowAsDesc.EntryPoint = "ASMain";
        m_ShadowAmplificationShader = device.CreateShader(shadowAsDesc);

        RHI::ShaderDesc shadowMsDesc;
        shadowMsDesc.Stage = RHI::ShaderStage::Mesh;
        shadowMsDesc.FilePath = shaderDirectory + L"ShadowMeshlet.kshader";
        shadowMsDesc.EntryPoint = "MSMain";
        m_ShadowMeshShader = device.CreateShader(shadowMsDesc);
    }
    void ShadowPasses::CreateRaytracedResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory)
    {
        // RTシャドウパス(コンピュートシェーダー。TLASへ太陽の円盤方向の影レイを撃ち可視率を求める)。
        // RTReflectionと同じくRayQueryを含むためシェーダーモデル6.5が必要
        RHI::ShaderDesc rtShadowCsDesc;
        rtShadowCsDesc.Stage = RHI::ShaderStage::Compute;
        rtShadowCsDesc.FilePath = shaderDirectory + L"RTShadow.kshader";
        rtShadowCsDesc.EntryPoint = "CSMain";
        m_RTShadowComputeShader = device.CreateShader(rtShadowCsDesc);
        m_RTShadowPipelineState = device.CreateComputePipelineState({ m_RTShadowComputeShader.get() });

        RHI::BufferDesc rtShadowConstantBufferDesc;
        rtShadowConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        rtShadowConstantBufferDesc.SizeInBytes = sizeof(RTShadowConstants);
        m_RTShadowConstantBuffer = device.CreateBuffer(rtShadowConstantBufferDesc);
    }
    void ShadowPasses::CreateCascadePipelineStates(RHI::IRHIDevice& device, const std::wstring& shaderDirectory)
    {
        // シャドウパス(ライト視点への深度のみの描画。頂点入力はPOSITIONのみ使用)
        RHI::ShaderDesc shadowVsDesc;
        shadowVsDesc.Stage = RHI::ShaderStage::Vertex;
        shadowVsDesc.FilePath = shaderDirectory + L"Shadow.kshader";
        shadowVsDesc.EntryPoint = "VSMain";
        m_ShadowVertexShader = device.CreateShader(shadowVsDesc);

        RHI::ShaderDesc shadowPsDesc;
        shadowPsDesc.Stage = RHI::ShaderStage::Pixel;
        shadowPsDesc.FilePath = shaderDirectory + L"Shadow.kshader";
        shadowPsDesc.EntryPoint = "PSMain";
        m_ShadowPixelShader = device.CreateShader(shadowPsDesc);

        // アルファカットアウト用。切り抜きを反映しないと、葉や柵のように
        // テクスチャで抜く前提のマテリアルが板ポリゴンのまま影を落とす
        RHI::ShaderDesc shadowCutoutVsDesc;
        shadowCutoutVsDesc.Stage = RHI::ShaderStage::Vertex;
        shadowCutoutVsDesc.FilePath = shaderDirectory + L"Shadow.kshader";
        shadowCutoutVsDesc.EntryPoint = "VSMainCutout";
        m_ShadowCutoutVertexShader = device.CreateShader(shadowCutoutVsDesc);

        RHI::ShaderDesc shadowCutoutPsDesc;
        shadowCutoutPsDesc.Stage = RHI::ShaderStage::Pixel;
        shadowCutoutPsDesc.FilePath = shaderDirectory + L"Shadow.kshader";
        shadowCutoutPsDesc.EntryPoint = "PSMainCutout";
        m_ShadowCutoutPixelShader = device.CreateShader(shadowCutoutPsDesc);

        const std::vector<RHI::InputElementDesc> shadowInputLayout =
        {
            { "POSITION", 0, RHI::Format::R32G32B32_Float, 0 },
        };

        // カットアウトはベースカラーのアルファを引くためUVも要る。
        // オフセット24はAssets::Vertexの並び(Position 0 / Normal 12 / UV 24)から
        const std::vector<RHI::InputElementDesc> shadowCutoutInputLayout =
        {
            { "POSITION", 0, RHI::Format::R32G32B32_Float, 0 },
            { "TEXCOORD", 0, RHI::Format::R32G32_Float, 24 },
        };

        RHI::PipelineStateDesc shadowPipelineDesc;
        shadowPipelineDesc.InputLayout = shadowInputLayout;
        shadowPipelineDesc.VertexShader = m_ShadowVertexShader.get();
        shadowPipelineDesc.PixelShader = m_ShadowPixelShader.get();
        shadowPipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
        shadowPipelineDesc.HasDepthStencil = true;
        m_ShadowPipelineState = device.CreatePipelineState(shadowPipelineDesc);
        // 影も同様に、ミラーリングされたインスタンスは表裏が入れ替わる。放置すると
        // シャドウマップへ内側の面の深度が書かれ、影の形と自己遮蔽の出方がずれる
        shadowPipelineDesc.FrontCounterClockwise = true;
        m_ShadowPipelineStateMirrored = device.CreatePipelineState(shadowPipelineDesc);

        // アルファカットアウト用(頂点シェーダー経路)。切り抜きを反映して深度を書く。
        // 【DX11でも効く】bindlessもメッシュシェーダーも要らないので、両バックエンドで同じ影になる
        if (m_ShadowCutoutVertexShader && m_ShadowCutoutPixelShader)
        {
            RHI::PipelineStateDesc shadowCutoutDesc;
            shadowCutoutDesc.InputLayout = shadowCutoutInputLayout;
            shadowCutoutDesc.VertexShader = m_ShadowCutoutVertexShader.get();
            shadowCutoutDesc.PixelShader = m_ShadowCutoutPixelShader.get();
            shadowCutoutDesc.Topology = RHI::PrimitiveTopology::TriangleList;
            shadowCutoutDesc.HasDepthStencil = true;
            shadowCutoutDesc.FrontCounterClockwise = false;
            m_ShadowCutoutPipelineState = device.CreatePipelineState(shadowCutoutDesc);
            shadowCutoutDesc.FrontCounterClockwise = true;
            m_ShadowCutoutPipelineStateMirrored = device.CreatePipelineState(shadowCutoutDesc);
        }

        // メッシュシェーダー版のシャドウPSO。
        //
        // 【これが無いと1ドロー化が片手落ちになる】メッシュレット経路はG-Bufferにしか
        // 無かったため、モデルを1ドローで描けるようになってもシャドウは従来どおり
        // メッシュ単位で、しかもカスケード4枚ぶん発行され続ける。
        // PLATEAU LOD2の1タイル(メッシュ1,715個)ならG-Bufferが1ドローになる一方で
        // シャドウは6,860ドローのまま、ということになる。
        //
        // ピクセルシェーダーは持たない(深度だけを書く)。頂点シェーダー版が
        // 空のPSMainを渡しているのに合わせず段ごと省いているのは、深度プリパスの
        // 不透明用PSOと同じ理由(RHIDesc.hのPixelShader=nullptrの扱い)
        if (m_ShadowAmplificationShader && m_ShadowMeshShader)
        {
            RHI::MeshPipelineStateDesc shadowMeshDesc;
            shadowMeshDesc.AmplificationShader = m_ShadowAmplificationShader.get();
            shadowMeshDesc.MeshShader = m_ShadowMeshShader.get();
            shadowMeshDesc.PixelShader = nullptr;
            shadowMeshDesc.HasDepthStencil = true;
            shadowMeshDesc.FrontCounterClockwise = false;
            m_ShadowMeshletPipelineState = device.CreateMeshPipelineState(shadowMeshDesc);
            shadowMeshDesc.FrontCounterClockwise = true;
            m_ShadowMeshletPipelineStateMirrored = device.CreateMeshPipelineState(shadowMeshDesc);

            // カットアウト用。ピクセルシェーダーは頂点シェーダー経路と共有する
            // (ShadowMeshlet.hlslのShadowPSInputとShadow.hlslのCutoutPSInputは
            //  同じ並び・同じセマンティクスにしてある)
            if (m_ShadowCutoutPixelShader)
            {
                shadowMeshDesc.PixelShader = m_ShadowCutoutPixelShader.get();
                shadowMeshDesc.FrontCounterClockwise = false;
                m_ShadowMeshletCutoutPipelineState = device.CreateMeshPipelineState(shadowMeshDesc);
                shadowMeshDesc.FrontCounterClockwise = true;
                m_ShadowMeshletCutoutPipelineStateMirrored = device.CreateMeshPipelineState(shadowMeshDesc);
            }
        }
    }
    void ShadowPasses::CreateCascadeConstantBuffer(RHI::IRHIDevice& device)
    {
        // シャドウパスはカスケードごとに異なるビュー・プロジェクション行列で同じメッシュ群を描き直すため、
        // 共有のFrameConstantsとは別に、この1個の行列だけを持つ専用バッファを使い回す
        RHI::BufferDesc cascadeConstantBufferDesc;
        cascadeConstantBufferDesc.Usage = RHI::BufferUsage::Constant;
        cascadeConstantBufferDesc.SizeInBytes = sizeof(CascadeConstants);
        m_ShadowCascadeConstantBuffer = device.CreateBuffer(cascadeConstantBufferDesc);
    }


    void ShadowPasses::RegisterCascades(
        Core::RenderGraph& graph, const Rendering::RenderFrameContext& frame)
    {
        // 【フレームの写しをローカルで受ける】frame自体はラムダへ捕捉しない
        const Rendering::RenderTargets* const targets = frame.Targets;

        // 【フレームの写しをローカルで受ける】frame自体はラムダへ捕捉しない
        RHI::IRHIBuffer* const modelInstanceBuffer = frame.Scene->ModelInstanceBuffer.get();

        // 【フレームの写しをローカルで受ける】ラムダへ値で渡すため
        const MeshletLODFrameConstants meshletLOD = frame.MeshletLOD;

        // 【ラムダへ値で渡すためローカルへ受け直す】frame そのものは捕捉しない作法
        // (Rendering/RenderFrameContext.h の冒頭)。設定は POD なので写しは安い
        const AmbientOcclusionSettings ambientOcclusionSettings = frame.Settings.AmbientOcclusion;
        const EmissiveLightSettings emissiveLightSettings = frame.Settings.EmissiveLight;
        const ShadowSettings shadowSettings = frame.Settings.Shadow;

        RHI::IRHIBuffer* const objectConstantBuffer = frame.ObjectConstantBuffer;
        RHI::IRHISamplerSet* const materialSamplers = frame.MaterialSamplers;

        // 【フレームの値をここで写し取る】以下はRender()から機械的に移した登録コードなので、
        // 参照している名前を変えずに済むよう同じ名前で受け直す。
        //
        // cascadeViewProj はRender()のローカル配列を指す。実体は graph.Execute() が
        // 終わるまで生きているので、ラムダへはポインタを値で捕捉すればよい
        const RHI::Viewport shadowViewport = frame.ShadowViewport;
        const DirectX::XMMATRIX* const cascadeViewProj = frame.CascadeViewProj;

        // --- シャドウパス: ライト視点から深度のみを描画する(常に固定のシャドウマップ解像度)。
        //     カスケードごとに1回ずつ、同じメッシュ群を異なるライト正射影で描き直す ---
        for (uint32_t cascade = 0; cascade < Rendering::kCascadeCount; ++cascade)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "Shadow" + std::to_string(cascade),
                .DepthTarget = targets->ShadowCascadeArray.get(),
                .DepthTargetArraySlice = cascade,
                .Execute = [this, modelInstanceBuffer, meshletLOD, ambientOcclusionSettings, emissiveLightSettings, shadowSettings, shadowViewport, cascade, cascadeViewProj, objectConstantBuffer, materialSamplers](RHI::IRHICommandList* cmd)
                {
                    cmd->SetViewport(shadowViewport);
                    // 深度1.0(最遠)にクリアしておく。無効時はこの後の描画をスキップするため、
                    // シェーダー側は深度比較で常に「影なし」と判定する(ComputeShadowFactor参照)
                    cmd->ClearDepth(1.0f);

                    // RTシャドウ選択時もここは描く。半透明(Transparent.hlsl)と反射プローブの
                    // キャプチャ(ProbeCapture.hlsl)はカメラ視点の可視率テクスチャを使えず、
                    // カスケードシャドウマップを必要とするため(26章)
                    if (shadowSettings.Mode != ShadowMode::Off)
                    {
                        CascadeConstants cascadeConstants{};
                        DirectX::XMStoreFloat4x4(&cascadeConstants.ViewProj, DirectX::XMMatrixTranspose(cascadeViewProj[cascade]));
                        cmd->UpdateBuffer(m_ShadowCascadeConstantBuffer.get(), &cascadeConstants, sizeof(cascadeConstants));

                        cmd->SetPipelineState(m_ShadowPipelineState.get());
                        cmd->SetConstantBuffer(0, m_ShadowCascadeConstantBuffer.get());

                        // ミラーリングされたインスタンスは表裏が入れ替わるため、GBufferパスと同じく
                        // 表裏判定を反転したパイプラインへ切り替える(切り替え時はb0も張り直す)
                        RHI::IRHIPipelineState* currentPipelineState = m_ShadowPipelineState.get();
                        const auto bindShadowPipelineState = [&](RHI::IRHIPipelineState* wanted)
                        {
                            if (!wanted || wanted == currentPipelineState)
                            {
                                return;
                            }
                            cmd->SetPipelineState(wanted);
                            cmd->SetConstantBuffer(0, m_ShadowCascadeConstantBuffer.get());
                            // カットアウトのピクセルシェーダーがベースカラーを引くためサンプラーが要る。
                            // 不透明用のPSOはピクセルシェーダーを持たないので無害
                            cmd->SetSamplerSet(materialSamplers);
                            currentPipelineState = wanted;
                        };
                        // アルファカットアウトのマテリアルは切り抜きを反映して深度を書く。
                        // PSOが作れていない場合は従来どおり切り抜きを見ない(影が板のままになる)
                        const auto selectShadowPipelineState = [&](bool mirrored, bool cutout)
                        {
                            if (cutout && m_ShadowCutoutPipelineState)
                            {
                                return mirrored ? m_ShadowCutoutPipelineStateMirrored.get()
                                                : m_ShadowCutoutPipelineState.get();
                            }
                            return mirrored ? m_ShadowPipelineStateMirrored.get() : m_ShadowPipelineState.get();
                        };

                        // このカスケードのライト正射影に対して視錐台カリングする。
                        // カメラではなくライト側の錐台なので、画面外でも影を落とすものは残る
                        const FrustumPlanes cascadeFrustum = ExtractFrustumPlanes(cascadeViewProj[cascade]);

                        // インスタンシングのバッチと、まとめられなかった1体を同じ形で回す。
                        // 【影は常に最も粗い段】影はテクスチャを読まないので詳細な段を描く
                        // 意味が無い。ストリーミング中で未読み込みなら描かない。
                        // バッチはどの段を描くかを既に決めてある(全員が同じ段であることが
                        // バッチの条件そのもの)
                        Rendering::GeometryDrawLoopDesc shadowLoop;
                        shadowLoop.Frustum = &cascadeFrustum;
                        shadowLoop.LODMode = Rendering::GeometryLODMode::Coarsest;
                        shadowLoop.MeshFilter = Rendering::GeometryMeshFilter::Opaque;

                        Rendering::ForEachGeometryDraw(
                m_Engine.MakeGeometryDrawHost(),
                            shadowLoop,
                            [&](const Rendering::InstanceDrawUnit& unit, const Assets::Model& coarsestModel, float)
                            {
                                const Assets::ModelInstance& instance = *unit.Instance;

                                // G-Bufferが1ドローで描くモデルは、シャドウも1ドローで描く。
                                //
                                // 半透明(BLEND)は影を落とさない。頂点シェーダー経路は
                                // MeshFilter=Opaqueで、メッシュレット経路は下のrejectMaskで同じことを行う。
                                // カットアウト(MASK)だけは従来どおり切り抜きを反映して影を描く。
                                //
                                // 【カットアウトを持つモデルだけ2回に分ける】不透明ぶんは
                                // ピクセルシェーダーを持たないPSOで描きたいので、
                                // 切り抜きが要るぶんとは同じドローにまとめられない
                                if (!m_ShadowMeshletPipelineState
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
                                        instance, coarsestModel, emissiveLightSettings.Intensity, ambientOcclusionSettings.OcclusionMapEnabled, rejectMask,
                                        requireMask, meshletLOD);
                                    cmd->UpdateBuffer(
                                        objectConstantBuffer, &objectConstants, sizeof(objectConstants));
                                    cmd->SetConstantBuffer(1, objectConstantBuffer);
                                    cmd->DispatchMesh(groupCount, 1, 1);
                                    ++m_DrawCallsShadow;
                                };

                                // カットアウト用のPSOが作れていない場合は、従来どおり
                                // 切り抜きを見ずに全部を1回で描く(影が板のままになる)
                                const bool splitCutout =
                                    coarsestModel.HasCutoutMaterial && m_ShadowMeshletCutoutPipelineState;

                                dispatchShadowMeshlets(
                                    instance.IsMirrored ? m_ShadowMeshletPipelineStateMirrored.get()
                                                        : m_ShadowMeshletPipelineState.get(),
                                    Assets::kGpuMaterialFlagTransparent
                                        | (splitCutout ? Assets::kGpuMaterialFlagCutout : 0u),
                                    0u);

                                if (splitCutout)
                                {
                                    dispatchShadowMeshlets(
                                        instance.IsMirrored ? m_ShadowMeshletCutoutPipelineStateMirrored.get()
                                                            : m_ShadowMeshletCutoutPipelineState.get(),
                                        Assets::kGpuMaterialFlagTransparent,
                                        Assets::kGpuMaterialFlagCutout);
                                }
                                return true;
                            },
                            [&](const Rendering::InstanceDrawUnit& unit, const Assets::Model& coarsestModel,
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
                                    MakeObjectConstants(instance, coarsestModel, mesh, emissiveLightSettings.Intensity, ambientOcclusionSettings.OcclusionMapEnabled, meshletLOD);
                                objectConstants.InstanceBase = unit.InstanceBase;
                                objectConstants.InstancingEnabled = unit.IsBatch() ? 1u : 0u;
                                cmd->UpdateBuffer(objectConstantBuffer, &objectConstants, sizeof(objectConstants));
                                cmd->SetConstantBuffer(1, objectConstantBuffer);

                                if (cutout)
                                {
                                    cmd->SetTexture(0, mesh.BaseColorTexture);
                                }

                                // 【毎回張り直す】頂点シェーダー用SRVはt0の1本しかなく、
                                // ドローンショーが同じスロットを使う。上書きされたまま描くと
                                // 全インスタンスがドローンの座標を行列として読んで画面外へ飛ぶ
                                if (unit.IsBatch())
                                {
                                    cmd->SetVertexShaderResourceBuffer(0, modelInstanceBuffer);
                                }

                                cmd->SetVertexBuffer(mesh.VertexBuffer.get());
                                cmd->SetIndexBuffer(mesh.IndexBuffer.get());
                                cmd->DrawIndexed(mesh.IndexCount, 0, 0, unit.InstanceCount);
                                ++m_DrawCallsShadow;
                                return true;
                            });
                    }
                },
            });
        }
    }

    void ShadowPasses::RegisterRaytraced(
        Core::RenderGraph& graph, const Rendering::RenderFrameContext& frame)
    {
        // 【フレームの写しをローカルで受ける】frame自体はラムダへ捕捉しない
        const Rendering::RenderTargets* const targets = frame.Targets;

        // 【フレームの写しをローカルで受ける】frame自体はラムダへ捕捉しない
        const Assets::RaytracingScene* const raytracingScene = &frame.Scene->RaytracingScene;

        // 【述語の結果はフレームの写しから引く】判定そのものは Should* が唯一の実装で、
        // ここで作り直さない。ラムダへ値で渡すためローカルで受ける
        const bool raytracedShadowRuns = frame.RaytracedShadowRuns;

        // 【ラムダへ値で渡すためローカルへ受け直す】frame そのものは捕捉しない作法
        // (Rendering/RenderFrameContext.h の冒頭)。設定は POD なので写しは安い
        const ShadowSettings shadowSettings = frame.Settings.Shadow;

        const uint32_t renderWidth = frame.RenderWidth;
        const uint32_t renderHeight = frame.RenderHeight;
        RHI::IRHIBuffer* const frameConstantBuffer = frame.FrameConstantBuffer;

        // 【フレームの値をここで写し取る】Render()から機械的に移した登録コードなので、
        // 参照している名前を変えずに済むよう同じ名前で受け直す
        const FrameConstants& constants = *frame.Constants;

        // --- RTシャドウパス: TLASへ太陽の見かけの円盤方向へ影レイを撃ち、可視率(0〜1)を
        //     単チャンネルのテクスチャへ書く。直後の直接光パスがt6でこれを読む ---
        if (raytracedShadowRuns)
        {
            graph.AddPass(Core::RenderGraphPassDesc{
                .Name = "RTShadow",
                .Reads = { targets->GBufferNormal.get(), targets->GBufferDepth.get() },
                .Writes = { targets->RTShadowTexture.get() },
                .Execute = [this, targets, raytracingScene, shadowSettings, renderWidth, renderHeight, frameConstantBuffer](RHI::IRHICommandList* cmd)
                {
                    Passes::RTShadowConstants rtShadowConstants{};
                    rtShadowConstants.Params0 =
                    {
                        static_cast<float>(renderWidth),
                        static_cast<float>(renderHeight),
                        DirectX::XMConvertToRadians(shadowSettings.RTSunAngularRadiusDegrees),
                        static_cast<float>(std::max(1, shadowSettings.RTSampleCount)),
                    };
                    cmd->UpdateBuffer(m_RTShadowConstantBuffer.get(), &rtShadowConstants, sizeof(rtShadowConstants));

                    cmd->SetComputePipelineState(m_RTShadowPipelineState.get());
                    cmd->SetComputeConstantBuffer(0, frameConstantBuffer);
                    cmd->SetComputeConstantBuffer(1, m_RTShadowConstantBuffer.get());

                    // レジスタ割り当てはRTShadow.hlsl側の宣言と一致させること。
                    // このシェーダはLoad(整数座標)しか使わないためサンプラーはバインドしない
                    cmd->SetComputeAccelerationStructure(0, raytracingScene->GetTopLevelAS());
                    cmd->SetComputeTexture(1, targets->GBufferNormal.get());
                    cmd->SetComputeTexture(2, targets->GBufferDepth.get());

                    // UAVはDispatch直後に解除されるため毎回バインドし直す(IRHICommandList.h参照)
                    cmd->SetComputeUnorderedAccessTexture(0, targets->RTShadowTexture.get());
                    cmd->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);
                },
            });
        }
    }
}
