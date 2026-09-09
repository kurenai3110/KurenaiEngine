#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "Assets/MeshLightScene.h"
#include "Assets/Scene.h"
#include "RHI/IRHIBuffer.h"
#include "RHI/IRHIDevice.h"
#include "RHI/IRHITexture.h"

#include "../GI/DDGIGrid.h"
#include "../Rendering/GeometryDrawHost.h"
#include "../Rendering/GeometryDrawLoop.h"
#include "../Settings/EngineSettings.h"

namespace Kurenai::Core { class RenderGraph; }

// パス群がエンジンへ求めるものだけを並べた口。
//
// 【なぜ口を切るのか】パス群がエンジンの公開ヘッダを直接引くと、パスを1つ触るたびに
// エンジンのヘッダの変更で作り直しになる。ここに並ぶのはパス群が実際に呼ぶものだけ。
//
// 【共通ループはここに無い】あれはテンプレートで純粋仮想にできないため、
// MakeGeometryDrawHost() が返す束を使って自由関数として呼ぶ(Rendering/GeometryDrawLoop.h)。
namespace Kurenai::Passes
{
    class IPassHost
    {
    public:
        virtual ~IPassHost() = default;

        virtual void ApplyDebugNamesIfDirty() = 0;
        virtual uint32_t ClampDDGIProbesPerFrameToConstantRing(uint32_t requested) = 0;
        virtual GI::DDGIGrid& GetDDGIGrid() = 0;
        virtual RHI::IRHIDevice* GetDevice() = 0;
        virtual const std::vector<bool>& GetEmissiveProxyInstances() const = 0;
        virtual const Assets::MeshLightScene& GetMeshLightScene() const = 0;
        virtual RHI::IRHIBuffer* GetMeshletCullStatsReadbackSlot() const = 0;
        virtual RHI::IRHIBuffer* GetModelCullReadbackSlot() const = 0;
        virtual uint32_t GetRenderHeight() const = 0;
        virtual uint32_t GetRenderWidth() const = 0;
        virtual const Assets::Scene& GetScene() const = 0;
        virtual Settings::EngineSettings& GetSettings() = 0;
        virtual std::atomic<bool>& GetTAAHistoryValid() = 0;
        virtual RHI::IRHITexture* GetWaterNormalMapTexture() const = 0;
        virtual bool IsMeshLightsEnabled() const = 0;
        virtual void IssueTextureDumps(Core::RenderGraph& graph) = 0;
        virtual Rendering::GeometryDrawHost MakeGeometryDrawHost() = 0;

        // 引数が複数行になるものは元の宣言の形を保つ
        virtual bool ShouldUseModelMeshletPath(
            const Assets::ModelInstance& instance, const Assets::Model& model) const = 0;
        virtual bool IsMeshVisibleCounted(
            const Rendering::FrustumPlanes& frustum, const Assets::ModelInstance& instance,
            const Assets::Model& model, const Assets::Mesh& mesh) = 0;
    };
}
