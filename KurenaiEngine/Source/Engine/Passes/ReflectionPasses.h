#pragma once

#include <memory>
#include <string>
#include <vector>

#include "RHI/IRHIDevice.h"

#include "PostProcessConstants.h"
#include "ReflectionConstants.h"

namespace Kurenai::Core
{
    class RenderGraph;
}

namespace Kurenai::Rendering
{
    struct RenderFrameContext;
    struct RenderBlackboard;
}

// 反射のパス群(段階6)。PlanarReflection / SSR / RTReflection。
//
// 【3本のうち走るのは高々1本】平面反射は手法がScreenSpaceのときだけ、
// RT反射はDXR対応環境のときだけ登録される。どれも走らないこともある。
//
// 【平面反射はドローンショーの機体も描く】水面へ編隊を映すため、
// PostProcessConstants.h の DroneShowConstants をここでも使う。
namespace Kurenai
{
    class KurenaiEngine3D;

    namespace Passes
    {
        class ReflectionPasses
        {
        public:
            explicit ReflectionPasses(KurenaiEngine3D& engine) : m_Engine(engine) {}

            void Register(
                Core::RenderGraph& graph,
                const Rendering::RenderFrameContext& frame,
                const Rendering::RenderBlackboard& bb);

            // 【エンジン側の元の行位置から呼ぶこと】DX12はディスクリプタ枠を生成順に
            // 割り当てるため、生成の呼び出しを寄せ集めると他のリソースとの前後関係が崩れ、
            // パスマニフェストの採取が一斉に不一致になる。所有権だけをこの群へ移し、
            // 呼び出しは CreateSceneResources() の元あった場所に残してある。
            // 3本の登録位置が離れているのと同じ理由で、生成の入口も3本に分けてある
            void CreateSSRPipelineState(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            // DXR対応環境でのみ呼ばれる。非対応なら全メンバがnullptrのままで、
            // KurenaiEngine3D::ShouldRunRaytracedReflectionがfalseを返して登録自体が起きない
            void CreateRaytracedResources(RHI::IRHIDevice& device, const std::wstring& shaderDirectory);
            void CreatePlanarPipelineStates(
                RHI::IRHIDevice& device, const std::wstring& shaderDirectory,
                const std::vector<RHI::InputElementDesc>& modelInputLayout);

            // ShadowPasses::HasRaytracedPipelineStateと同じ役割。
            // KurenaiEngine3D::ShouldRunRaytracedReflectionが実行可否の判定に使う
            bool HasRaytracedPipelineState() const { return m_RTReflectionPipelineState != nullptr; }

        private:
            KurenaiEngine3D& m_Engine;

            // SSR(Screen Space Reflections)パス: LightingパスのSceneColorを反射先の環境色として
            // 再利用し、G-Buffer(Normal/Material/Depth)からワールド空間でレイマーチングして
            // 鏡面反射を加算する。無効時はこのパスをスキップし、Presentが直接
            // RenderTargets::SceneColorを参照する
            std::unique_ptr<RHI::IRHIShader> m_SSRVertexShader;
            std::unique_ptr<RHI::IRHIShader> m_SSRPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_SSRPipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_SSRConstantBuffer;

            // RT反射パス: TLASへ鏡面レイを撃ち、ヒット面を陰影計算して反射色を求めるコンピュートパス。
            // 出力はSSRと同じ「SceneColor + 反射の差し替え」なので、後段(Tonemap)から見ると
            // RenderTargets::SSRTextureと完全に等価な入れ替え可能なバッファになる。
            // 出力テクスチャはレンダー解像度に追従して作り直すため、持ち主は
            // RenderTargets(RTReflectionTexture)側にある
            std::unique_ptr<RHI::IRHIShader> m_RTReflectionComputeShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_RTReflectionPipelineState;
            std::unique_ptr<RHI::IRHIBuffer> m_RTReflectionConstantBuffer;

            // --- 平面反射 ---
            // 水面に不透明ジオメトリの鏡像を映す専用フォワードパス。設計判断の詳細は
            // Shaders/3D/PlanarReflection.hlsl冒頭のコメントを参照。反射解像度はレンダー解像度に
            // m_ReflectionSettings.PlanarResolutionScaleを掛けた値で、レンダーターゲット2枚と
            // 実寸はPresentPassのデバッグ表示も読むためRenderTargets側が持つ
            std::unique_ptr<RHI::IRHIShader> m_PlanarReflectionVertexShader;
            std::unique_ptr<RHI::IRHIShader> m_PlanarReflectionPixelShader;
            std::unique_ptr<RHI::IRHIPipelineState> m_PlanarReflectionPipelineState;
            // 鏡映カメラで描くとワインディングが全反転するため、m_GBufferPipelineStateMirroredと同じ
            // 仕組み(FrontCounterClockwiseの反転)で吸収する。ただし選択条件はinstance.IsMirroredの
            // 否定になる(Registerのbind時のラムダ参照)
            std::unique_ptr<RHI::IRHIPipelineState> m_PlanarReflectionPipelineStateMirrored;
            // captureProbeFaceと同じ役割の専用FrameConstants(共有のm_FrameConstantBufferとは別インスタンス)。
            // ViewProj/CameraPosition/PlanarReflectionPlaneだけをこのパス用に差し替える
            std::unique_ptr<RHI::IRHIBuffer> m_PlanarReflectionConstantBuffer;
        };
    }
}
