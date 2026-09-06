#pragma once

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

        private:
            KurenaiEngine3D& m_Engine;
        };
    }
}
