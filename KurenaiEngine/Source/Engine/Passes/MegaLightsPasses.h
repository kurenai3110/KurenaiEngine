#pragma once

#include "MegaLightsConstants.h"

namespace Kurenai::Core
{
    class RenderGraph;
}

namespace Kurenai::Rendering
{
    struct RenderFrameContext;
    struct RenderBlackboard;
}

// タイルライトカリングと MegaLights のパス群(段階6)。
// LightCull / MegaLightsPool / MegaLights / MegaLights の確率的サンプリング系14本。
//
// 【デノイズを走らせたかをブラックボードへ載せる】後段の直接光パスが
// 「生出力とデノイズ後のどちらを t7 へ張るか」をこの値で決めるため、bb は非 const で受ける。
namespace Kurenai
{
    class KurenaiEngine3D;

    namespace Passes
    {
        class MegaLightsPasses
        {
        public:
            explicit MegaLightsPasses(KurenaiEngine3D& engine) : m_Engine(engine) {}

            void Register(
                Core::RenderGraph& graph,
                const Rendering::RenderFrameContext& frame,
                Rendering::RenderBlackboard& bb);

        private:
            KurenaiEngine3D& m_Engine;
        };
    }
}
