#include "../KurenaiEngine3D.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "Core/Logger.h"
#include "Core/RenderGraph.h"
#include "Core/StringUtil.h"
// パス群のカウンタを m_XxxPasses->Get...() で読むため、前方宣言では足りない
#include "../Passes/GeometryPasses.h"
#include "../Passes/ShadowPasses.h"
#include "RenderDumpService.h"

// 中間レンダーターゲットのダンプ(-dumptex)、パスマニフェストの書き出し(-passmanifest)、
// 性能記録のログ出力。
// KurenaiEngine3D のメンバ関数のまま、翻訳単位だけをここへ分けている
// (宣言は KurenaiEngine3D.h のまま)
namespace Kurenai
{
    std::vector<KurenaiEngine3D::DumpableTexture> KurenaiEngine3D::BuildDumpableTextureTable() const
    {
        // 名前 -> 中間テクスチャ。AddTextureDump(起動オプション -dumptex)が引く。
        //
        // 【DebugViewの番号と共有しない】あちらは「表示モード」でテクスチャと1対1ではない
        // (DepthとDepthRawは同じm_RenderTargets.GBufferDepth、LightTilesはテクスチャではなくバッファを読む)。
        // さらに切り分けで見たいもの ―― SSILRaw / TransmittanceLUT / TAAHistory / ExposureTexture ――
        // はDebugViewに存在せず、足すにはPresent.hlslの表示モードを増やすことになる。
        // 加えてDebugViewの番号は -debugview N として既に契約になっており、
        // 途中に足すとdocsと履歴に記録済みの番号が全部ずれる。
        //
        // 【CreateRenderTargetsの直後に置いてある】ポインタが生まれる場所の隣なら、
        // テクスチャを増やしたときにここへ足し忘れにくい。
        // **CreateRenderTargets等でテクスチャを増やしたらここにも足すこと。**
        //
        // 名前はメンバ名から m_ を外したもの。中身がnullptr(機能が無効・非対応環境)の
        // エントリも表には載せる ―― 「名前が無い」と「今は作られていない」は別のことで、
        // 呼び出し側にそれぞれ別のログを出させるため
        return {
            // G-Buffer
            { "GBufferAlbedo", m_RenderTargets.GBufferAlbedo.get() },
            { "GBufferNormal", m_RenderTargets.GBufferNormal.get() },
            { "GBufferMaterial", m_RenderTargets.GBufferMaterial.get() },
            { "GBufferEmissive", m_RenderTargets.GBufferEmissive.get() },
            { "GBufferDepth", m_RenderTargets.GBufferDepth.get() },
            { "GBufferVelocity", m_RenderTargets.GBufferVelocity.get() },
            { "GBufferBentNormal", m_RenderTargets.GBufferBentNormal.get() },
            // ライティングと間接光
            { "DirectLightTexture", m_RenderTargets.DirectLightTexture.get() },
            { "SSAORawTexture", m_RenderTargets.SSAORawTexture.get() },
            { "SSAOTexture", m_RenderTargets.SSAOTexture.get() },
            { "SSILRawTexture", m_RenderTargets.SSILRawTexture.get() },
            { "SSILTexture", m_RenderTargets.SSILTexture.get() },
            { "RTAORawTexture", m_RenderTargets.RTAORawTexture.get() },
            { "RTAOTexture", m_RenderTargets.RTAOTexture.get() },
            { "RTShadowTexture", m_RenderTargets.RTShadowTexture.get() },
            { "SceneColor", m_RenderTargets.SceneColor.get() },
            // 反射
            { "SSRTexture", m_RenderTargets.SSRTexture.get() },
            { "RTReflectionTexture", m_RenderTargets.RTReflectionTexture.get() },
            { "PlanarReflectionColor", m_RenderTargets.PlanarReflectionColor.get() },
            { "PlanarReflectionDepth", m_RenderTargets.PlanarReflectionDepth.get() },
            // MegaLights
            { "MegaLightsTexture", m_RenderTargets.MegaLightsTexture.get() },
            { "MegaLightsDenoisedTexture", m_RenderTargets.MegaLightsDenoisedTexture.get() },
            // 影・Hi-Z
            { "ShadowCascadeArray", m_RenderTargets.ShadowCascadeArray.get() },
            { "HiZTexture", m_RenderTargets.HiZTexture.get() },
            // 空と大気
            { "SkyCloudTexture", m_RenderTargets.SkyCloudTexture.get() },
            { "SkyCloudFogTexture", m_RenderTargets.SkyCloudFogTexture.get() },
            { "AerialPerspectiveTexture", m_RenderTargets.AerialPerspectiveTexture.get() },
            { "TransmittanceLUT", m_SkyResources.TransmittanceLUT.get() },
            { "MultiScatteringLUT", m_SkyResources.MultiScatteringLUT.get() },
            { "SkyViewLUT", m_SkyResources.SkyViewLUT.get() },
            // DDGI
            { "DDGIIrradianceAtlas", m_GIResources.DDGIIrradianceAtlas.get() },
            { "DDGIDistanceAtlas", m_GIResources.DDGIDistanceAtlas.get() },
            { "DDGIResolveTexture", m_GIResources.DDGIResolveTexture.get() },
            { "DDGIResolveDepthTexture", m_GIResources.DDGIResolveDepthTexture.get() },
            // IBL
            { "BRDFLUTTexture", m_IBLResources.BRDFLUTTexture.get() },
            // ポストプロセスと最終段
            { "TonemapTexture", m_RenderTargets.TonemapTexture.get() },
            { "UpscaleTexture", m_RenderTargets.UpscaleTexture.get() },
            { "UpscaleSharpTexture", m_RenderTargets.UpscaleSharpTexture.get() },
            { "ExposureTexture", m_RenderTargets.ExposureTexture.get() },
            // TAAの履歴。今フレームの書き込み先が m_TAAHistoryIndex なので、
            // 「前フレームの履歴」を見たいときは Prev のほうを指定する
            { "TAAHistory", m_RenderTargets.TAAHistory[m_TAAHistoryIndex].get() },
            { "TAAHistoryPrev", m_RenderTargets.TAAHistory[m_TAAHistoryIndex ^ 1u].get() },
            // 自前ソフトウェアラスタライザ
            { "SoftwareRasterColor", m_RenderTargets.SoftwareRasterColor.get() },
            { "SoftwareRasterDepth", m_RenderTargets.SoftwareRasterDepth.get() },
            { "SoftwareRasterNormal", m_RenderTargets.SoftwareRasterNormal.get() },
        };
    }

    std::vector<std::string> KurenaiEngine3D::GetDumpableTextureNames() const
    {
        std::vector<std::string> names;
        for (const DumpableTexture& entry : BuildDumpableTextureTable())
        {
            names.emplace_back(entry.Name);
        }
        return names;
    }

    void KurenaiEngine3D::AddScheduledRecreation(const ScheduledRecreation& request)
    {
        const char* kindName = nullptr;
        switch (request.Kind)
        {
        case ScheduledRecreationKind::RenderResolution:
            kindName = "RenderResolution";
            if (request.Width == 0 || request.Height == 0)
            {
                Core::Logger::Error("KurenaiEngine3D", "AddScheduledRecreation: RenderResolutionの解像度が不正です");
                return;
            }
            break;
        case ScheduledRecreationKind::UpscaleOutput:
            kindName = "UpscaleOutput";
            if (request.Width == 0 || request.Height == 0)
            {
                Core::Logger::Error("KurenaiEngine3D", "AddScheduledRecreation: UpscaleOutputの解像度が不正です");
                return;
            }
            break;
        case ScheduledRecreationKind::BufferPrecision:
            kindName = "BufferPrecision";
            break;
        case ScheduledRecreationKind::SceneLoad:
            kindName = "SceneLoad";
            if (request.SceneName.empty())
            {
                Core::Logger::Error("KurenaiEngine3D", "AddScheduledRecreation: SceneLoadのシーン名が空です");
                return;
            }
            break;
        default:
            Core::Logger::Error("KurenaiEngine3D", "AddScheduledRecreation: 未知の作り直し種別です");
            return;
        }

        m_ScheduledRecreations.push_back({ request });
        std::string message = "作り直し予約を登録しました: frame=" + std::to_string(request.Frame) + ", kind=" + kindName;
        if (request.Kind == ScheduledRecreationKind::RenderResolution || request.Kind == ScheduledRecreationKind::UpscaleOutput)
        {
            message += ", size=" + std::to_string(request.Width) + "x" + std::to_string(request.Height);
        }
        else if (request.Kind == ScheduledRecreationKind::BufferPrecision)
        {
            message += std::string(", precision=") +
                (request.Precision == BufferPrecision::Legacy8bit ? "Legacy8bit" : "HDR");
        }
        else
        {
            message += ", scene=" + Core::WideToUtf8(request.SceneName);
        }
        Core::Logger::Info("KurenaiEngine3D", message);
    }

    void KurenaiEngine3D::ApplyScheduledRecreations()
    {
        for (ScheduledRecreationSlot& slot : m_ScheduledRecreations)
        {
            if (slot.Fired || m_TAAFrameIndex < slot.Request.Frame)
            {
                continue;
            }

            slot.Fired = true;
            const ScheduledRecreation& request = slot.Request;
            switch (request.Kind)
            {
            case ScheduledRecreationKind::RenderResolution:
                RequestUpscaleSettings(false, m_Settings.PostProcess.UpscaleQuality, request.Width, request.Height);
                Core::Logger::Info(
                    "KurenaiEngine3D", "作り直し予約を発火しました: frame=" + std::to_string(m_TAAFrameIndex) +
                        ", RenderResolution=" + std::to_string(request.Width) + "x" + std::to_string(request.Height));
                break;
            case ScheduledRecreationKind::UpscaleOutput:
                RequestUpscaleSettings(true, m_Settings.PostProcess.UpscaleQuality, request.Width, request.Height);
                Core::Logger::Info(
                    "KurenaiEngine3D", "作り直し予約を発火しました: frame=" + std::to_string(m_TAAFrameIndex) +
                        ", UpscaleOutput=" + std::to_string(request.Width) + "x" + std::to_string(request.Height));
                break;
            case ScheduledRecreationKind::BufferPrecision:
                m_Settings.System.Precision = request.Precision;
                m_BufferPrecisionDirty = true;
                Core::Logger::Info(
                    "KurenaiEngine3D", "作り直し予約を発火しました: frame=" + std::to_string(m_TAAFrameIndex) +
                        ", BufferPrecision=" +
                        (request.Precision == BufferPrecision::Legacy8bit ? std::string("Legacy8bit") : std::string("HDR")));
                break;
            case ScheduledRecreationKind::SceneLoad:
            {
                size_t sceneIndex = m_SceneFilePaths.size();
                std::string candidates;
                for (size_t i = 0; i < m_SceneFilePaths.size(); ++i)
                {
                    const std::wstring& path = m_SceneFilePaths[i];
                    const size_t slash = path.find_last_of(L"\\/");
                    const size_t nameBegin = slash == std::wstring::npos ? 0 : slash + 1;
                    const size_t dot = path.find_last_of(L'.');
                    const std::wstring name = dot != std::wstring::npos && dot >= nameBegin
                        ? path.substr(nameBegin, dot - nameBegin)
                        : path.substr(nameBegin);
                    if (!candidates.empty())
                    {
                        candidates += ", ";
                    }
                    candidates += Core::WideToUtf8(name);
                    if (_wcsicmp(name.c_str(), request.SceneName.c_str()) == 0)
                    {
                        sceneIndex = i;
                    }
                }

                if (sceneIndex == m_SceneFilePaths.size())
                {
                    Core::Logger::Error(
                        "KurenaiEngine3D", "作り直し予約のSceneLoadでシーンが見つかりません: " +
                            Core::WideToUtf8(request.SceneName) + " (候補: " +
                            (candidates.empty() ? std::string("なし") : candidates) + ")");
                    break;
                }

                RequestSceneLoad(sceneIndex);
                Core::Logger::Info(
                    "KurenaiEngine3D", "作り直し予約を発火しました: frame=" + std::to_string(m_TAAFrameIndex) +
                        ", SceneLoad=" + Core::WideToUtf8(request.SceneName));
                break;
            }
            default:
                Core::Logger::Error("KurenaiEngine3D", "作り直し予約の発火で未知の種別を検出しました");
                break;
            }
        }
    }

    void KurenaiEngine3D::ApplyDebugNamesIfDirty()
    {
        // 【表は毎回作り直す】理由は BuildDumpableTextureTable のコメント
        m_DumpService.ApplyDebugNamesIfDirty(BuildDumpableTextureTable());
    }

    void KurenaiEngine3D::IssueTextureDumps(Core::RenderGraph& graph)
    {
        m_DumpService.IssueTextureDumps(graph, BuildDumpableTextureTable(), m_TAAFrameIndex, *m_Device);
    }

    void KurenaiEngine3D::ResolveTextureDumps()
    {
        m_DumpService.ResolveTextureDumps(
            m_TAAFrameIndex, m_Window.get(), m_GraphicsAPI == GraphicsAPI::DX12);
    }

    void KurenaiEngine3D::WritePassManifestIfDue(Core::RenderGraph& graph)
    {
        m_DumpService.WritePassManifestIfDue(graph, m_TAAFrameIndex, m_GraphicsAPI == GraphicsAPI::DX12);
    }
}
