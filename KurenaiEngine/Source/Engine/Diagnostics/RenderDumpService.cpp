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
// シャドウのドローコール数を m_ShadowPasses->GetDrawCalls() で読むため、前方宣言では足りない
#include "../Passes/ShadowPasses.h"
#include "RenderDumpService.h"

// 中間レンダーターゲットのダンプ(-dumptex)と、性能記録のログ出力。
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

    void KurenaiEngine3D::ApplyDebugNames() const
    {
        uint32_t named = 0;
        for (const DumpableTexture& entry : BuildDumpableTextureTable())
        {
            if (entry.Texture != nullptr)
            {
                entry.Texture->SetDebugName(entry.Name);
                ++named;
            }
        }
        // 何本に名前が付いたかを残す。**「名前が出ない」ときに、付け忘れなのか
        // その機能が無効でテクスチャ自体が無いのかを、ログだけで切り分けられるようにする**
        Core::Logger::Info(
            "KurenaiEngine3D",
            "グラフィックスデバッガ向けの名前を付けました: " + std::to_string(named) + "本");
    }

    void KurenaiEngine3D::ApplyDebugNamesIfDirty()
    {
        if (!m_DebugNamesDirty)
        {
            return;
        }
        ApplyDebugNames();
        m_DebugNamesDirty = false;
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

    std::wstring MakeTextureDumpSequencePath(const std::wstring& path, uint32_t targetFrames, uint32_t sequenceIndex)
    {
        if (targetFrames <= 1)
        {
            return path;
        }

        wchar_t suffix[16] = {};
        if (swprintf_s(suffix, L"_%04u", sequenceIndex) < 0)
        {
            Core::Logger::Error("KurenaiEngine3D", "テクスチャの書き出し: 連番ファイル名を作れませんでした");
            return path;
        }
        const size_t slash = path.find_last_of(L"\\/");
        const size_t dot = path.find_last_of(L'.');
        if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
        {
            return path.substr(0, dot) + suffix + path.substr(dot);
        }
        return path + suffix;
    }

    void KurenaiEngine3D::AddTextureDump(
        const wchar_t* name, const wchar_t* path, int mipLevel, int arraySlice, int frames, int stride)
    {
        if (name == nullptr || path == nullptr || name[0] == L'\0' || path[0] == L'\0')
        {
            Core::Logger::Error("KurenaiEngine3D", "AddTextureDump: テクスチャ名か出力先が空です");
            return;
        }

        TextureDumpRequest request;
        request.Name = Core::WideToUtf8(name);
        request.Path = path;
        request.MipLevel = mipLevel > 0 ? static_cast<uint32_t>(mipLevel) : 0u;
        request.ArraySlice = arraySlice > 0 ? static_cast<uint32_t>(arraySlice) : 0u;
        request.TargetFrames = frames > 0 ? static_cast<uint32_t>(frames) : 1u;
        request.Stride = stride > 0 ? static_cast<uint32_t>(stride) : 1u;
        m_TextureDumps.push_back(std::move(request));

        Core::Logger::Info(
            "KurenaiEngine3D",
            "テクスチャの書き出しを予約しました: " + m_TextureDumps.back().Name + " -> " +
                Core::WideToUtf8(path) + " (mip=" + std::to_string(m_TextureDumps.back().MipLevel) +
                ", slice=" + std::to_string(m_TextureDumps.back().ArraySlice) +
                ", frames=" + std::to_string(m_TextureDumps.back().TargetFrames) +
                ", stride=" + std::to_string(m_TextureDumps.back().Stride) + ")");
    }

    void KurenaiEngine3D::SetTextureDumpFrame(int frame)
    {
        m_TextureDumpFrame = frame;
        Core::Logger::Info(
            "KurenaiEngine3D",
            "テクスチャを書き出すフレームを設定しました: " +
                (frame < 0 ? std::string("既定(") + std::to_string(kMegaLightsAccumWarmup) + ")"
                           : std::to_string(frame)));
    }

    void KurenaiEngine3D::SetExitAfterDump(bool enabled)
    {
        m_ExitAfterDump = enabled;
        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("書き出し後の自動終了: ") + (enabled ? "有効" : "無効"));
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
                RequestUpscaleSettings(false, m_PostProcessSettings.UpscaleQuality, request.Width, request.Height);
                Core::Logger::Info(
                    "KurenaiEngine3D", "作り直し予約を発火しました: frame=" + std::to_string(m_TAAFrameIndex) +
                        ", RenderResolution=" + std::to_string(request.Width) + "x" + std::to_string(request.Height));
                break;
            case ScheduledRecreationKind::UpscaleOutput:
                RequestUpscaleSettings(true, m_PostProcessSettings.UpscaleQuality, request.Width, request.Height);
                Core::Logger::Info(
                    "KurenaiEngine3D", "作り直し予約を発火しました: frame=" + std::to_string(m_TAAFrameIndex) +
                        ", UpscaleOutput=" + std::to_string(request.Width) + "x" + std::to_string(request.Height));
                break;
            case ScheduledRecreationKind::BufferPrecision:
                m_SystemSettings.Precision = request.Precision;
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

    void KurenaiEngine3D::IssueTextureDumps(Core::RenderGraph& graph)
    {
        if (m_TextureDumps.empty())
        {
            return;
        }

        // 【整定を待つ】起動直後はストリーミングでモデルとテクスチャが入ってくる途中であり、
        // シーンがRenderResolutionを持つ場合は内部解像度も既定値(1920x1080)から切り替わる。
        // 待たずに書き出すと、読み込み途中の絵を既定解像度のまま吐き出すことになる
        // (kMegaLightsAccumWarmupのコメントに、実際にそうなった記録がある)
        const uint32_t targetFrame =
            m_TextureDumpFrame >= 0 ? static_cast<uint32_t>(m_TextureDumpFrame) : kMegaLightsAccumWarmup;
        if (m_TAAFrameIndex < targetFrame)
        {
            return;
        }

        // 【表は毎回作り直す】レンダーターゲットはリサイズやバッファ精度の切り替えで
        // ポインタごと作り直される。キャッシュすると解放済みのテクスチャを指す
        const std::vector<DumpableTexture> table = BuildDumpableTextureTable();

        // このフレームでコピーを積むぶん。要求ごとにコピー元を覚えておく
        struct PendingCopy
        {
            size_t RequestIndex = 0;
            size_t SlotIndex = 0;
            RHI::IRHITexture* Source = nullptr;
        };
        std::vector<PendingCopy> pending;
        std::vector<RHI::IRHITexture*> reads;

        for (size_t i = 0; i < m_TextureDumps.size(); ++i)
        {
            TextureDumpRequest& request = m_TextureDumps[i];
            if (request.Done || request.IssuedCount >= request.TargetFrames ||
                (request.AnyIssued && m_TAAFrameIndex - request.LastIssueFrame < request.Stride))
            {
                continue;
            }

            size_t slotIndex = request.Slots.size();
            for (size_t j = 0; j < request.Slots.size(); ++j)
            {
                if (!request.Slots[j].Busy) { slotIndex = j; break; }
            }
            if (slotIndex == request.Slots.size() && request.RingDepth != 0 && request.Slots.size() >= request.RingDepth)
            {
                // 全受け皿が遅延中なら、未回収のコピーを壊さず次フレームへ回す。
                continue;
            }

            const DumpableTexture* found = nullptr;
            for (const DumpableTexture& entry : table)
            {
                if (_stricmp(entry.Name, request.Name.c_str()) == 0)
                {
                    found = &entry;
                    break;
                }
            }

            if (found == nullptr)
            {
                // 【有効な名前を全部並べる】UIを見られない利用者にとって、
                // これが「何が指定できるか」を知る唯一の手段になる
                std::string names;
                for (const DumpableTexture& entry : table)
                {
                    if (!names.empty())
                    {
                        names += ", ";
                    }
                    names += entry.Name;
                }
                Core::Logger::Error(
                    "KurenaiEngine3D",
                    "テクスチャの書き出し: 名前が見つかりません: " + request.Name + " / 指定できる名前: " + names);
                request.Done = true;
                continue;
            }

            if (found->Texture == nullptr)
            {
                // 名前はあるが、その機能が無効か非対応環境。**「名前が無い」とは別のログにする**
                Core::Logger::Error(
                    "KurenaiEngine3D",
                    "テクスチャの書き出し: " + request.Name +
                        " は今このフレームでは作られていません(機能が無効か、非対応の環境)。書き出しを中止します");
                request.Done = true;
                continue;
            }

            if (slotIndex == request.Slots.size())
            {
                request.Slots.emplace_back();
            }
            TextureDumpSlot& slot = request.Slots[slotIndex];
            slot.Readback = m_Device->CreateReadbackTexture(found->Texture, request.MipLevel);
            if (!slot.Readback)
            {
                // CreateReadbackTextureが理由をログへ出している(非対応フォーマット・範囲外のミップ等)
                Core::Logger::Error(
                    "KurenaiEngine3D", "テクスチャの書き出し: 受け皿を作れませんでした: " + request.Name);
                request.Done = true;
                for (TextureDumpSlot& releaseSlot : request.Slots)
                {
                    releaseSlot.Readback.reset();
                }
                continue;
            }

            // 【寸法は今ここで控える】あとで生ポインタから引き直すと、その間にリサイズが起きた場合に
            // 受け皿の中身と食い違う値をヘッダへ書いてしまう
            slot.Desc = slot.Readback->GetReadbackDesc(0);
            if (request.IssuedCount == 0)
            {
                request.FirstDesc = slot.Desc;
                const size_t bytesPerSlot =
                    static_cast<size_t>(slot.Desc.Width) * slot.Desc.BytesPerTexel * slot.Desc.Height;
                const uint32_t desiredDepth = std::min(request.TargetFrames, kTextureDumpRingDepth);
                const uint32_t memoryDepth = bytesPerSlot == 0 ? 1u :
                    static_cast<uint32_t>(std::max<size_t>(1, kTextureDumpRingMaxBytes / bytesPerSlot));
                const uint32_t ringDepth = std::min(desiredDepth, memoryDepth);
                request.RingDepth = ringDepth;
                if (ringDepth < desiredDepth)
                {
                    Core::Logger::Warning("KurenaiEngine3D", "テクスチャの書き出し: リング深さを " + std::to_string(ringDepth) +
                        " へ下げました。実効レートが 1/(遅延+1) に落ちます: " + request.Name);
                }
            }
            else if (slot.Desc.Width != request.FirstDesc.Width ||
                     slot.Desc.Height != request.FirstDesc.Height ||
                     slot.Desc.ChannelCount != request.FirstDesc.ChannelCount ||
                     slot.Desc.ElementType != request.FirstDesc.ElementType)
            {
                // 【寸法の違う絵を1つの連番に混ぜない】混ざったまま時間方向の平均や分散を取ると、
                // エラーにならず静かに間違った数字が出る。ここで打ち切って、何が変わったかを数値で残す
                Core::Logger::Error(
                    "KurenaiEngine3D",
                    "テクスチャの書き出し: 連番の途中で寸法か形式が変わったので打ち切ります: " + request.Name +
                        " (" + std::to_string(request.FirstDesc.Width) + "x" +
                        std::to_string(request.FirstDesc.Height) +
                        " ch=" + std::to_string(request.FirstDesc.ChannelCount) +
                        " elem=" + std::to_string(static_cast<uint32_t>(request.FirstDesc.ElementType)) +
                        " から " + std::to_string(slot.Desc.Width) + "x" + std::to_string(slot.Desc.Height) +
                        " ch=" + std::to_string(slot.Desc.ChannelCount) +
                        " elem=" + std::to_string(static_cast<uint32_t>(slot.Desc.ElementType)) +
                        " へ変化。" + std::to_string(request.WrittenCount) + "枚まで書けています)");
                request.Done = true;
                for (TextureDumpSlot& releaseSlot : request.Slots)
                {
                    releaseSlot.Readback.reset();
                }
                continue;
            }
            slot.CopyFrame = m_TAAFrameIndex;
            slot.SequenceIndex = request.IssuedCount;
            slot.FailedFrames = 0;
            slot.Busy = true;
            ++request.IssuedCount;
            request.LastIssueFrame = m_TAAFrameIndex;
            if (!request.AnyIssued)
            {
                request.FirstIssueFrame = m_TAAFrameIndex;
            }
            request.AnyIssued = true;

            pending.push_back(PendingCopy{ i, slotIndex, found->Texture });
            reads.push_back(found->Texture);
        }

        if (pending.empty())
        {
            return;
        }

        // 【Readsだけを持つパス】書き手より後に順序付けるためにReadsへ入れる。
        // Writesを持たないので新たな循環依存は作らない(MegaLightsDumpと同じ形)
        graph.AddPass(Core::RenderGraphPassDesc{
            .Name = "TextureDump",
            .Reads = std::move(reads),
            .Execute = [this, pending](RHI::IRHICommandList* cmd)
            {
                for (const PendingCopy& copy : pending)
                {
                    TextureDumpRequest& request = m_TextureDumps[copy.RequestIndex];
                    if (copy.SlotIndex >= request.Slots.size() || !request.Slots[copy.SlotIndex].Readback)
                    {
                        Core::Logger::Error("KurenaiEngine3D", "テクスチャの書き出し: コピー先スロットが無効です: " + request.Name);
                        continue;
                    }
                    cmd->CopyTextureToReadback(
                        request.Slots[copy.SlotIndex].Readback.get(), copy.Source, request.MipLevel, request.ArraySlice);
                }
            },
        });
    }

    void KurenaiEngine3D::ResolveTextureDumps()
    {
        if (m_TextureDumps.empty())
        {
            return;
        }
        bool allDone = true;
        for (TextureDumpRequest& request : m_TextureDumps)
        {
            if (request.Done)
            {
                continue;
            }

            // 【必ず打ち切る】対象のテクスチャがそのフレームで作られなくなると連番は永久に揃わない。
            // -exitafterdump と組み合わせた無人実行が静かに固まるのが最悪の失敗なので、
            // 上限を決めて諦める(kTextureDumpMaxFailedFrames が置かれているのと同じ理由)
            const uint64_t timeout = static_cast<uint64_t>(request.TargetFrames) *
                std::max(request.Stride, kTextureDumpReadDelayFrames + 1) + kTextureDumpMaxFailedFrames + 120;
            bool abortedByTimeout = false;
            if (request.AnyIssued && static_cast<uint64_t>(m_TAAFrameIndex - request.FirstIssueFrame) > timeout)
            {
                Core::Logger::Error("KurenaiEngine3D", "テクスチャの書き出し: " + std::to_string(request.TargetFrames) +
                    "枚要求のうち" + std::to_string(request.WrittenCount) + "枚しか書けないまま打ち切りました: " + request.Name);
                request.Done = true;
                abortedByTimeout = true;
            }

            bool anyBusy = false;
            for (TextureDumpSlot& slot : request.Slots)
            {
                if (!slot.Busy)
                {
                    continue;
                }
                if (m_TAAFrameIndex - slot.CopyFrame < kTextureDumpReadDelayFrames)
                {
                    allDone = false;
                    continue;
                }
                const uint32_t rowPitch = slot.Desc.Width * slot.Desc.BytesPerTexel;
                const size_t totalBytes = static_cast<size_t>(rowPitch) * slot.Desc.Height;
                if (totalBytes == 0 || totalBytes > std::numeric_limits<uint32_t>::max())
                {
                    Core::Logger::Error("KurenaiEngine3D", "テクスチャの書き出し: 中間のサイズが不正です: " + request.Name);
                    slot.Busy = false;
                    continue;
                }
                std::vector<uint8_t> pixels(totalBytes);
                if (!slot.Readback || !slot.Readback->ReadbackData(pixels.data(), static_cast<uint32_t>(totalBytes)))
                {
                    ++slot.FailedFrames;
                    if (slot.FailedFrames >= kTextureDumpMaxFailedFrames)
                    {
                        Core::Logger::Error("KurenaiEngine3D", "テクスチャの書き出し: " +
                            std::to_string(kTextureDumpMaxFailedFrames) + "フレーム続けて読み戻せませんでした。中止します: " + request.Name);
                        slot.Busy = false;
                    }
                    else
                    {
                        allDone = false;
                    }
                    continue;
                }
                if (WriteTextureDumpFile(request, slot, pixels))
                {
                    ++request.WrittenCount;
                }
                slot.Busy = false;
            }

            anyBusy = std::any_of(request.Slots.begin(), request.Slots.end(),
                [](const TextureDumpSlot& slot) { return slot.Busy; });
            if (!request.Done && request.IssuedCount >= request.TargetFrames && !anyBusy)
            {
                request.Done = true;
            }
            if (request.Done)
            {
                for (TextureDumpSlot& slot : request.Slots)
                {
                    slot.Readback.reset();
                }
                // 打ち切りのログに枚数がもう入っているので、そのときはサマリを重ねない
                if (!abortedByTimeout)
                {
                    // 【「書けた枚数」を必ず出す】これまでは「諦めた」も完了として扱われ、
                    // 1枚も書けなくても -exitafterdump が正常終了していた
                    const std::string summary = "テクスチャの書き出し完了: " + std::to_string(request.TargetFrames) +
                        "枚中" + std::to_string(request.WrittenCount) + "枚書けました: " + request.Name;
                    if (request.WrittenCount == 0)
                    {
                        Core::Logger::Error("KurenaiEngine3D", summary);
                    }
                    else
                    {
                        Core::Logger::Info("KurenaiEngine3D", summary);
                    }
                }
            }
            else
            {
                allDone = false;
            }
        }

        if (allDone && m_ExitAfterDump && !m_ExitAfterDumpRequested)
        {
            m_ExitAfterDumpRequested = true;
            if (m_Window)
            {
                // 【PostQuitMessageではない】あれは**呼び出したスレッドの**キューへWM_QUITを積む。
                // ここはRenderスレッドで、メッセージを汲むのはUpdateスレッド(Run()のPumpMessages)
                // なので、Renderスレッドから呼んでも誰も拾わず永久に終わらない。
                // PostMessageWはスレッド安全にウィンドウのキューへ積める。
                // WM_CLOSEはWindow::HandleMessageがm_ShouldClose=trueにするだけなので、
                // Run()のループが正規の手順で抜ける(スレッドの停止も後始末も普段どおり走る)
                Core::Logger::Info("KurenaiEngine3D", "テクスチャの書き出しが完了したので終了します");
                PostMessageW(m_Window->GetHandle(), WM_CLOSE, 0, 0);
            }
            else
            {
                Core::Logger::Error(
                    "KurenaiEngine3D", "書き出し後の自動終了: ウィンドウが無いため終了要求を出せません");
            }
        }
    }

    bool KurenaiEngine3D::WriteTextureDumpFile(
        const TextureDumpRequest& request, const TextureDumpSlot& slot, const std::vector<uint8_t>& pixels) const
    {
        // ファイル形式(Tools/texdump_inspect.py と一致させること):
        //   off  size  内容
        //     0    4   マジック 'K','T','X','D'
        //     4    4   uint32 Version (=2。v1はBackend欄が無く、SourceNameの位置が4バイト手前)
        //     8    4   uint32 HeaderBytes (=128。ピクセルデータはここから始まる)
        //    12    4   uint32 Width
        //    16    4   uint32 Height
        //    20    4   uint32 ChannelCount (1..4。ElementType=4では「展開後の」成分数=3)
        //    24    4   uint32 ElementType (1=UNorm8, 2=Float16, 3=Float32, 4=Packed11_11_10_Float)
        //    28    4   uint32 BytesPerElement (1/2/4。ElementType=4だけは1テクセルのバイト数=4)
        //    32    4   uint32 FrameIndex (m_TAAFrameIndex。複数枚が同一フレームかの照合用)
        //    36    4   uint32 MipLevel
        //    40    4   uint32 ArraySlice
        //    44    4   uint32 Backend (1=DX11, 2=DX12)
        //    48   64   char   SourceName[64] (NUL終端UTF-8。迷子のファイルの自己申告用)
        //   112   16   予約(0)
        //   128  ...   ピクセルデータ。**行パディング無し**、上から下・左から右、
        //              index = (y * Width + x) * ChannelCount。リトルエンディアン
        //              (ElementType=4だけは 1テクセル=uint32 1個で、展開は読み手が行う)
        //
        // 【HeaderBytesを持たせる理由】後からフィールドを足しても、読み手の
        // 「ここからがデータ」という判断が変わらないようにするため。
        //
        // 【DXGI_FORMATは書かない】RHIがD3D固有の型を公開していないうえ、読み手が知りたい
        // 「量子化の刻み幅」はElementTypeだけで決まる(UNorm8なら1/255、Float16なら半精度)。
        // 意味を持たない値をヘッダに置くと、いつか誰かがそれを根拠に判断してしまう。
        //
        // 【Backendを書く理由】DX11とDX12のダンプは、一致していればヘッダまでバイト一致する。
        // そうなると「本当に別々のバックエンドで採ったのか」をファイルから確かめられず、
        // A/Bで言うところの「片方が実行されていない」を潰せない。
        // 出所をファイル自身に自己申告させる
        constexpr uint32_t kHeaderBytes = 128;
        constexpr uint32_t kNameBytes = 64;

        uint32_t elementType = 0;
        uint32_t bytesPerElement = 0;
        const std::wstring outputPath = MakeTextureDumpSequencePath(request.Path, request.TargetFrames, slot.SequenceIndex);
        switch (slot.Desc.ElementType)
        {
        case RHI::TextureElementType::UNorm8:
            elementType = 1;
            bytesPerElement = 1;
            break;
        case RHI::TextureElementType::Float16:
            elementType = 2;
            bytesPerElement = 2;
            break;
        case RHI::TextureElementType::Float32:
            elementType = 3;
            bytesPerElement = 4;
            break;
        case RHI::TextureElementType::Packed11_11_10_Float:
            // 1テクセル4バイトに3成分が詰まっている。**ここでは展開せず、詰まったまま書く。**
            //
            // 【なぜC++側で展開しないのか】展開の正しさを確かめるには非ゼロのR11G11B10データが要るが、
            // このフォーマットを使うのはG-Bufferのエミッシブだけで、手元のどのシーンでも全画素0だった
            // (MaterialTest / PenumbraH4 / BistroInteriorLit で確認)。
            // 一度も動かせないデコーダをC++に置くと、いつか非ゼロのデータが来たときに
            // 静かに誤った数値を返す。読み手(Tools/texdump_inspect.py)に置けば、
            // 11bit/10bitの全ビットパターンを網羅した検算をselftestで常時回せる
            elementType = 4;
            bytesPerElement = 4; // 1テクセルあたりのバイト数(1成分あたりではない)
            break;
        default:
            Core::Logger::Error(
                "KurenaiEngine3D", "テクスチャの書き出し: 解釈できない要素型です: " + request.Name);
            return false;
        }

        std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "テクスチャを書き出せませんでした(ファイルを開けない): " + Core::WideToUtf8(outputPath));
            return false;
        }

        const char magic[4] = { 'K', 'T', 'X', 'D' };
        const uint32_t header[11] = {
            // 【Backend欄を足したときに上げた】v1とv2はSourceNameの位置が4バイトずれる。
            // 上げずに黙って読ませると、名前の先頭4文字がBackendとして解釈される
            2u,                        // Version
            kHeaderBytes,              // HeaderBytes
            slot.Desc.Width,           // Width
            slot.Desc.Height,          // Height
            slot.Desc.ChannelCount,    // ChannelCount
            elementType,               // ElementType
            bytesPerElement,           // BytesPerElement
            // 読み戻し完了時ではなく、画素が属するコピー発行時のフレームを記録する。
            slot.CopyFrame,            // FrameIndex
            request.MipLevel,          // MipLevel
            request.ArraySlice,        // ArraySlice
            m_GraphicsAPI == GraphicsAPI::DX12 ? 2u : 1u, // Backend
        };
        char name[kNameBytes] = {};
        // 名前が64バイトを超える場合は切り詰める(NUL終端は必ず残す)
        const size_t nameLength = std::min(request.Name.size(), static_cast<size_t>(kNameBytes - 1));
        std::memcpy(name, request.Name.data(), nameLength);
        char reserved[kHeaderBytes - sizeof(magic) - sizeof(header) - kNameBytes] = {};

        file.write(magic, sizeof(magic));
        file.write(reinterpret_cast<const char*>(header), sizeof(header));
        file.write(name, sizeof(name));
        file.write(reserved, sizeof(reserved));
        file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));

        if (!file)
        {
            Core::Logger::Error(
                "KurenaiEngine3D",
                "テクスチャを書き出せませんでした(書き込みに失敗): " + Core::WideToUtf8(outputPath));
            return false;
        }

        // 【この行を待って読むこと】ファイルが存在することは書き終わりを意味しない。
        // 呼び出し側(スキルの手順)はこの行が出てからプロセスを落とす
        Core::Logger::Info(
            "KurenaiEngine3D",
            "テクスチャを書き出しました: " + Core::WideToUtf8(outputPath) + " (" + request.Name + " " +
                std::to_string(slot.Desc.Width) + "x" + std::to_string(slot.Desc.Height) +
                " ch=" + std::to_string(slot.Desc.ChannelCount) + " elem=" + std::to_string(elementType) +
                " mip=" + std::to_string(request.MipLevel) + " slice=" + std::to_string(request.ArraySlice) +
                " frame=" + std::to_string(slot.CopyFrame) + ")");
        return true;
    }

    // 性能の記録をログファイルへ残す。ProfilerPanelの表示は実行中しか見えず、後から
    // 「この変更でフレーム時間がどう変わったか」を比較できない。集計期間ぶんを1行に
    // まとめて出すことで、フレーム時間への影響(Logger::Infoはflushを伴う)を
    // 1秒に1回に抑えつつ、実行ごとの記録が残るようにしている
    void KurenaiEngine3D::LogFrameStatsIfDue(float renderDeltaTime)
    {
        if (!m_SystemSettings.FrameStatsLoggingEnabled)
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (m_FrameStatsFrameCount == 0)
        {
            m_FrameStatsWindowStart = now;
        }

        ++m_FrameStatsFrameCount;
        m_FrameStatsCPUTimeSumMs += m_RenderStats.CPUFrameTimeMs;
        m_FrameStatsGPUTimeSumMs += m_GPUProfiler ? m_GPUProfiler->GetTotalFrameTimeMs() : 0.0f;
        m_FrameStatsGPUWaitSumMs += m_Device->GetLastFrameGPUWaitTimeMs();
        m_FrameStatsWorstFrameTimeMs = std::max(m_FrameStatsWorstFrameTimeMs, renderDeltaTime * 1000.0f);
        m_FrameStatsCullTestedSum += m_FrustumCullTested;
        m_FrameStatsCullCulledSum += m_FrustumCullCulled;
        m_FrameStatsLODSwitchSum += m_LODSwitchCount;
        m_FrameStatsLODFadingSum += m_RenderStats.LODFadingCount;
        m_FrameStatsMeshCullTestedSum += m_MeshCullTested;
        m_FrameStatsMeshCullCulledSum += m_MeshCullCulled;
        m_FrameStatsDrawCallsGBufferSum += m_DrawCallsGBuffer;
        m_FrameStatsDrawCallsShadowSum += m_ShadowPasses->GetDrawCalls();
        m_FrameStatsDrawCallsDepthPrepassSum += m_DrawCallsDepthPrepass;
        m_FrameStatsInstancedBatchSum += m_InstancedBatchCount;
        m_FrameStatsInstancedInstanceSum += m_InstancedInstanceCount;

        const float elapsedSeconds = std::chrono::duration<float>(now - m_FrameStatsWindowStart).count();
        if (elapsedSeconds < Defaults::FrameStatsLogIntervalSeconds)
        {
            return;
        }

        // 集計期間の実測フレーム数から求める。m_RenderStats.FPS(指数移動平均)と違い、この値は
        // 期間中に落ちたフレームがそのまま反映される
        const float averageFPS = static_cast<float>(m_FrameStatsFrameCount) / std::max(elapsedSeconds, 1e-6f);
        const double frameCount = static_cast<double>(m_FrameStatsFrameCount);

        char buffer[256];
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%ux%u %s | FPS %.1f (%u frames / %.2fs) | CPU %.2fms | GPU %.2fms | GPU待ち %.2fms | 最悪フレーム %.2fms",
            m_RenderWidth,
            m_RenderHeight,
            m_GraphicsAPI == GraphicsAPI::DX12 ? "DX12" : "DX11",
            averageFPS,
            m_FrameStatsFrameCount,
            elapsedSeconds,
            m_FrameStatsCPUTimeSumMs / frameCount,
            m_FrameStatsGPUTimeSumMs / frameCount,
            m_FrameStatsGPUWaitSumMs / frameCount,
            m_FrameStatsWorstFrameTimeMs);
        Core::Logger::Info("Perf", buffer);

        // パス別の内訳。どのパスを削れば効くのかは合計値からは分からないため、
        // 集計期間の最後のフレームぶんを重い順に並べて残す。
        // (毎フレーム平均を取るにはパス構成がフレームごとに変わりうるので、
        //  代表として1フレームぶんを出す。ベイクパスが走ったフレームに当たると
        //  その分だけ大きく出るが、常時走るパスの比較には十分)
        if (m_GPUProfiler)
        {
            std::vector<RHI::GPUTimingResult> passes = m_GPUProfiler->GetResults();
            std::sort(passes.begin(), passes.end(), [](const auto& a, const auto& b) { return a.TimeMs > b.TimeMs; });

            std::string breakdown;
            for (const auto& pass : passes)
            {
                // 0.05ms未満は並べても判断材料にならず、行が長くなるだけなので落とす
                if (pass.TimeMs < 0.05f)
                {
                    break;
                }
                char passText[64];
                std::snprintf(passText, sizeof(passText), "%s %.2f", pass.Name.c_str(), pass.TimeMs);
                if (!breakdown.empty())
                {
                    breakdown += " / ";
                }
                breakdown += passText;
            }

            if (!breakdown.empty())
            {
                Core::Logger::Info("Perf", "  GPU内訳[ms]: " + breakdown);
            }
        }

        // CPU側の内訳も同じ形で残す。GIVolumeを持つシーンではCPUフレーム時間が24〜28msあり、
        // 60fpsの予算(16.7ms)をCPU単独で超えている。GPUの内訳だけでは、その時間が
        // どのパスのドローコール発行に消えているのかが分からない
        {
            std::vector<Core::CPUTimingResult> cpuPasses = m_CPUProfiler.GetResults();
            std::sort(
                cpuPasses.begin(), cpuPasses.end(), [](const auto& a, const auto& b) { return a.TimeMs > b.TimeMs; });

            std::string breakdown;
            for (const auto& pass : cpuPasses)
            {
                // GPU側と同じ理由で0.05ms未満は落とす
                if (pass.TimeMs < 0.05f)
                {
                    break;
                }
                char passText[64];
                std::snprintf(passText, sizeof(passText), "%s %.2f", pass.Name.c_str(), pass.TimeMs);
                if (!breakdown.empty())
                {
                    breakdown += " / ";
                }
                breakdown += passText;
            }

            if (!breakdown.empty())
            {
                Core::Logger::Info("Perf", "  CPU内訳[ms]: " + breakdown);
            }
        }

        // フラスタムカリングの効き。「間引いた数が0」は、判定式が常に通しているのか
        // 本当に全部が視界内なのかを区別できないため、テストした数と併せて出す。
        //
        // 【モデル単位とメッシュ単位を別の行にする】分母も、効くシーンも違う。
        // モデル単位は.kmodelを多数並べるシーンで効き、1モデルに数千メッシュを持つ
        // アセットでは1つも間引けない。メッシュ単位はその逆。合算すると、どちらが効いたのか
        // ―― あるいは片方が一度も実行されていないのか ―― が読めなくなる
        const auto logCullStats = [this](const char* label, uint64_t testedSum, uint64_t culledSum)
        {
            if (testedSum == 0 || m_FrameStatsFrameCount == 0)
            {
                // 判定が1回も走っていない。「間引き0」と区別が付くよう、行そのものを出さない
                return;
            }
            const double testedPerFrame = static_cast<double>(testedSum) / m_FrameStatsFrameCount;
            const double culledPerFrame = static_cast<double>(culledSum) / m_FrameStatsFrameCount;
            const double ratio = 100.0 * static_cast<double>(culledSum) / static_cast<double>(testedSum);

            char cullText[224];
            std::snprintf(
                cullText, sizeof(cullText), "  %s: 判定 %.1f / 間引き %.1f (%.1f%%) [1フレームあたり・全パス合計]",
                label, testedPerFrame, culledPerFrame, ratio);
            Core::Logger::Info("Perf", cullText);
        };
        logCullStats("フラスタムカリング(モデル単位)", m_FrameStatsCullTestedSum, m_FrameStatsCullCulledSum);
        logCullStats("フラスタムカリング(メッシュ単位)", m_FrameStatsMeshCullTestedSum, m_FrameStatsMeshCullCulledSum);

        // モデルLOD。【切り替え0回なら一度も効いていない】距離のしきい値が実際の
        // カメラの動く範囲から外れているか、そもそもLODPathが指定されていない
        {
            char lodText[192];
            std::snprintf(
                lodText, sizeof(lodText),
                "  モデルLOD: 切り替え %llu回 / フェード %llu インスタンス×フレーム [いずれも集計期間の合計]",
                static_cast<unsigned long long>(m_FrameStatsLODSwitchSum),
                static_cast<unsigned long long>(m_FrameStatsLODFadingSum));
            Core::Logger::Info("Perf", lodText);
        }

        // モデルのストリーミング。【常駐0や読み込み0なら効いていない】
        // 範囲内なのに常駐していないものが残り続けるなら、発注か受け取りのどこかで詰まっている
        if (m_Scene.HasStreamingDistance)
        {
            char streamText[192];
            std::snprintf(
                streamText, sizeof(streamText),
                "  ストリーミング: 常駐 %u / 範囲内 %u (距離 %.0fm) / 読み込み累計 %llu件 / 破棄累計 %llu件"
                " / RT再構築 %llu回(直近 %.1fms)",
                m_StreamingResidentCount, m_StreamingTargetCount, m_Scene.StreamingDistance,
                static_cast<unsigned long long>(m_StreamingLoadedTotal),
                static_cast<unsigned long long>(m_StreamingEvictedTotal),
                static_cast<unsigned long long>(m_RaytracingRebuildCount), m_RaytracingRebuildLastMs);
            Core::Logger::Info("Perf", streamText);
        }

        // パス別のドローコール数。**「G-Bufferは減ったがシャドウは減っていない」**のような
        // 片手落ちは合計値では見えない(シャドウはカスケード4回ぶんが積み上がる)
        if (m_FrameStatsFrameCount > 0)
        {
            const double frames = static_cast<double>(m_FrameStatsFrameCount);
            char drawText[224];
            std::snprintf(
                drawText, sizeof(drawText),
                "  ドローコール: G-Buffer %.1f / シャドウ %.1f (4カスケード計) / 深度プリパス %.1f "
                "[1フレームあたり]",
                static_cast<double>(m_FrameStatsDrawCallsGBufferSum) / frames,
                static_cast<double>(m_FrameStatsDrawCallsShadowSum) / frames,
                static_cast<double>(m_FrameStatsDrawCallsDepthPrepassSum) / frames);
            Core::Logger::Info("Perf", drawText);
        }

        // インスタンシングの効き。**ドローコール数とは別建てにする** ――
        // 「バッチ0」は「まとめられる相手がいない」のか「一度も実行されていない」のかを
        // 区別できないので、まとめた数(バッチ)とまとめた対象(インスタンス)の両方を出す。
        // まとめたことで減ったドロー数は (インスタンス数 - バッチ数) x そのモデルのメッシュ数
        if (m_FrameStatsFrameCount > 0 && m_FrameStatsInstancedBatchSum > 0)
        {
            const double frames = static_cast<double>(m_FrameStatsFrameCount);
            char instText[192];
            std::snprintf(
                instText, sizeof(instText),
                "  インスタンシング: バッチ %.1f / まとめたインスタンス %.1f [1フレームあたり・2組の合計]",
                static_cast<double>(m_FrameStatsInstancedBatchSum) / frames,
                static_cast<double>(m_FrameStatsInstancedInstanceSum) / frames);
            Core::Logger::Info("Perf", instText);
        }

        // bindless区画の使用状況。**満杯になっても例外は飛ばず、エラーログ1行と
        // kInvalidBindlessIndex(=白1x1へ落ちる)しか残らない**ため、上限へ近づいていることを
        // 定期的に見えるようにしておく(IRHIDevice::GetBindlessUsedCountのコメント参照)
        if (m_Device)
        {
            const uint32_t bindlessCapacity = m_Device->GetBindlessCapacity();
            if (bindlessCapacity > 0)
            {
                const uint32_t bindlessUsed = m_Device->GetBindlessUsedCount();
                char bindlessText[160];
                std::snprintf(
                    bindlessText, sizeof(bindlessText), "  bindless: %u / %u ディスクリプタ (%.1f%%)",
                    bindlessUsed, bindlessCapacity,
                    100.0 * static_cast<double>(bindlessUsed) / static_cast<double>(bindlessCapacity));
                Core::Logger::Info("Perf", bindlessText);
            }
        }

        // メッシュレット単位のカリング(増幅シェーダー)の効き。上のCPU側とは粒度も判定の種類も
        // 違うので別の行に出す。
        //
        // 【オクルージョンを視錐台+コーンと分けて出す】完了条件がここにある ――
        // 俯瞰(遮蔽が少ない)と街路(遮蔽が多い)でオクルージョンの割合に差が出ることが、
        // 判定が実際に効いていることの証拠になる。合算すると視錐台の変動に埋もれて分からない
        if (m_FrameStatsMeshletSampleCount > 0 && m_FrameStatsMeshletTestedSum > 0)
        {
            const double samples = static_cast<double>(m_FrameStatsMeshletSampleCount);
            const double tested = static_cast<double>(m_FrameStatsMeshletTestedSum);
            const double frustumRatio = 100.0 * static_cast<double>(m_FrameStatsMeshletFrustumCulledSum) / tested;
            const double occlusionRatio = 100.0 * static_cast<double>(m_FrameStatsMeshletOcclusionCulledSum) / tested;

            char meshletCullText[256];
            std::snprintf(
                meshletCullText, sizeof(meshletCullText),
                "  メッシュレットカリング: 判定 %.1f / 視錐台+コーン %.1f (%.1f%%) / オクルージョン %.1f (%.1f%%)"
                " [1フレームあたり・%u フレーム分]",
                tested / samples,
                static_cast<double>(m_FrameStatsMeshletFrustumCulledSum) / samples, frustumRatio,
                static_cast<double>(m_FrameStatsMeshletOcclusionCulledSum) / samples, occlusionRatio,
                m_FrameStatsMeshletSampleCount);
            Core::Logger::Info("Perf", meshletCullText);
        }

        // モデル単位のGPUカリング(Stage 5-3)。
        //
        // 【判定数と視錐台の間引き数がCPUと一致することが合格条件】GPUは同じAABBを
        // 同じ視錐台で判定しているので、一致しなければ平面の作り方か候補の積み方が壊れている。
        // **間接描画はこの数を信じて描く**ので、食い違ったまま進むと絵が消えてから
        // 原因を探すことになる。だから食い違いは警告として残す
        if (m_ModelCullTested > 0)
        {
            char modelCullText[256];
            std::snprintf(
                modelCullText, sizeof(modelCullText),
                "  モデル単位GPUカリング: 判定 %u (CPU候補 %u) / 視錐台 %u (CPU %u) / オクルージョン %u / 生存 %u",
                m_ModelCullTested, m_ModelCullComparedCandidateCount,
                m_ModelCullFrustumCulled, m_ModelCullComparedCpuFrustumCulled,
                m_ModelCullOcclusionCulled, m_ModelCullSurvived);
            Core::Logger::Info("Perf", modelCullText);

            // 区画ごとの発行数。**間引きの数だけ見ても、間接描画が本当に描いているかは分からない** ――
            // 描画発行に繋がっていなければここは全部0のままで、絵はCPUループが出している
            char modelCullRegionText[256];
            std::snprintf(
                modelCullRegionText, sizeof(modelCullRegionText),
                "  モデル単位GPU発行(%s): G-Buffer %u+%u / プリパス不透明 %u+%u / プリパスカットアウト %u+%u",
                m_ModelCullIndirectActiveLastFrame ? "間接描画" : "計数のみ",
                m_ModelCullRegionIssued[kModelCullRegionGBuffer],
                m_ModelCullRegionIssued[kModelCullRegionGBufferMirrored],
                m_ModelCullRegionIssued[kModelCullRegionPrepassOpaque],
                m_ModelCullRegionIssued[kModelCullRegionPrepassOpaqueMirrored],
                m_ModelCullRegionIssued[kModelCullRegionPrepassCutout],
                m_ModelCullRegionIssued[kModelCullRegionPrepassCutoutMirrored]);
            Core::Logger::Info("Perf", modelCullRegionText);

            // どの経路で判定したか。**間引き数だけでは切り替わったか分からない** ――
            // カメラが止まっていれば前フレームのHi-Zと今フレームのHi-Zは同じ内容になり、
            // 新旧どちらの経路でも同じ数が出る。経路そのものを出しておく
            char modelCullPathText[192];
            std::snprintf(
                modelCullPathText, sizeof(modelCullPathText),
                "  Hi-Zの出どころ: %s / 判定ディスパッチ: プリパスぶん %u + G-Bufferぶん %u",
                m_HiZFromDepthPrepassLastFrame ? "深度プリパス(今フレーム)" : "G-Bufferの後(前フレーム)",
                m_ModelCullDispatchCounts[0], m_ModelCullDispatchCounts[1]);
            Core::Logger::Info("Perf", modelCullPathText);

            if (m_ModelCullTested != m_ModelCullComparedCandidateCount ||
                m_ModelCullFrustumCulled != m_ModelCullComparedCpuFrustumCulled)
            {
                // 【黙って進めない】食い違ったままExecuteIndirectへ繋ぐと、
                // 絵が消えてから原因を探すことになる
                Core::Logger::Warning(
                    "Perf",
                    "モデル単位GPUカリングの判定がCPUと食い違っています(判定 " +
                        std::to_string(m_ModelCullTested) + " vs " +
                        std::to_string(m_ModelCullComparedCandidateCount) + " / 視錐台 " +
                        std::to_string(m_ModelCullFrustumCulled) + " vs " +
                        std::to_string(m_ModelCullComparedCpuFrustumCulled) + ")");
            }
        }

        m_FrameStatsFrameCount = 0;
        m_FrameStatsCPUTimeSumMs = 0.0;
        m_FrameStatsGPUTimeSumMs = 0.0;
        m_FrameStatsGPUWaitSumMs = 0.0;
        m_FrameStatsWorstFrameTimeMs = 0.0f;
        m_FrameStatsCullTestedSum = 0;
        m_FrameStatsCullCulledSum = 0;
        m_FrameStatsLODSwitchSum = 0;
        m_FrameStatsLODFadingSum = 0;
        m_FrameStatsMeshCullTestedSum = 0;
        m_FrameStatsMeshCullCulledSum = 0;
        m_FrameStatsDrawCallsGBufferSum = 0;
        m_FrameStatsDrawCallsShadowSum = 0;
        m_FrameStatsDrawCallsDepthPrepassSum = 0;
        m_FrameStatsInstancedBatchSum = 0;
        m_FrameStatsInstancedInstanceSum = 0;
        m_FrameStatsMeshletTestedSum = 0;
        m_FrameStatsMeshletFrustumCulledSum = 0;
        m_FrameStatsMeshletOcclusionCulledSum = 0;
        m_FrameStatsMeshletSampleCount = 0;

        // テクスチャの常駐ミップの内訳。**サイズ帯ごとに分けて出す** ――
        // 64KBタイルはBC7で256x256テクセルを覆うため、ミップ/タイル単位の制御が効くのは
        // 大きいテクスチャに偏る。「入れたから減った」ではなくどの帯に効いたかで語るため
        if (m_TextureStreaming.IsEnabled())
        {
            m_TextureStreaming.LogStats("periodic");
        }

        // 【自己申告と実測を並べる】常駐管理が積算したバイト数だけを見ていると、
        // 物差し自体が間違っていても気付けない。OSから見たVRAM使用量と一緒に出す
        uint64_t usedBytes = 0;
        uint64_t budgetBytes = 0;
        if (m_Device->GetVideoMemoryUsage(usedBytes, budgetBytes))
        {
            constexpr double kBytesPerMiB = 1024.0 * 1024.0;
            char vramLine[160];
            std::snprintf(
                vramLine, sizeof(vramLine), "VRAM: 使用 %.1f MB / 予算 %.1f MB",
                static_cast<double>(usedBytes) / kBytesPerMiB, static_cast<double>(budgetBytes) / kBytesPerMiB);
            Core::Logger::Info("Perf", vramLine);
        }
    }
}
