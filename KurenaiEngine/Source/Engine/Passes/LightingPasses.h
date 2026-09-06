#pragma once

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

        private:
            KurenaiEngine3D& m_Engine;
        };
    }
}
