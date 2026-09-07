#pragma once

#include <memory>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "RHI/IRHIDevice.h"

#include "LightingConstants.h"

namespace Kurenai::Core
{
    class RenderGraph;
}

namespace Kurenai::Rendering
{
    struct RenderFrameContext;
    struct RenderBlackboard;
}

// ライティングのパス群(段階6)。
//
// 【登録の入口が2つに分かれている】直接光〜雲(DirectLight / RTAO / AO / AOBlur / SkyCloud)と、
// 合成(Lighting / Transparent)の間に DDGIResolve が挟まる。登録順は実行順の一部なので
// 寄せられない。それぞれ元の位置から呼ぶ。
namespace Kurenai
{
    class KurenaiEngine3D;

    namespace Passes
    {
        class LightingPasses
        {
        public:
            explicit LightingPasses(KurenaiEngine3D& engine) : m_Engine(engine) {}

            // 直接光とAO/GIと雲。AOの出力先をブラックボードへ載せるので bb は非 const
            void RegisterDirectAndAO(
                Core::RenderGraph& graph,
                const Rendering::RenderFrameContext& frame,
                Rendering::RenderBlackboard& bb);

            // G-Bufferの合成と半透明フォワード
            void RegisterSceneLighting(
                Core::RenderGraph& graph,
                const Rendering::RenderFrameContext& frame,
                const Rendering::RenderBlackboard& bb);

            // 【エンジン側の元の行位置から呼ぶこと】DX12はディスクリプタ枠を生成順に
            // 割り当てるため、生成の呼び出しを寄せ集めると他のリソースとの前後関係が崩れ、
            // パスマニフェストの採取が一斉に不一致になる。所有権だけをこの群へ移し、
            // 呼び出しは元あった場所に残してある。**入口が多いのはそのため**で、
            // まとめて1本にしてはいけない
            void CreateDirectLightPipelineState(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            // AO/GIのシェーダーと定数バッファ。**PSOはここでは作らない** ――
            // 出力先のフォーマットがバッファ精度に依存するため
            // CreatePrecisionDependentPipelineStates が作る
            void CreateAOResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory, uint32_t ssaoKernelSize);
            void CreateLightingPipelineState(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void CreateTransparentPipelineStates(
                RHI::IRHIDevice& device, const std::wstring& shaderDirectory,
                const std::vector<RHI::InputElementDesc>& modelInputLayout);
            void CreateSkyCloudPipelineState(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            // DXR対応環境でのみ呼ばれる。非対応ならnullptrのままで、
            // KurenaiEngine3D::ShouldRunRaytracedAO が false を返し登録自体が起きない
            void CreateRaytracedAOResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void CreateLightingConstantBuffer(RHI::IRHIDevice& device);
            // バッファ精度が変わるたびに作り直す。**呼び出し元のtry内から呼ぶこと**
            // (確保失敗時のログと投げ直しは KurenaiEngine3D 側が持つ)
            void CreatePrecisionDependentPipelineStates(RHI::IRHIDevice& device, RHI::Format aoFormat);

            // ShadowPasses / ReflectionPasses の同名メソッドと同じ役割。
            // KurenaiEngine3D::ShouldRunRaytracedAO が実行可否の判定に使う
            bool HasRaytracedPipelineState() const { return m_RTAOPipelineState != nullptr; }

        private:
            KurenaiEngine3D& m_Engine;

            // 直接光パス(G-Buffer+シャドウマップからPBRの直接光(拡散+鏡面反射、シャドウ適用済み)を
            // 計算しHDRで書き出す。DeferredLightingパスとSSIL_VisibilityBitmask.hlslの両方から
            // サンプルされるため、G-Bufferと同じレンダー解像度・R32G32B32A32_Float(HDR)で保持する)
            std::unique_ptr<RHI::IRHIShader> m_DirectLightVertexShader;
            std::unique_ptr<RHI::IRHIShader> m_DirectLightPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_DirectLightPipelineState;

            // AO/GI共通の頂点シェーダ(フルスクリーン三角形)。SSAO/SSIL/共通ブラーの
            // 3つのピクセルシェーダで使い回す
            std::unique_ptr<RHI::IRHIShader> m_AOVertexShader;
            // AO/GI共通のブラーパス(4x4ボックスブラーでrgba全チャンネルを均す。SSAO/SSIL両方から使い回す)
            std::unique_ptr<RHI::IRHIShader> m_AOBlurPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_AOBlurPipelineState;

            // SSAOパス(G-BufferのNormal/Depthから遮蔽率を計算する。G-Bufferと同じレンダー解像度)
            std::unique_ptr<RHI::IRHIShader> m_SSAOPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_SSAOPipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_SSAOConstantBuffer;
            // カーネルの大きさはUIから変えられる。**変わったらこの群が作り直す**
            // (RegisterDirectAndAO 内で現在の設定と突き合わせている)
            std::vector<DirectX::XMFLOAT4> m_SSAOKernel;

            // SSILパス(Visibility Bitmask): G-BufferのAlbedo/Normal/Depthから遮蔽率と間接拡散光を計算する
            std::unique_ptr<RHI::IRHIShader> m_SSILPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_SSILPipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_SSILConstantBuffer;

            // RTAOパス: 法線周りの半球へ余弦重みでレイを撃ち、遮蔽率と1バウンスの間接拡散光を求める
            // コンピュートパス。出力はSSAO/SSILとまったく同じ意味・同じフォーマットなので、
            // 後段のAOBlurパスとライティングパスは無変更で使い回せる(27章)。
            // 出力テクスチャ(生・ブラー後)はレンダー解像度に追従して作り直すため、
            // 持ち主は RenderTargets(RTAORawTexture / RTAOTexture)側にある
            std::unique_ptr<RHI::IRHIShader> m_RTAOComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_RTAOPipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_RTAOConstantBuffer;

            // ライティングパス(G-Bufferを読みSceneColorへ出力。G-Bufferと同じレンダー解像度)。
            // SceneColorはHDR(R16G16B16A16_Float)で、トーンマッピングは行わない(Tonemapパス参照)
            std::unique_ptr<RHI::IRHIShader> m_LightingVertexShader;
            std::unique_ptr<RHI::IRHIShader> m_LightingPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_LightingPipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_LightingConstantBuffer;

            // 半透明フォワードパス: Deferred(G-Buffer)には書き込まれなかったBLENDマテリアルのメッシュを、
            // Lightingパスの後にSceneColorへ直接フォワードシェーディングしてアルファブレンド合成する
            // (深度テストはGBuffer深度に対して行うが書き込みは行わない)。頂点レイアウトはGBufferパスと
            // 共通(POSITION/NORMAL/TEXCOORD/TANGENT)
            std::unique_ptr<RHI::IRHIShader> m_TransparentVertexShader;
            std::unique_ptr<RHI::IRHIShader> m_TransparentPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_TransparentPipelineState;
            std::unique_ptr<RHI::IRHIPipelineState> m_TransparentPipelineStateMirrored;

            // 雲パス(積雲と巻雲だけを1/2解像度で評価し、透過率と事前乗算済みの散乱光を書く)。
            // 書き先2枚と実寸は、レンダー解像度に追従して作り直すものであり、
            // RenderDumpService(-dumptex)も読むため RenderTargets 側が持つ
            std::unique_ptr<RHI::IRHIShader> m_SkyCloudVertexShader;
            std::unique_ptr<RHI::IRHIShader> m_SkyCloudPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_SkyCloudPipelineState;
        };
    }
}
