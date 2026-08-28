#include "TextureStreaming.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>

#include "Core/Logger.h"
#include "Core/StringUtil.h"
#include "Scene.h"

using namespace DirectX;

namespace Kurenai::Assets
{
    namespace
    {
        using Core::WideToUtf8;

        // 1フレームで確定(ディスクリプタ書き換え)する上限。
        // 書き換え自体は数百ナノ秒だが、1件ごとに古いリソースが遅延解放キューへ積まれるため
        // 際限なく通すとVRAMの解放が追いつかない
        constexpr uint32_t kMaxCommitsPerFrame = 8;

        // 同時に走らせる要求の上限。ワーカーは1件ずつGPUリソースを作る(内部でGPU同期待ちがある)ので、
        // 積みすぎても待ち行列が伸びるだけでVRAMの山が高くなる
        constexpr size_t kMaxPendingRequests = 32;

        // 1フレームで見直すエントリ数の分母。全件をこの回数のフレームに分けて走査する
        constexpr size_t kScanFramesPerSweep = 8;

        // 粗くする(常駐ミップを削る)前に、その状態が続いていることを確かめる秒数。
        // パタつくとA/B比較のたびに絵が変わって計測が濁る
        constexpr float kCoarserHoldSeconds = 2.0f;

        // Worldから代表スケールを取り出す。ModelInstance::Worldは
        // HLSLのmul(vec, matrix)規約に合わせて転置して格納されている(SceneLoader参照)ため、
        // 基底ベクトルを取り出すには一度戻す
        float ExtractUniformScale(const XMFLOAT4X4& storedWorld)
        {
            const XMMATRIX world = XMMatrixTranspose(XMLoadFloat4x4(&storedWorld));
            const float sx = XMVectorGetX(XMVector3Length(world.r[0]));
            const float sy = XMVectorGetX(XMVector3Length(world.r[1]));
            const float sz = XMVectorGetX(XMVector3Length(world.r[2]));
            // 非一様スケールでも桁を外さないよう平均を代表値にする
            const float average = (sx + sy + sz) / 3.0f;
            return average > 1e-6f ? average : 1.0f;
        }

        // 点からAABBまでの最近接距離。中心距離だと細長いメッシュ(街路の壁など)で破綻する
        float DistanceToAABB(const XMFLOAT3& point, const float boundsMin[3], const float boundsMax[3])
        {
            const float p[3] = { point.x, point.y, point.z };
            float sum = 0.0f;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float d = std::max({ boundsMin[axis] - p[axis], 0.0f, p[axis] - boundsMax[axis] });
                sum += d * d;
            }
            return std::sqrt(sum);
        }

        // ローカルAABBの8頂点をWorldで変換し、その包絡(ワールド空間のAABB)を求める。
        // min/maxだけを変換すると回転時に不正確になるため、必ず8頂点すべてを変換する
        // (SceneLoaderがインスタンスのAABBを求めているのと同じ手順)
        void TransformBoundsToWorld(
            const float localMin[3], const float localMax[3], const XMFLOAT4X4& storedWorld,
            float outMin[3], float outMax[3])
        {
            const XMMATRIX world = XMMatrixTranspose(XMLoadFloat4x4(&storedWorld));
            bool initialized = false;
            for (int corner = 0; corner < 8; ++corner)
            {
                const XMVECTOR localCorner = XMVectorSet(
                    (corner & 1) ? localMax[0] : localMin[0],
                    (corner & 2) ? localMax[1] : localMin[1],
                    (corner & 4) ? localMax[2] : localMin[2],
                    1.0f);
                XMFLOAT3 transformed;
                XMStoreFloat3(&transformed, XMVector3TransformCoord(localCorner, world));

                const float xyz[3] = { transformed.x, transformed.y, transformed.z };
                for (int axis = 0; axis < 3; ++axis)
                {
                    if (!initialized)
                    {
                        outMin[axis] = xyz[axis];
                        outMax[axis] = xyz[axis];
                    }
                    else
                    {
                        outMin[axis] = std::min(outMin[axis], xyz[axis]);
                        outMax[axis] = std::max(outMax[axis], xyz[axis]);
                    }
                }
                initialized = true;
            }
        }
    }

    const char* TextureStreamingManager::GetSizeBandName(size_t band)
    {
        switch (band)
        {
        case 0: return "  <=128";
        case 1: return "    256";
        case 2: return "    512";
        case 3: return "   1024";
        default: return ">=2048";
        }
    }

    size_t TextureStreamingManager::SizeBandOf(const RHI::PackedTextureInfo& info)
    {
        const uint32_t longest = std::max(info.Width, info.Height);
        for (size_t band = 0; band + 1 < kSizeBandCount; ++band)
        {
            if (longest <= kSizeBandMax[band])
            {
                return band;
            }
        }
        return kSizeBandCount - 1;
    }

    TextureStreamingManager::TextureStreamingManager() = default;

    TextureStreamingManager::~TextureStreamingManager()
    {
        Reset();
    }

    void TextureStreamingManager::Configure(bool enabled, float mipBias)
    {
        m_Enabled = enabled;
        m_MipBias = mipBias;
    }

    void TextureStreamingManager::Build(const Scene& scene, RHI::IRHIDevice& device)
    {
        Reset();

        // 【無効のときも累計をここで0に戻す】戻さないと、シーンを無効設定で読み直したあとも
        // 前のシーンの「差し替え累計」が残り、「効いていない」ことを確かめる対照実験で
        // 0件かどうかを見る手がかりが濁る
        {
            std::lock_guard<std::mutex> lock(m_StatsMutex);
            m_CommittedUpdates = 0;
            m_FailedUpdates = 0;
            m_InFlightCount = 0;
        }

        if (!m_Enabled)
        {
            return;
        }

        // IRHITexture* から追跡表の添字を引く。1つの.ktexは1つのModelの中でしか
        // 共有されないが、Model単位では複数メッシュから参照される
        std::unordered_map<const RHI::IRHITexture*, size_t> textureToEntry;

        for (const ModelInstance& instance : scene.Instances)
        {
            // --scaleで縮めたモデルのUV密度をワールド空間の値へ直すのに使う
            const float uniformScale = ExtractUniformScale(instance.World);

            // 【実体が無いことがある】ModelInstance::Modelはshared_ptrで、[Scene]StreamingDistanceを
            // 使うシーンでは読み込み時点で空。まだ読まれていないモデルのテクスチャは追跡できない
            // (Buildはシーン読み込み時の1回だけなので、後から常駐したモデルは全ミップのままになる)
            if (!instance.Model)
            {
                continue;
            }

            const Model& model = *instance.Model;
            if (model.Textures.size() != model.TexturePaths.size())
            {
                Core::Logger::Error(
                    "TextureStreaming",
                    "テクスチャとパスの数が一致しません(テクスチャ " + std::to_string(model.Textures.size()) +
                        " / パス " + std::to_string(model.TexturePaths.size()) + ")。このモデルは追跡しません");
                continue;
            }

            // このモデルのテクスチャを追跡表へ登録する(まだ登録されていなければ)
            for (size_t t = 0; t < model.Textures.size(); ++t)
            {
                const RHI::IRHITexture* texture = model.Textures[t].get();
                if (texture == nullptr || textureToEntry.count(texture) != 0)
                {
                    continue;
                }

                RHI::PackedTextureInfo packedInfo{};
                if (!RHI::TextureImage::TryReadPackedTextureInfo(model.TexturePaths[t], packedInfo))
                {
                    // ヘッダが読めないものは触らない(全ミップ常駐のまま)
                    continue;
                }
                if (!packedInfo.SupportsPartialMipLoad)
                {
                    continue;
                }

                Entry entry;
                entry.Texture = model.Textures[t].get();
                entry.Path = model.TexturePaths[t];
                entry.Info = packedInfo;
                // Buildの時点では全ミップが載っている(ModelLoaderがそう読んでいる)
                entry.ResidentFirstMip = 0;
                entry.RequestedFirstMip = 0;
                textureToEntry.emplace(texture, m_Entries.size());
                m_Entries.push_back(std::move(entry));
            }

            // メッシュ→テクスチャの参照を、**メッシュ単位で**登録する。
            // インスタンス単位にまとめると、街区全体を覆う1インスタンスの内側にカメラが
            // 入ったときに距離が0になって効かなくなる(Entry::Refのコメント参照)
            for (const Mesh& mesh : model.Meshes)
            {
                if (mesh.UVPerLocalMeter <= 0.0f)
                {
                    // UV密度を見積もれなかったメッシュは、そのテクスチャの常駐ミップを削らせない
                    continue;
                }

                const RHI::IRHITexture* const referenced[] = {
                    mesh.BaseColorTexture, mesh.NormalTexture, mesh.MetallicRoughnessTexture,
                    mesh.EmissiveTexture, mesh.OcclusionTexture, mesh.BentNormalTexture,
                };

                Entry::Ref ref;
                TransformBoundsToWorld(mesh.BoundsMin, mesh.BoundsMax, instance.World, ref.BoundsMin, ref.BoundsMax);
                ref.UVPerWorldMeter = mesh.UVPerLocalMeter / uniformScale;

                for (const RHI::IRHITexture* texture : referenced)
                {
                    const auto found = textureToEntry.find(texture);
                    if (found == textureToEntry.end())
                    {
                        continue;
                    }
                    m_Entries[found->second].Refs.push_back(ref);
                }
            }
        }

        // 参照されていないテクスチャ(UV密度を見積もれないメッシュからしか使われていない等)は
        // 追跡しても目標が決まらないので落とす
        m_Entries.erase(
            std::remove_if(m_Entries.begin(), m_Entries.end(), [](const Entry& e) { return e.Refs.empty(); }),
            m_Entries.end());

        m_ScanCursor = 0;
        m_TiledResourcesTier = device.GetTiledResourcesTier();
        m_TileState.assign(m_Entries.size(), kTileUnknown);

        if (m_Entries.empty())
        {
            Core::Logger::Info("TextureStreaming", "追跡対象のテクスチャがありません(常駐ミップ制御は働きません)");
            return;
        }

        m_StopRequested = false;
        m_Worker = std::thread(&TextureStreamingManager::WorkerMain, this, &device);

        LogStats("build");
    }

    void TextureStreamingManager::Reset()
    {
        StopWorker();

        {
            std::lock_guard<std::mutex> lock(m_ReadyMutex);
            m_Ready.clear();
        }
        {
            std::lock_guard<std::mutex> lock(m_RequestMutex);
            m_Requests.clear();
        }
        m_Entries.clear();
        m_TileState.clear();
        m_ScanCursor = 0;
    }

    void TextureStreamingManager::StopWorker()
    {
        if (!m_Worker.joinable())
        {
            return;
        }

        m_StopRequested = true;
        m_RequestCV.notify_all();
        m_Worker.join();
        m_StopRequested = false;
    }

    void TextureStreamingManager::WorkerMain(RHI::IRHIDevice* device)
    {
        // COMを使うのはDirectXTexのWICデコードだけで、.ktexの読み込み経路では走らないが、
        // ModelLoaderのワーカーと同じく念のため初期化しておく(失敗しても続行する)
        for (;;)
        {
            Request request;
            {
                std::unique_lock<std::mutex> lock(m_RequestMutex);
                m_RequestCV.wait(lock, [this] { return m_StopRequested || !m_Requests.empty(); });
                if (m_StopRequested)
                {
                    return;
                }
                request = m_Requests.front();
                m_Requests.pop_front();
            }

            // EntryのPath/Textureはビルド後に変わらないため、ロック無しで読んでよい
            const Entry& entry = m_Entries[request.EntryIndex];

            ReadyItem ready;
            ready.EntryIndex = request.EntryIndex;
            ready.FirstMip = request.FirstMip;
            try
            {
                RHI::TextureImage image =
                    RHI::TextureImage::LoadFromPackedTexture(entry.Path, request.FirstMip);

                // タイルリソース経路を先に試す。リソースもSRV番号もbindless番号も変えずに
                // 済むぶんこちらが軽い。乗らない形(標準ミップが1段も無い=全部ミップテール)なら
                // nullptrが返るので、リソースごと作り直す従来経路へ落とす。
                // m_TileStateはこのワーカースレッドだけが書き換える
                if (m_TiledResourcesTier > 0 && m_TileState[request.EntryIndex] != kTileUnavailable)
                {
                    RHI::TiledTextureDesc tiledDesc{};
                    tiledDesc.Width = entry.Info.Width;
                    tiledDesc.Height = entry.Info.Height;
                    tiledDesc.MipLevels = entry.Info.MipLevels;
                    tiledDesc.DxgiFormat = entry.Info.Format;
                    ready.Pending =
                        device->PrepareTiledTextureResidency(entry.Texture, tiledDesc, image, request.FirstMip);
                    m_TileState[request.EntryIndex] = ready.Pending ? kTileInUse : kTileUnavailable;
                }

                if (!ready.Pending)
                {
                    ready.Pending = device->PrepareTextureContents(entry.Texture, image);
                }
            }
            catch (const std::exception& e)
            {
                Core::Logger::Error(
                    "TextureStreaming",
                    "常駐ミップの読み直しに失敗しました (" + WideToUtf8(entry.Path) + "): " + e.what());
                ready.Pending.reset();
            }

            {
                std::lock_guard<std::mutex> lock(m_ReadyMutex);
                m_Ready.push_back(std::move(ready));
            }
        }
    }

    uint32_t TextureStreamingManager::CommitReady(RHI::IRHIDevice& device)
    {
        if (m_Entries.empty())
        {
            return 0;
        }

        // GetStatsはデバイスを受け取らないため、触れるここで拾って控える
        device.GetTilePoolUsage(m_TilePoolReservedBytes, m_TilePoolUsedBytes);

        std::vector<ReadyItem> batch;
        {
            std::lock_guard<std::mutex> lock(m_ReadyMutex);
            if (m_Ready.empty())
            {
                return 0;
            }
            const size_t take = std::min<size_t>(m_Ready.size(), kMaxCommitsPerFrame);
            batch.reserve(take);
            for (size_t i = 0; i < take; ++i)
            {
                batch.push_back(std::move(m_Ready[i]));
            }
            m_Ready.erase(m_Ready.begin(), m_Ready.begin() + static_cast<ptrdiff_t>(take));
        }

        uint32_t committed = 0;
        uint32_t failed = 0;
        for (ReadyItem& item : batch)
        {
            Entry& entry = m_Entries[item.EntryIndex];
            entry.InFlight = false;

            if (!item.Pending)
            {
                ++failed;
                continue;
            }
            if (!device.CommitTextureContents(item.Pending.get()))
            {
                ++failed;
                continue;
            }

            entry.ResidentFirstMip = item.FirstMip;
            ++committed;
        }

        {
            std::lock_guard<std::mutex> lock(m_StatsMutex);
            m_CommittedUpdates += committed;
            m_FailedUpdates += failed;
            m_InFlightCount -= std::min<uint32_t>(m_InFlightCount, committed + failed);
        }
        return committed;
    }

    void TextureStreamingManager::UpdateTargets(
        const XMFLOAT3& cameraPosition, float tanHalfFovY, uint32_t screenHeight, float deltaTime)
    {
        if (m_Entries.empty() || screenHeight == 0 || tanHalfFovY <= 0.0f)
        {
            return;
        }

        // 画面のピクセル密度。距離dのところで1メートルが何ピクセルに写るか
        const float pixelsPerMeterAtOneMeter = static_cast<float>(screenHeight) / (2.0f * tanHalfFovY);

        const size_t sweep = std::max<size_t>(1, (m_Entries.size() + kScanFramesPerSweep - 1) / kScanFramesPerSweep);
        std::vector<Request> newRequests;

        for (size_t processed = 0; processed < sweep; ++processed)
        {
            if (m_ScanCursor >= m_Entries.size())
            {
                m_ScanCursor = 0;
            }
            Entry& entry = m_Entries[m_ScanCursor++];

            // 無効にしている間は全ミップ常駐へ戻す(A/Bの対照を同じ起動の中で取るため)。
            // 追跡表は壊さないので、もう一度有効にすればそのまま効き始める
            if (!m_Active)
            {
                if (!entry.InFlight && entry.ResidentFirstMip != 0)
                {
                    entry.InFlight = true;
                    entry.RequestedFirstMip = 0;
                    newRequests.push_back(Request{ m_ScanCursor - 1, 0 });
                }
                entry.CoarserHoldSeconds = 0.0f;
                continue;
            }

            // このテクスチャを参照している中で最も細かさを要求する参照に合わせる
            // (粗い方に合わせると、近くで見ている参照がぼける)
            uint32_t desiredFirstMip = entry.Info.MipLevels - 1;
            const float textureExtent =
                std::sqrt(static_cast<float>(entry.Info.Width) * static_cast<float>(entry.Info.Height));
            for (const Entry::Ref& ref : entry.Refs)
            {
                const float distance =
                    std::max(0.01f, DistanceToAABB(cameraPosition, ref.BoundsMin, ref.BoundsMax));

                // テクセル密度[texels/m]
                const float texelsPerMeter = ref.UVPerWorldMeter * textureExtent;
                const float pixelsPerMeter = pixelsPerMeterAtOneMeter / distance;
                if (texelsPerMeter <= 0.0f || pixelsPerMeter <= 0.0f)
                {
                    desiredFirstMip = 0;
                    break;
                }

                const float levels = std::log2(texelsPerMeter / pixelsPerMeter) + m_MipBias;
                const int32_t clamped = static_cast<int32_t>(std::floor(levels));
                const uint32_t candidate = clamped <= 0
                    ? 0u
                    : std::min<uint32_t>(static_cast<uint32_t>(clamped), entry.Info.MipLevels - 1);
                desiredFirstMip = std::min(desiredFirstMip, candidate);
            }

            // ヒステリシス: 詳細化(段を下げる)は即座、粗化(段を上げる)は一定時間続いてから1段ずつ
            uint32_t target = entry.ResidentFirstMip;
            if (desiredFirstMip < entry.ResidentFirstMip)
            {
                target = desiredFirstMip;
                entry.CoarserHoldSeconds = 0.0f;
            }
            else if (desiredFirstMip > entry.ResidentFirstMip)
            {
                entry.CoarserHoldSeconds += deltaTime * static_cast<float>(kScanFramesPerSweep);
                if (entry.CoarserHoldSeconds >= kCoarserHoldSeconds)
                {
                    target = entry.ResidentFirstMip + 1;
                    entry.CoarserHoldSeconds = 0.0f;
                }
            }
            else
            {
                entry.CoarserHoldSeconds = 0.0f;
            }

            if (entry.InFlight || target == entry.ResidentFirstMip)
            {
                continue;
            }

            entry.InFlight = true;
            entry.RequestedFirstMip = target;
            newRequests.push_back(Request{ m_ScanCursor - 1, target });
        }

        if (newRequests.empty())
        {
            return;
        }

        uint32_t accepted = 0;
        {
            std::lock_guard<std::mutex> lock(m_RequestMutex);
            for (Request& request : newRequests)
            {
                if (m_Requests.size() >= kMaxPendingRequests)
                {
                    // 積みきれなかったぶんは次の走査で拾い直す
                    m_Entries[request.EntryIndex].InFlight = false;
                    continue;
                }
                m_Requests.push_back(request);
                ++accepted;
            }
        }
        if (accepted > 0)
        {
            {
                std::lock_guard<std::mutex> lock(m_StatsMutex);
                m_InFlightCount += accepted;
            }
            m_RequestCV.notify_all();
        }
    }

    TextureStreamingManager::Stats TextureStreamingManager::GetStats() const
    {
        Stats stats;
        stats.Enabled = m_Enabled;
        stats.TrackedTextures = static_cast<uint32_t>(m_Entries.size());
        {
            std::lock_guard<std::mutex> lock(m_RequestMutex);
            stats.PendingRequests = static_cast<uint32_t>(m_Requests.size());
        }
        {
            std::lock_guard<std::mutex> lock(m_StatsMutex);
            stats.CommittedUpdates = m_CommittedUpdates;
            stats.FailedUpdates = m_FailedUpdates;
            stats.InFlight = m_InFlightCount;
        }

        for (size_t band = 0; band < kSizeBandCount; ++band)
        {
            stats.Bands[band].MinResidentMips = UINT32_MAX;
        }

        stats.TiledResourcesTier = m_TiledResourcesTier;
        stats.TilePoolReservedBytes = m_TilePoolReservedBytes;
        stats.TilePoolUsedBytes = m_TilePoolUsedBytes;

        for (size_t index = 0; index < m_Entries.size(); ++index)
        {
            const Entry& entry = m_Entries[index];
            const size_t band = SizeBandOf(entry.Info);
            SizeBandStats& bandStats = stats.Bands[band];

            // m_TileStateはワーカースレッドが書くが、統計表示のための読み出しなので
            // 1フレーム古い値を拾っても支障は無い
            if (index < m_TileState.size() && m_TileState[index] == kTileInUse)
            {
                ++bandStats.TiledCount;
                ++stats.TiledTextures;
            }

            const uint64_t residentBytes = RHI::TextureImage::ComputeMipChainBytes(entry.Info, entry.ResidentFirstMip);
            const uint64_t fullBytes = RHI::TextureImage::ComputeMipChainBytes(entry.Info, 0);
            const uint32_t residentMips = entry.Info.MipLevels - entry.ResidentFirstMip;

            ++bandStats.TextureCount;
            bandStats.ResidentBytes += residentBytes;
            bandStats.FullBytes += fullBytes;
            bandStats.MinResidentMips = std::min(bandStats.MinResidentMips, residentMips);
            bandStats.MaxResidentMips = std::max(bandStats.MaxResidentMips, residentMips);
            bandStats.SumResidentMips += residentMips;
            bandStats.SumDroppedMips += entry.ResidentFirstMip;

            stats.ResidentBytes += residentBytes;
            stats.FullBytes += fullBytes;
        }

        for (size_t band = 0; band < kSizeBandCount; ++band)
        {
            if (stats.Bands[band].TextureCount == 0)
            {
                stats.Bands[band].MinResidentMips = 0;
            }
        }
        return stats;
    }

    void TextureStreamingManager::LogStats(const char* label) const
    {
        const Stats stats = GetStats();
        const std::string prefix = std::string("[") + label + "] ";

        if (!stats.Enabled)
        {
            Core::Logger::Info("TextureStreaming", prefix + "無効(全ミップ常駐)");
            return;
        }

        const double residentMB = static_cast<double>(stats.ResidentBytes) / (1024.0 * 1024.0);
        const double fullMB = static_cast<double>(stats.FullBytes) / (1024.0 * 1024.0);
        const double ratio = stats.FullBytes > 0
            ? 100.0 * static_cast<double>(stats.ResidentBytes) / static_cast<double>(stats.FullBytes)
            : 0.0;

        char summary[256];
        std::snprintf(
            summary, sizeof(summary),
            "追跡 %u枚 / 常駐 %.1f MB / 全ミップなら %.1f MB (%.1f%%) / 差し替え累計 %llu件 (失敗 %llu件)",
            stats.TrackedTextures, residentMB, fullMB, ratio,
            static_cast<unsigned long long>(stats.CommittedUpdates),
            static_cast<unsigned long long>(stats.FailedUpdates));
        Core::Logger::Info("TextureStreaming", prefix + summary);

        // 【タイルリソースがどこに効いているか】64KBタイルはBC7で256x256テクセルを覆うため、
        // 標準ミップを持てない小さいテクスチャは全部がミップテールになり、この経路に乗れない。
        // 「入れたから減った」ではなく、乗れた枚数とプールの実サイズで語る
        char tileLine[224];
        std::snprintf(
            tileLine, sizeof(tileLine),
            "タイルリソース: Tier %u / 経路に乗ったテクスチャ %u枚 / プール確保 %.1f MB (うち使用 %.1f MB)",
            stats.TiledResourcesTier, stats.TiledTextures,
            static_cast<double>(stats.TilePoolReservedBytes) / (1024.0 * 1024.0),
            static_cast<double>(stats.TilePoolUsedBytes) / (1024.0 * 1024.0));
        Core::Logger::Info("TextureStreaming", prefix + tileLine);

        // 【サイズ帯ごとに分けて出す】64KBタイルはBC7で256x256テクセルを覆うため、
        // タイル単位の制御が効くのは大きいテクスチャだけになる。
        // 「入れたから減った」ではなく「どの帯にどれだけ効いたか」で語るための内訳
        Core::Logger::Info("TextureStreaming", prefix + "サイズ帯   枚数  タイル   常駐MB  全ミップMB   常駐率  常駐ミップ(最小/平均/最大)  落とした段の平均");
        for (size_t band = 0; band < kSizeBandCount; ++band)
        {
            const SizeBandStats& b = stats.Bands[band];
            if (b.TextureCount == 0)
            {
                continue;
            }
            char line[256];
            std::snprintf(
                line, sizeof(line),
                "%s  %5u  %6u  %7.1f  %9.1f  %6.1f%%  %3u / %5.2f / %3u  %5.2f",
                GetSizeBandName(band), b.TextureCount, b.TiledCount,
                static_cast<double>(b.ResidentBytes) / (1024.0 * 1024.0),
                static_cast<double>(b.FullBytes) / (1024.0 * 1024.0),
                b.FullBytes > 0 ? 100.0 * static_cast<double>(b.ResidentBytes) / static_cast<double>(b.FullBytes) : 0.0,
                b.MinResidentMips,
                static_cast<double>(b.SumResidentMips) / static_cast<double>(b.TextureCount),
                b.MaxResidentMips,
                static_cast<double>(b.SumDroppedMips) / static_cast<double>(b.TextureCount));
            Core::Logger::Info("TextureStreaming", prefix + line);
        }
    }
}
