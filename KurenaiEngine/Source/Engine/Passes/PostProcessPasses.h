#pragma once

#include "PostProcessConstants.h"

namespace Kurenai::Core
{
    class RenderGraph;
}

namespace Kurenai::RHI
{
    class IRHICommandList;
}

namespace Kurenai::Rendering
{
    struct RenderFrameContext;
    struct RenderBlackboard;
}

// ポストプロセスのパス群(段階6)。
// 大気遠近 / ドローショー / TAA / 自動露出 / ブルーム / トーンマップ / 超解像(EASU・RCAS)。
//
// 【この群が Blackboard へ書く】TAA の蓄積結果を指す HdrSceneColor と、
// 超解像を走らせたかの UpscaleActive は、後ろの Present が読む。
// そのため bb は非 const で受ける(読むだけの群は const& で受けること)。
namespace Kurenai
{
    class KurenaiEngine3D;

    namespace Passes
    {
        class PostProcessPasses
        {
        public:
            explicit PostProcessPasses(KurenaiEngine3D& engine) : m_Engine(engine) {}

            void Register(
                Core::RenderGraph& graph,
                const Rendering::RenderFrameContext& frame,
                Rendering::RenderBlackboard& bb);

        private:
            KurenaiEngine3D& m_Engine;
        };
    }
}
