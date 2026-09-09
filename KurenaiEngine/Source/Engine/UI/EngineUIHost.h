#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "Assets/Scene.h"
#include "Assets/TextureStreaming.h"
#include "Core/CPUProfiler.h"
#include "RHI/IRHIDevice.h"
#include "RHI/IRHIGPUProfiler.h"

#include "../Diagnostics/RenderCapabilities.h"
#include "../KurenaiTypes.h"
#include "../Diagnostics/RenderStats.h"
#include "../Scene/InstanceLODState.h"
#include "../Settings/EngineSettings.h"

// UIパネルがエンジンへ求めるものだけを並べた口。
//
// 【なぜ口を切るのか】UIパネルがエンジンの公開ヘッダを直接引くと、パネルを1つ触る
// たびにエンジンのヘッダの変更で作り直しになる。ここに並ぶのはUIが実際に呼ぶものだけで、
// **エンジンの持ち物の全体を見せるためのものではない**。
//
// 【参照で返すものを値に変えないこと】UIはここへ直接書き込む。値で返すと一時オブジェクトを
// 掴んで操作が効かなくなるが、**コンパイルは通ってしまう**。
namespace Kurenai::UI
{
    class IEngineUIHost
    {
    public:
        virtual ~IEngineUIHost() = default;

        virtual void ApplyQualityPreset(QualityPreset preset) = 0;
        virtual bool& GetBufferPrecisionDirty() = 0;
        virtual const Core::CPUProfiler& GetCPUProfiler() const = 0;
        virtual const Assets::Model* GetCurrentLOD(size_t instanceIndex) const = 0;
        virtual size_t GetCurrentSceneIndex() const = 0;
        virtual bool& GetDDGIEmissiveSuppressLoggedRaster() = 0;
        virtual bool& GetDDGIEmissiveSuppressLoggedTrace() = 0;
        virtual uint32_t GetDDGIProbeCount() const = 0;
        virtual uint32_t& GetDDGIStableCycles() = 0;
        virtual bool& GetDDGIUpdateSuspended() = 0;
        virtual bool GetDDGIWarmingUp() const = 0;
        virtual RHI::IRHIDevice* GetDevice() = 0;
        virtual bool& GetEmissiveLightsCapLogged() = 0;
        virtual bool& GetEmissiveLightsValuesLogged() = 0;
        virtual const std::vector<Assets::EmissiveProxy>& GetEmissiveProxies() const = 0;
        virtual const Assets::GIVolume& GetGIVolume() const = 0;
        virtual RHI::IRHIGPUProfiler* GetGPUProfiler() const = 0;
        virtual GraphicsAPI GetGraphicsAPI() const = 0;
        virtual bool GetHasGIVolume() const = 0;
        virtual uint32_t GetHeight() const = 0;
        virtual uint32_t GetHiZMipLevels() const = 0;
        virtual bool& GetIBLBaked() = 0;
        virtual bool& GetIBLIrradianceBaked() = 0;
        virtual const std::vector<Scene::InstanceLODState>& GetInstanceLODStates() const = 0;
        virtual float GetLastFrameGPUWaitTimeMs() const = 0;
        virtual uint32_t GetLightTileCountX() const = 0;
        virtual uint32_t GetLightTileCountY() const = 0;
        virtual std::vector<Assets::Light>& GetLights() = 0;
        virtual float GetMonitorDpiScale() const = 0;
        virtual bool& GetProbeBakeRequested() = 0;
        virtual bool& GetProbeBaked() = 0;
        virtual uint32_t GetProbeRealtimeFace() const = 0;
        virtual uint32_t GetProbeRealtimeProbeIndex() const = 0;
        virtual const QualitySettings& GetQualitySettings() const = 0;
        virtual std::vector<Assets::ReflectionProbe>& GetReflectionProbes() = 0;
        virtual const RenderCapabilities& GetRenderCapabilities() const = 0;
        virtual uint32_t GetRenderHeight() const = 0;
        virtual const RenderStats& GetRenderStats() const = 0;
        virtual uint32_t GetRenderWidth() const = 0;
        virtual const Assets::Scene& GetScene() const = 0;
        virtual ReflectionMode GetSceneDefaultReflectionMode() const = 0;
        virtual const std::vector<std::wstring>& GetSceneDisplayNames() const = 0;
        virtual bool GetSceneLoadInFlight() const = 0;
        virtual std::atomic<uint32_t>& GetSceneLoadProgressLoaded() = 0;
        virtual std::atomic<uint32_t>& GetSceneLoadProgressTotal() = 0;
        virtual size_t GetSceneLoadingIndex() const = 0;
        virtual int& GetSelectedLightIndex() = 0;
        virtual int& GetSelectedProbeIndex() = 0;
        virtual Settings::EngineSettings& GetSettings() = 0;
        virtual bool& GetSkyBakeDirty() = 0;
        virtual std::atomic<bool>& GetTAAHistoryValid() = 0;
        virtual Assets::TextureStreamingManager& GetTextureStreaming() = 0;
        virtual uint32_t GetWidth() const = 0;
        virtual void RequestGraphicsAPIChange(GraphicsAPI api) = 0;
        virtual void RequestPlanarReflectionResolutionScale(float scale) = 0;
        virtual void RequestSceneLoad(size_t sceneIndex) = 0;
        virtual void RequestUpscaleSettings(bool enabled, UpscaleQualityMode mode, uint32_t outputWidth, uint32_t outputHeight) = 0;
        virtual void ResetSceneDependentParams() = 0;
        virtual void SetMegaLightsQuadSamples(int samples) = 0;
        virtual void SetMegaLightsTileJitter(int mode) = 0;
        virtual void SetMegaLightsTilePoolCapacity(int capacity) = 0;
        virtual bool ShouldRunMegaLights() const = 0;
    };
}
