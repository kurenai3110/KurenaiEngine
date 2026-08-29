#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <DirectXMath.h>

#include "KurenaiTypes.h"

#include "RHI/IRHIDevice.h"
#include "RHI/TextureImage.h"

#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai::Assets
{
    struct Model;
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
    //   Renderスレッド: Build / AttachModel / DetachModel / UpdateTargets / CommitReady / GetStats
    //   Loaderスレッド: ProcessRequests(.ktexの部分読み出しとGPUリソースの作成)
    // ディスクリプタを書き換えるのはCommitReadyだけで、これはRenderスレッドがそのフレームで
    // 最初にSetTextureを呼ぶより前に実行する。理由はIRHIDevice::PrepareTextureContentsのコメント。
    //
    // 【専用スレッドを持たない】読み出しはモデルストリーミングと同じLoaderスレッドに相乗りする
    // (KurenaiEngine3D::LoaderThreadMainがProcessRequestsを呼ぶ)。スレッドを別に立てると、
    // 同じディスクを2本で奪い合ったうえ、GPUアップロード(内部でGPU同期待ちがある)が
    // モデル読み込みと並走してVRAMの山が二重になる。モデルの読み込み1件ごとに
    // 割り込ませることで、都市を流している最中でもミップの差し替えが止まらないようにしている。
    // **ProcessRequestsはm_Entriesを一切触らない**(必要な情報はRequestが写しで持つ)。
    // これによりRenderスレッド側の追跡表の付け外しとロック無しで共存できる。
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
            // この帯のうち、タイルリソース経路で扱えている枚数。
            // **64KBタイルはBC7で256x256テクセルを覆うため、標準ミップを持てない小さい
            // テクスチャは全部がミップテールになり、この経路に乗れない。**
            // 「タイルリソースが効くのはどの帯か」はこの数字で答える
            uint32_t TiledCount = 0;
        };

        // 1つのモデル(=1つの.kmodel)ぶんの常駐状況。常駐マップ(StreamingPanel)が
        // 街区ごとの色分けに使う
        struct ModelResidency
        {
            uint32_t TrackedTextures = 0;
            uint64_t ResidentBytes = 0;
            uint64_t FullBytes = 0;
            // 落とせているミップ0側の段数の平均。0なら一段も削れていない
            float MeanDroppedMips = 0.0f;
        };

        struct Stats
        {
            bool Enabled = false;
            uint32_t TrackedTextures = 0;
            // 追跡中のモデル数。ストリーミングで出入りするので毎フレーム変わる
            uint32_t TrackedModels = 0;
            uint32_t PendingRequests = 0;
            uint32_t InFlight = 0;
            uint64_t ResidentBytes = 0;
            uint64_t FullBytes = 0;
            // 差し替えの累計。**0なら一度も実行されていない**(対照実験で最初に潰す項目)
            uint64_t CommittedUpdates = 0;
            uint64_t FailedUpdates = 0;
            // **失敗ではない**破棄の累計。読み直している最中にモデルがストリーミングで
            // 外れると、出来上がった差し替えは行き先を失う。
            // 【失敗と同じ欄に混ぜない】混ぜると「街を流すと必ず失敗が出る」ように見え、
            // 本当の失敗(ファイルが読めない・GPUリソースを作れない)に気づけなくなる
            uint64_t DiscardedUpdates = 0;
            // タイルリソース経路で扱えているテクスチャ数と、タイルプールの実サイズ
            uint32_t TiledTextures = 0;
            uint32_t TiledResourcesTier = 0;
            uint64_t TilePoolReservedBytes = 0;
            uint64_t TilePoolUsedBytes = 0;
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

        // 追跡表が組まれているか(対象が1枚以上ある)
        bool IsBuilt() const { return m_AliveEntries > 0; }

        // 常駐ミップ制御を実際に効かせるか。falseの間はすべてのテクスチャを
        // 全ミップ常駐へ戻す方向へ目標を置く。
        //
        // 【A/B比較のためにある】「効かせない状態」を追跡表を壊さずに作れないと、
        // 同じ起動の中で off と on を比べられない。falseにしたあと常駐率が100%へ
        // 戻りきってから測ることで、対照(A)を同じ手順・同じ露出で取れる
        bool IsActive() const { return m_Active; }
        void SetActive(bool active) { m_Active = active; }

        // シーン読み込み直後に一度呼び、その時点で実体のあるモデルを追跡表へ入れる。
        // **[Scene]StreamingDistanceを使うシーンではここでは0件になる**
        // (モデルはまだ1つも読まれていない)。以降はAttachModel/DetachModelで出入りする
        void Build(const Scene& scene, RHI::IRHIDevice& device);

        // ストリーミングで読み込まれたモデルを追跡表へ入れる。
        // **同じモデルを指すインスタンスの数だけ呼ぶこと**(worldごとに参照が増える)。
        // テクスチャの登録はIRHITexture*で重複を除くので、2回目以降は参照だけが足される
        void AttachModel(const Model& model, const DirectX::XMFLOAT4X4& world, RHI::IRHIDevice& device);

        // ストリーミングで破棄されるモデルを追跡表から外す。**破棄より前に呼ぶこと**
        void DetachModel(const Model& model);

        // このモデルのテクスチャを読み直している最中か。
        // **trueの間はモデルを破棄してはいけない**(ProcessRequestsがIRHITexture*を掴んでいる)。
        // 破棄側(KurenaiEngine3D::UpdateModelStreaming)がこれで待つ
        bool IsModelBusy(const Model* model) const;

        // 常駐マップの色分け用。追跡していないモデルならfalse
        bool GetModelResidency(const Model* model, ModelResidency& outResidency) const;

        // 追跡表を捨てる。**シーンを破棄する前に必ず呼ぶこと**
        // (Loaderスレッドが読み出し中ならその1件が終わるまでここで待つ)
        void Reset();

        // --- Loaderスレッドから呼ぶ ---------------------------------------------------------
        // 新しい要求が積まれたときに呼ぶ通知。Loaderスレッドを起こすために使う
        void SetRequestNotifier(std::function<void()> notifier) { m_Notifier = std::move(notifier); }
        // 待っている要求があるか(Loaderスレッドの条件変数の述語に足す)
        bool HasPendingRequests() const;
        // 待っている要求を最大maxCount件だけ処理する。処理した件数を返す
        uint32_t ProcessRequests(RHI::IRHIDevice& device, size_t maxCount);

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
        // タイル経路の判定結果。0 = まだ判定していない / 1 = 乗っている / 2 = 乗らない
        static constexpr uint8_t kTileUnknown = 0;
        static constexpr uint8_t kTileInUse = 1;
        static constexpr uint8_t kTileUnavailable = 2;

        // 1つの.ktexにつき1件。Attach後、Owner/Path/Texture/Infoは変更しない。
        //
        // 【添字は使い回す】モデルがストリーミングで出入りするたびに詰め直すと、
        // 発注済みの要求が指す添字がずれる。Detachでは要素を消さずAlive=falseにし、
        // 空いた枠をm_FreeSlotsへ返して再利用する。世代(Generation)を付けてあるので、
        // 枠が別のテクスチャへ回った後に古い要求が戻ってきても取り違えない
        struct Entry
        {
            // このテクスチャを持つモデル。Detach/IsModelBusyの単位
            const Model* Owner = nullptr;
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
            // 枠が使われているか。falseならm_FreeSlotsに入っている(または入る途中)
            bool Alive = false;
            // Detachのたびに1つ進める。戻ってきた結果が今の枠のものかを見分ける
            uint32_t Generation = 0;
            uint8_t TileState = kTileUnknown;
            uint32_t ResidentFirstMip = 0;
            uint32_t RequestedFirstMip = 0;
            bool InFlight = false;
            // Detachされたが読み出し中で枠を返せていない。CommitReadyが引き取る
            bool PendingFree = false;
            // 粗くする(FirstMipを増やす)側だけに掛けるヒステリシス。
            // 詳細化は即座、粗化は一定時間その状態が続いてから1段ずつ
            float CoarserHoldSeconds = 0.0f;
        };

        // 【Entryへの参照を持たない】ProcessRequestsはLoaderスレッドで動くので、
        // Renderスレッドが付け外ししているm_Entriesを触らせない。必要なものは写しで渡す
        struct Request
        {
            size_t EntrySlot = 0;
            uint32_t EntryGeneration = 0;
            uint32_t FirstMip = 0;
            uint8_t TileState = kTileUnknown;
            // m_InFlightByModelの鍵にするだけ。**Loaderスレッドはこれを参照外ししない**
            const Model* Owner = nullptr;
            RHI::IRHITexture* Texture = nullptr;
            std::wstring Path;
            RHI::PackedTextureInfo Info{};
        };

        struct ReadyItem
        {
            size_t EntrySlot = 0;
            uint32_t EntryGeneration = 0;
            uint32_t FirstMip = 0;
            uint8_t TileState = kTileUnknown;
            const Model* Owner = nullptr;
            // nullptrなら失敗(読み込みかGPUリソース作成でこけた)
            std::unique_ptr<RHI::IRHIPendingTextureContents> Pending;
        };

        // Detachされた枠を実際にm_FreeSlotsへ返す(Renderスレッド専用)
        void FreeEntrySlot(size_t slot);
        static size_t SizeBandOf(const RHI::PackedTextureInfo& info);

        bool m_Enabled = false;
        bool m_Active = true;
        float m_MipBias = 0.0f;

        // 枠の配列。Detachでは縮めず、空いた枠をm_FreeSlotsで使い回す(Entryのコメント参照)
        std::vector<Entry> m_Entries;
        std::vector<size_t> m_FreeSlots;
        uint32_t m_AliveEntries = 0;
        // 同じテクスチャを2枠に登録しないための逆引き。1つの.ktexは1つのModelの中でしか
        // 共有されないが、Model単位では複数メッシュから参照される
        std::unordered_map<const RHI::IRHITexture*, size_t> m_TextureToEntry;
        // モデル→そのモデルが持つ枠。Detach / GetModelResidencyで使う
        std::unordered_map<const Model*, std::vector<size_t>> m_ModelEntries;
        // モデル→そのモデルで読み出し中の件数。IsModelBusyをO(1)で答えるために持つ。
        // **Detachした後も残る**(破棄を待たせる相手はまさにそれ)ので、m_ModelEntriesとは別に持つ
        std::unordered_map<const Model*, uint32_t> m_InFlightByModel;
        // IRHIDevice::GetTiledResourcesTier()の写し。0ならタイル経路を試さない
        uint32_t m_TiledResourcesTier = 0;
        // タイルプールの実サイズ。CommitReady(デバイスを触れる場所)で拾って控え、
        // GetStatsはこれを読むだけにする(GetStatsはデバイスを受け取らないため)
        uint64_t m_TilePoolReservedBytes = 0;
        uint64_t m_TilePoolUsedBytes = 0;
        // 毎フレーム全件を走査すると数万件では効かないため、フレームごとに一部だけ見る。
        // 目標ミップは距離に対して緩やかにしか変わらないので、数フレームの遅れは見えない
        size_t m_ScanCursor = 0;

        std::deque<Request> m_Requests;
        mutable std::mutex m_RequestMutex;
        // ProcessRequestsが要求を1件抱えている間だけ1になる。
        // Resetが「Loaderスレッドがテクスチャを掴んでいない」ことを待つために使う
        size_t m_Processing = 0;
        std::condition_variable m_IdleCV;
        // 要求が積まれたことをLoaderスレッドへ知らせる
        std::function<void()> m_Notifier;

        std::vector<ReadyItem> m_Ready;
        mutable std::mutex m_ReadyMutex;

        // 統計(Renderスレッドが更新、UIスレッド相当が読む)
        mutable std::mutex m_StatsMutex;
        uint64_t m_CommittedUpdates = 0;
        uint64_t m_FailedUpdates = 0;
        uint64_t m_DiscardedUpdates = 0;
        uint32_t m_InFlightCount = 0;
    };
}

#pragma warning(pop)
