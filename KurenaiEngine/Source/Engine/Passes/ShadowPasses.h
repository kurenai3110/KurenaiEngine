#pragma once

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
            explicit ShadowPasses(KurenaiEngine3D& engine) : m_Engine(engine) {}

            // カスケードシャドウマップ(Shadow0..Shadow3)
            void RegisterCascades(Core::RenderGraph& graph, const Rendering::RenderFrameContext& frame);

        private:
            KurenaiEngine3D& m_Engine;
        };
    }
}
