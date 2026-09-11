#include "RenderDumpServiceState.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "Core/Logger.h"
#include "Core/RenderGraph.h"
#include "Core/StringUtil.h"
#include "Core/Window.h"

#include "../Passes/MegaLightsConstants.h"

// 中間レンダーターゲットの生値ダンプ(-dumptex)と、パスマニフェストの書き出し
// (-passmanifest)。設計の意図と、表を毎回作り直す理由は RenderDumpServiceState.h にある
namespace Kurenai::Diagnostics
{
    namespace
    {
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
    }

    void RenderDumpService::ApplyDebugNames(const std::vector<DumpableTexture>& table) const
    {
        uint32_t named = 0;
        for (const DumpableTexture& entry : table)
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

    void RenderDumpService::ApplyDebugNamesIfDirty(const std::vector<DumpableTexture>& table)
    {
        if (!m_DebugNamesDirty)
        {
            return;
        }
        ApplyDebugNames(table);
        m_DebugNamesDirty = false;
    }

    void RenderDumpService::AddTextureDump(
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

    void RenderDumpService::SetTextureDumpFrame(int frame)
    {
        m_TextureDumpFrame = frame;
        Core::Logger::Info(
            "KurenaiEngine3D",
            "テクスチャを書き出すフレームを設定しました: " +
                (frame < 0 ? std::string("既定(") + std::to_string(Passes::kMegaLightsAccumWarmup) + ")"
                           : std::to_string(frame)));
    }

    void RenderDumpService::SetExitAfterDump(bool enabled)
    {
        m_ExitAfterDump = enabled;
        Core::Logger::Info(
            "KurenaiEngine3D",
            std::string("書き出し後の自動終了: ") + (enabled ? "有効" : "無効"));
    }

    void RenderDumpService::IssueTextureDumps(
        Core::RenderGraph& graph, const std::vector<DumpableTexture>& table,
        uint32_t frameIndex, RHI::IRHIDevice& device)
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
            m_TextureDumpFrame >= 0 ? static_cast<uint32_t>(m_TextureDumpFrame) : Passes::kMegaLightsAccumWarmup;
        if (frameIndex < targetFrame)
        {
            return;
        }

        // 【表は毎回作り直す】レンダーターゲットはリサイズやバッファ精度の切り替えで
        // ポインタごと作り直される。キャッシュすると解放済みのテクスチャを指す

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
                (request.AnyIssued && frameIndex - request.LastIssueFrame < request.Stride))
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
            slot.Readback = device.CreateReadbackTexture(found->Texture, request.MipLevel);
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
            slot.CopyFrame = frameIndex;
            slot.SequenceIndex = request.IssuedCount;
            slot.FailedFrames = 0;
            slot.Busy = true;
            ++request.IssuedCount;
            request.LastIssueFrame = frameIndex;
            if (!request.AnyIssued)
            {
                request.FirstIssueFrame = frameIndex;
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

    void RenderDumpService::ResolveTextureDumps(uint32_t frameIndex, Core::Window* window, bool isDX12)
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
            if (request.AnyIssued && static_cast<uint64_t>(frameIndex - request.FirstIssueFrame) > timeout)
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
                if (frameIndex - slot.CopyFrame < kTextureDumpReadDelayFrames)
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
                if (WriteTextureDumpFile(request, slot, pixels, isDX12))
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
            if (window)
            {
                // 【PostQuitMessageではない】あれは**呼び出したスレッドの**キューへWM_QUITを積む。
                // ここはRenderスレッドで、メッセージを汲むのはUpdateスレッド(Run()のPumpMessages)
                // なので、Renderスレッドから呼んでも誰も拾わず永久に終わらない。
                // PostMessageWはスレッド安全にウィンドウのキューへ積める。
                // WM_CLOSEはWindow::HandleMessageがm_ShouldClose=trueにするだけなので、
                // Run()のループが正規の手順で抜ける(スレッドの停止も後始末も普段どおり走る)
                Core::Logger::Info("KurenaiEngine3D", "テクスチャの書き出しが完了したので終了します");
                PostMessageW(window->GetHandle(), WM_CLOSE, 0, 0);
            }
            else
            {
                Core::Logger::Error(
                    "KurenaiEngine3D", "書き出し後の自動終了: ウィンドウが無いため終了要求を出せません");
            }
        }
    }

    bool RenderDumpService::WriteTextureDumpFile(
        const TextureDumpRequest& request, const TextureDumpSlot& slot, const std::vector<uint8_t>& pixels,
        bool isDX12) const
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
        //    32    4   uint32 FrameIndex (frameIndex。複数枚が同一フレームかの照合用)
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
            isDX12 ? 2u : 1u, // Backend
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

    void RenderDumpService::WritePassManifestIfDue(Core::RenderGraph& graph, uint32_t frameIndex, bool isDX12)
    {
        // 1枚だけなら既存のテクスチャダンプと同じフレームを使う。複数枚では焼き込みを捕まえるため最初から出す。
        const uint32_t manifestTargetFrame =
            m_TextureDumpFrame >= 0 ? static_cast<uint32_t>(m_TextureDumpFrame) : Passes::kMegaLightsAccumWarmup;
        const bool writeSingleManifest =
            m_PassManifestTargetFrames == 1 && !m_PassManifestIssued && frameIndex >= manifestTargetFrame;
        const bool writeManifestSequence =
            m_PassManifestTargetFrames > 1 && m_PassManifestIssuedFrames < m_PassManifestTargetFrames;
        if (!m_PassManifestPath.empty() && (writeSingleManifest || writeManifestSequence))
        {
            const uint32_t manifestSequenceIndex = m_PassManifestIssuedFrames;
            m_PassManifestIssued = true;
            ++m_PassManifestIssuedFrames;
            std::string executionOrderError;
            const std::string manifest = graph.BuildPassManifest(&executionOrderError);
            if (!executionOrderError.empty())
            {
                Core::Logger::Error("KurenaiEngine3D", "パスマニフェストの実行順を解決できませんでした: " + executionOrderError);
            }

            const std::wstring outputPath = MakeTextureDumpSequencePath(
                m_PassManifestPath, m_PassManifestTargetFrames, manifestSequenceIndex);
            std::ofstream file(outputPath, std::ios::binary);
            if (!file)
            {
                Core::Logger::Error(
                    "KurenaiEngine3D", "パスマニフェストを書き出せませんでした(ファイルを開けない): " +
                        Core::WideToUtf8(outputPath));
            }
            else
            {
                file.write(manifest.data(), static_cast<std::streamsize>(manifest.size()));
                if (!file)
                {
                    Core::Logger::Error(
                        "KurenaiEngine3D", "パスマニフェストを書き出せませんでした(書き込み失敗): " +
                            Core::WideToUtf8(outputPath));
                }
                else
                {
                    Core::Logger::Info(
                        "KurenaiEngine3D", "パスマニフェストを書き出しました: " + Core::WideToUtf8(outputPath));
                }
            }
        }
    }

    void RenderDumpService::SetPassManifest(const wchar_t* path, int frames)
    {
        if (path == nullptr || path[0] == L'\0' || frames < 1)
        {
            m_PassManifestPath.clear();
            m_PassManifestTargetFrames = 1;
            m_PassManifestIssuedFrames = 0;
            m_PassManifestIssued = false;
            Core::Logger::Error("KurenaiEngine3D", "パスマニフェストの出力設定が不正です");
            return;
        }

        m_PassManifestPath = path;
        m_PassManifestTargetFrames = static_cast<uint32_t>(frames);
        m_PassManifestIssuedFrames = 0;
        m_PassManifestIssued = false;
        Core::Logger::Info(
            "KurenaiEngine3D", "パスマニフェストの出力を設定しました: " + Core::WideToUtf8(path) +
                " (frames=" + std::to_string(frames) + ")");
    }
}
