#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <DirectXMath.h>

#include "KurenaiTypes.h"

#include "RHI/IRHIDevice.h"
#include "RHI/TextureImage.h"

#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai::Assets
{
    struct Scene;

    // テクスチャの常駐ミップ制御。
    //
    // 【何をするか】カメラからの距離とメッシュのUV密度から「このテクスチャは何段目のミップから
    // 常駐していれば足りるか」をCPUで見積もり、足りているぶんだけを.ktexから読み直して
    // GPUリソースを小さく作り直す。DDSはミップ0を先頭に降順で連続しているため、
    // 目的のミップへシークするだけで読み飛ばせる(TextureImage::LoadFromPackedTexture)。
    //
    // 【なぜCPUで見積もるのか(Sampler Feedbackを使わない理由)】
    //   1. フィードバック用リソースがテクスチャごとに要る。PLATEAU LOD2は33,353枚ある
    //   2. GPUからの読み戻しに数フレームかかり、都市を高速で横断すると追従しない
    //   3. DX12専用で、DX11でも性能の基準機(Intel UHD Graphics 620)でも動かない
    // CPU推定なら決定的で追いやすく、両バックエンドで効く。
    // 詳細と推定式の根拠は docs/ImplementationDetail.md を参照。
    //
    // 【スレッド】
    //   Renderスレッド: UpdateTargets(目標ミップの更新と要求の発行) / CommitReady(差し替えの確定)
    //   専用ワーカー   : .ktexの部分読み出しとGPUリソースの作成(IRHIDevice::PrepareTextureContents)
    // ディスクリプタを書き換えるのはCommitReadyだけで、これはRenderスレッドがそのフレームで
    // 最初にSetTextureを呼ぶより前に実行する。理由はIRHIDevice::PrepareTextureContentsのコメント。
    class KURENAI_LIB_API TextureStreamingManager
    {
    public:
        // 解像度のサイズ帯。タイルリソース(64KBタイル=BC7で256x256テクセル)が効くのは
        // 大きいテクスチャだけなので、効果を必ずこの帯ごとに分けて報告する
        static constexpr size_t kSizeBandCount = 5;
        // 各帯の上限(長辺のピクセル数)。最後の帯は上限なし
        static constexpr uint32_t kSizeBandMax[kSizeBandCount - 1] = { 128, 256, 512, 1024 };
        static const char* GetSizeBandName(size_t band);

        struct SizeBandStats
        {
            uint32_t TextureCount = 0;
            uint64_t ResidentBytes = 0;
            uint64_t FullBytes = 0;
            // 常駐ミップ「段数」の分布(全ミップならMipLevels、mip0を落とせばMipLevels-1)
            uint32_t MinResidentMips = 0;
            uint32_t MaxResidentMips = 0;
            uint64_t SumResidentMips = 0;
            // 落とせているミップ0側の段数の合計(FirstMipの総和)。0なら一段も削れていない
            uint64_t SumDroppedMips = 0;
        };

        struct Stats
        {
            bool Enabled = false;
            uint32_t TrackedTextures = 0;
            uint32_t PendingRequests = 0;
            uint32_t InFlight = 0;
            uint64_t ResidentBytes = 0;
            uint64_t FullBytes = 0;
            // 差し替えの累計。**0なら一度も実行されていない**(対照実験で最初に潰す項目)
            uint64_t CommittedUpdates = 0;
            uint64_t FailedUpdates = 0;
            SizeBandStats Bands[kSizeBandCount];
        };

        TextureStreamingManager();
        ~TextureStreamingManager();

        TextureStreamingManager(const TextureStreamingManager&) = delete;
        TextureStreamingManager& operator=(const TextureStreamingManager&) = delete;

        // .ksceneの[Scene]セクションから読んだ設定を反映する。
        // enabled=falseなら以降Buildしても何も追跡しない(従来どおり全ミップ常駐)
        void Configure(bool enabled, float mipBias);
        bool IsEnabled() const { return m_Enabled; }
        float GetMipBias() const { return m_MipBias; }
        void SetMipBias(float bias) { m_MipBias = bias; }

        // 追跡表が組まれているか(Build済みで対象が1枚以上ある)
        bool IsBuilt() const { return !m_Entries.empty(); }

        // 常駐ミップ制御を実際に効かせるか。falseの間はすべてのテクスチャを
        // 全ミップ常駐へ戻す方向へ目標を置く。
        //
        // 【A/B比較のためにある】「効かせない状態」を追跡表を壊さずに作れないと、
        // 同じ起動の中で off と on を比べられない。falseにしたあと常駐率が100%へ
        // 戻りきってから測ることで、対照(A)を同じ手順・同じ露出で取れる
        bool IsActive() const { return m_Active; }
        void SetActive(bool active) { m_Active = active; }

        // シーン読み込み直後に一度呼び、追跡表を作ってワーカーを起こす。
        // sceneはこのマネージャより長生きしなければならない(Reset()で参照を切る)
        void Build(const Scene& scene, RHI::IRHIDevice& device);
        // ワーカーを止め、追跡表を捨てる。**シーンを破棄する前に必ず呼ぶこと**
        // (ワーカーがIRHITexture*を掴んだままになるため)
        void Reset();

        // Renderスレッド: 準備の整った差し替えを確定する。
        // **そのフレームで最初にSetTextureを呼ぶより前に**呼ぶこと
        uint32_t CommitReady(RHI::IRHIDevice& device);

        // Renderスレッド: 目標ミップを更新し、差のあるものを要求としてワーカーへ積む。
        // tanHalfFovY = tan(垂直画角 / 2)、screenHeight = 内部レンダー解像度の高さ
        void UpdateTargets(
            const DirectX::XMFLOAT3& cameraPosition, float tanHalfFovY, uint32_t screenHeight, float deltaTime);

        Stats GetStats() const;
        // 起動時の1回と、サイズ帯ごとの内訳をログへ出す
        void LogStats(const char* label) const;

    private:
        // 1つの.ktexにつき1件。Buildで作ったあと、Path/Texture/Info/Refsは変更しない
        struct Entry
        {
            RHI::IRHITexture* Texture = nullptr;
            std::wstring Path;
            RHI::PackedTextureInfo Info{};
            // このテクスチャを参照しているメッシュ。**インスタンス単位ではなくメッシュ単位で持つ。**
            //
            // 【なぜメッシュ単位か】1つのモデルが街区全体を覆うことがあり(Bistro Exteriorは
            // 132メッシュで1インスタンス / AABB 109x32x115m)、カメラがその内側に入ると
            // インスタンスまでの距離が全参照で0になって「常に最大解像度が要る」としか
            // 言えなくなる。実際にこれで差し替えが1件も走らなかった。
            // PLATEAUのLOD2タイル(1.1km四方)で街路に降りたときも同じことが起きる
            struct Ref
            {
                // ワールド空間のAABB(読み込み時にメッシュのローカルAABBをWorldで変換して確定)
                float BoundsMin[3] = { 0.0f, 0.0f, 0.0f };
                float BoundsMax[3] = { 0.0f, 0.0f, 0.0f };
                // ワールド1メートルあたりのUV単位(モデルのスケールを織り込み済み)
                float UVPerWorldMeter = 0.0f;
            };
            std::vector<Ref> Refs;

            // 以下はRenderスレッドだけが読み書きする
            uint32_t ResidentFirstMip = 0;
            uint32_t RequestedFirstMip = 0;
            bool InFlight = false;
            // 粗くする(FirstMipを増やす)側だけに掛けるヒステリシス。
            // 詳細化は即座、粗化は一定時間その状態が続いてから1段ずつ
            float CoarserHoldSeconds = 0.0f;
        };

        struct Request
        {
            size_t EntryIndex = 0;
            uint32_t FirstMip = 0;
        };

        struct ReadyItem
        {
            size_t EntryIndex = 0;
            uint32_t FirstMip = 0;
            // nullptrなら失敗(読み込みかGPUリソース作成でこけた)
            std::unique_ptr<RHI::IRHIPendingTextureContents> Pending;
        };

        void WorkerMain(RHI::IRHIDevice* device);
        void StopWorker();
        static size_t SizeBandOf(const RHI::PackedTextureInfo& info);

        bool m_Enabled = false;
        bool m_Active = true;
        float m_MipBias = 0.0f;

        std::vector<Entry> m_Entries;
        // 毎フレーム全件を走査すると数万件では効かないため、フレームごとに一部だけ見る。
        // 目標ミップは距離に対して緩やかにしか変わらないので、数フレームの遅れは見えない
        size_t m_ScanCursor = 0;

        std::deque<Request> m_Requests;
        mutable std::mutex m_RequestMutex;
        std::condition_variable m_RequestCV;

        std::vector<ReadyItem> m_Ready;
        mutable std::mutex m_ReadyMutex;

        std::thread m_Worker;
        std::atomic<bool> m_StopRequested{ false };

        // 統計(Renderスレッドが更新、UIスレッド相当が読む)
        mutable std::mutex m_StatsMutex;
        uint64_t m_CommittedUpdates = 0;
        uint64_t m_FailedUpdates = 0;
        uint32_t m_InFlightCount = 0;
    };
}

#pragma warning(pop)
