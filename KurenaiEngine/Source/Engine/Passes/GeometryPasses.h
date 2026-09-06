#pragma once

#include <vector>

#include "GeometryConstants.h"

namespace Kurenai::Core
{
    class RenderGraph;
}

namespace Kurenai::Rendering
{
    struct RenderFrameContext;
    struct RenderBlackboard;
}

// ジオメトリのパス群(段階6)。
// 深度プリパス / モデル単位GPUカリング(動的なパス名) / G-Buffer /
// ModelCullReadback / ソフトウェアラスタライザ / Hi-Z。
//
// 【プリパスとG-Bufferは同じ組を描かなければならない】片方だけ間引くと
// 「深度はあるのに色が無い」穴が開く。組を揃える保証は、両者が
// Rendering/GeometryDrawLoop.h の同じ列挙(ForEachGeometryDraw)を通ることにある。
namespace Kurenai
{
    class KurenaiEngine3D;

    namespace Passes
    {
        class GeometryPasses
        {
        public:
            explicit GeometryPasses(KurenaiEngine3D& engine) : m_Engine(engine) {}

            void Register(
                Core::RenderGraph& graph,
                const Rendering::RenderFrameContext& frame,
                Rendering::RenderBlackboard& bb);

        private:
            KurenaiEngine3D& m_Engine;

            // モデル単位GPUカリングの候補。**この群が持ち主である。**
            // 間接描画のパスを区画ごとに複数登録し、そのExecuteラムダが揃って
            // これを参照捕捉するため、登録関数のローカルにすると寿命が足りない。
            // 中身はフレームごとに clear() して作り直す(容量は使い回す)
            std::vector<ModelCullDrawCandidate> m_ModelCullDraws;
        };
    }
}
