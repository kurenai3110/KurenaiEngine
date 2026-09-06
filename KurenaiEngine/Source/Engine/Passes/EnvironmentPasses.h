#pragma once

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

        private:
            KurenaiEngine3D& m_Engine;
        };
    }
}
