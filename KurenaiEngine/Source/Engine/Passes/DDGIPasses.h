#pragma once

#include "DDGIConstants.h"

namespace Kurenai::Core
{
    class RenderGraph;
}

namespace Kurenai::Rendering
{
    struct RenderFrameContext;
    struct RenderBlackboard;
}

// DDGI のパス群(段階6)。
//
// 【登録位置が2箇所に離れている】プローブの捕捉と更新(DDGIInvalidate / DDGIUpdate)は
// グラフの前寄り、格子から画面へ解決する DDGIResolve は AO の後ろに登録される。
// 登録順は実行順の一部なので寄せられない。したがって登録の入口を分ける。
//
// 【キャプチャ経路は反射プローブと共有】キューブ面の行列は Rendering/CubeFaceMath.h、
// 面の射影・灯の数・読むテクスチャ一式はフレームのスナップショットから受け取る。
namespace Kurenai
{
    class KurenaiEngine3D;

    namespace Passes
    {
        class DDGIPasses
        {
        public:
            explicit DDGIPasses(KurenaiEngine3D& engine) : m_Engine(engine) {}

            // プローブの捕捉と更新(DDGIInvalidate / DDGIUpdate<probe>)
            void RegisterProbeUpdate(
                Core::RenderGraph& graph,
                const Rendering::RenderFrameContext& frame,
                const Rendering::RenderBlackboard& bb);

        private:
            KurenaiEngine3D& m_Engine;
        };
    }
}
