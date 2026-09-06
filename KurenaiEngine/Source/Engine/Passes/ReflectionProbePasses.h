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

// 反射プローブのパス群(段階6)。
// ProbeBakeCapture / ProbeBakeConvolvePrefilter / ProbeRealtime*。
//
// 【キャプチャ経路はDDGIと共有】キューブ面の行列は Rendering/CubeFaceMath.h にあり、
// 面ごとの射影(ProbeFaceProjection)と焼き込みに入れる灯の数(BakedLightCount)は
// フレームのスナップショット越しに両者へ配る。
namespace Kurenai
{
    class KurenaiEngine3D;

    namespace Passes
    {
        class ReflectionProbePasses
        {
        public:
            explicit ReflectionProbePasses(KurenaiEngine3D& engine) : m_Engine(engine) {}

            void Register(
                Core::RenderGraph& graph,
                const Rendering::RenderFrameContext& frame,
                const Rendering::RenderBlackboard& bb);

        private:
            KurenaiEngine3D& m_Engine;
        };
    }
}
