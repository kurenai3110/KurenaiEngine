#include "ScheduledRecreationQueue.h"

#include <string>

#include "Core/Logger.h"
#include "Core/StringUtil.h"

// 作り直し経路の予約。純粋仮想で受ける理由は ScheduledRecreationQueue.h にある
namespace Kurenai::Diagnostics
{
    void ScheduledRecreationQueue::Add(const ScheduledRecreation& request)
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

        m_Slots.push_back({ request });
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

    void ScheduledRecreationQueue::Apply(IRecreationTarget& target, uint32_t frameIndex)
    {
        for (Slot& slot : m_Slots)
        {
            if (slot.Fired || frameIndex < slot.Request.Frame)
            {
                continue;
            }

            slot.Fired = true;
            const ScheduledRecreation& request = slot.Request;
            switch (request.Kind)
            {
            case ScheduledRecreationKind::RenderResolution:
                target.RequestUpscaleSettings(false, request.Width, request.Height);
                Core::Logger::Info(
                    "KurenaiEngine3D", "作り直し予約を発火しました: frame=" + std::to_string(frameIndex) +
                        ", RenderResolution=" + std::to_string(request.Width) + "x" + std::to_string(request.Height));
                break;
            case ScheduledRecreationKind::UpscaleOutput:
                target.RequestUpscaleSettings(true, request.Width, request.Height);
                Core::Logger::Info(
                    "KurenaiEngine3D", "作り直し予約を発火しました: frame=" + std::to_string(frameIndex) +
                        ", UpscaleOutput=" + std::to_string(request.Width) + "x" + std::to_string(request.Height));
                break;
            case ScheduledRecreationKind::BufferPrecision:
                target.RequestBufferPrecision(request.Precision);
                Core::Logger::Info(
                    "KurenaiEngine3D", "作り直し予約を発火しました: frame=" + std::to_string(frameIndex) +
                        ", BufferPrecision=" +
                        (request.Precision == BufferPrecision::Legacy8bit ? std::string("Legacy8bit") : std::string("HDR")));
                break;
            case ScheduledRecreationKind::SceneLoad:
            {
                const std::vector<std::wstring>& scenePaths = target.GetSceneFilePaths();
                size_t sceneIndex = scenePaths.size();
                std::string candidates;
                for (size_t i = 0; i < scenePaths.size(); ++i)
                {
                    const std::wstring& path = scenePaths[i];
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

                if (sceneIndex == scenePaths.size())
                {
                    Core::Logger::Error(
                        "KurenaiEngine3D", "作り直し予約のSceneLoadでシーンが見つかりません: " +
                            Core::WideToUtf8(request.SceneName) + " (候補: " +
                            (candidates.empty() ? std::string("なし") : candidates) + ")");
                    break;
                }

                target.RequestSceneLoad(sceneIndex);
                Core::Logger::Info(
                    "KurenaiEngine3D", "作り直し予約を発火しました: frame=" + std::to_string(frameIndex) +
                        ", SceneLoad=" + Core::WideToUtf8(request.SceneName));
                break;
            }
            default:
                Core::Logger::Error("KurenaiEngine3D", "作り直し予約の発火で未知の種別を検出しました");
                break;
            }
        }
    }
}
