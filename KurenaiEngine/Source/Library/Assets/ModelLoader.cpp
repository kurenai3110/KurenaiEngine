#include "ModelLoader.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include "Core/Logger.h"
#include "Core/StringUtil.h"
#include "ModelPackage.h"
#include "RHI/TextureImage.h"
#include "Vertex.h"

// KurenaiPacker.exe(オフラインのアセットビルドツール)が生成した.kmodel/.kgeom/.ktexを
// 読み込む。**assimpによるモデル解析・WICデコード・ミップ生成・GPU BC7圧縮をここで行っては
// いけない**(前処理はすべてKurenaiPackerの担当)。このファイルはassimp/zlibに依存せず、
// 「パース済み・圧縮済みのデータをファイルから読み、GPUバッファ/テクスチャへ流し込むだけ」
// である。詳細な設計判断はdocs/Architecture.htmlの「モデルパッケージ形式」の章を参照

namespace Kurenai::Assets
{
    namespace
    {
        using Core::Utf8ToWide;
        using Core::WideToUtf8;

        std::wstring GetDirectory(const std::wstring& filePath)
        {
            const size_t pos = filePath.find_last_of(L"/\\");
            return pos == std::wstring::npos ? L"" : filePath.substr(0, pos + 1);
        }

        // 2時点間の経過時間をミリ秒の整数文字列にする(ログ表示用)
        std::string FormatMs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
        {
            return std::to_string(static_cast<long long>(std::chrono::duration<double, std::milli>(end - start).count()));
        }

        // StringPool(offset,length)からUTF-8部分文字列を安全に取り出す。壊れた.kmodelが
        // 範囲外を指していてもプロセスを異常終了させないよう、必ず範囲チェックを行う
        std::string ReadPoolString(const std::string& pool, uint32_t offset, uint32_t length, const char* fieldNameForError)
        {
            if (static_cast<uint64_t>(offset) + length > pool.size())
            {
                throw std::runtime_error(std::string("パッケージのStringPool参照が範囲外です: ") + fieldNameForError);
            }
            return pool.substr(offset, length);
        }

        // テクスチャの読み込みとキャッシュ・共有インスタンス(白/フラット法線/マゼンタ)の管理。
        // .kmodelのTextureEntryは既にKurenaiPacker側でユニーク化(同じ画像+同じsRGBは1件に集約)
        // 済みのため、パス文字列ベースの重複排除キャッシュは持たず、
        // 添字(TextureEntryのインデックス)だけで管理する
        class TextureLoader
        {
        public:
            // sharedTextures が非nullなら、1x1のフォールバック(白/フラット法線/黒/マゼンタ)を
            // そちらから借りる。nullならモデル自身が持つ(従来の挙動)
            TextureLoader(RHI::IRHIDevice& device, Model& model, SharedTexturePool* sharedTextures)
                : m_Device(device)
                , m_Model(model)
                , m_SharedTextures(sharedTextures)
            {
            }

            // texturePathsの各要素(.ktexへのフルパス)を並列にデコードし、成功した分だけGPU
            // リソース化してoutTexturesへ格納する。失敗した添字はoutTextures[i]==nullptrのまま
            // 残すので、呼び出し側(LoadModel)がスロットの種類(BaseColor/Normal/MetallicRoughness)
            // ごとに適切なフォールバック(白/フラット法線/マゼンタ)を選んで埋めること
            void LoadAll(const std::vector<std::wstring>& texturePaths, std::vector<RHI::IRHITexture*>& outTextures)
            {
                outTextures.assign(texturePaths.size(), nullptr);
                if (texturePaths.empty())
                {
                    return;
                }

                // デコード(TextureImage::LoadFromPackedTexture、単なるファイル読み込み+DDSデコードで
                // GPUデバイスを必要としない)はワーカースレッドで並列化できるが、GPUリソース作成
                // (device.CreateTextureFromImage)はデバイスに紐づく処理のためこのスレッドで直列に行う
                constexpr unsigned int kMaxWorkers = 8;
                const unsigned int hardwareThreads = std::min(kMaxWorkers, std::max(1u, std::thread::hardware_concurrency()));
                const unsigned int workerCount = std::min(hardwareThreads, static_cast<unsigned int>(texturePaths.size()));

                struct CompletedItem
                {
                    size_t Index = 0;
                    std::optional<RHI::TextureImage> Image;
                    std::string ErrorMessage;
                    uint64_t SizeInBytes = 0;
                };

                std::mutex queueMutex;
                std::condition_variable spaceAvailable;
                std::condition_variable itemAvailable;
                std::deque<CompletedItem> completedQueue;
                uint64_t pendingBytes = 0;
                std::atomic<size_t> nextIndex{ 0 };

                // ワーカーがGPU化(このスレッド)に追いつかれすぎてデコード済みイメージを
                // メモリに溜め込みすぎないよう、件数とバイト数の両方で上限を設ける
                const size_t maxPendingCount = static_cast<size_t>(workerCount) * 2;
                constexpr uint64_t kMaxPendingBytes = 1ull * 1024 * 1024 * 1024;

                auto workerFn = [&]()
                {
                    for (;;)
                    {
                        const size_t index = nextIndex.fetch_add(1);
                        if (index >= texturePaths.size())
                        {
                            break;
                        }

                        CompletedItem item;
                        item.Index = index;
                        try
                        {
                            RHI::TextureImage image = RHI::TextureImage::LoadFromPackedTexture(texturePaths[index]);
                            item.SizeInBytes = image.GetSizeInBytes();
                            item.Image = std::move(image);
                        }
                        catch (const std::exception& e)
                        {
                            item.ErrorMessage = e.what();
                        }

                        std::unique_lock<std::mutex> lock(queueMutex);
                        spaceAvailable.wait(lock, [&] { return completedQueue.size() < maxPendingCount && pendingBytes < kMaxPendingBytes; });
                        pendingBytes += item.SizeInBytes;
                        completedQueue.push_back(std::move(item));
                        lock.unlock();
                        itemAvailable.notify_one();
                    }
                };

                std::vector<std::thread> workers;
                workers.reserve(workerCount);
                for (unsigned int w = 0; w < workerCount; ++w)
                {
                    workers.emplace_back(workerFn);
                }

                for (size_t consumed = 0; consumed < texturePaths.size(); ++consumed)
                {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    itemAvailable.wait(lock, [&] { return !completedQueue.empty(); });
                    CompletedItem item = std::move(completedQueue.front());
                    completedQueue.pop_front();
                    pendingBytes -= item.SizeInBytes;
                    lock.unlock();
                    spaceAvailable.notify_one();

                    if (item.Image.has_value())
                    {
                        try
                        {
                            auto texture = m_Device.CreateTextureFromImage(*item.Image);
                            outTextures[item.Index] = texture.get();
                            m_Model.Textures.push_back(std::move(texture));
                        }
                        catch (const std::exception& e)
                        {
                            Core::Logger::Error("ModelLoader", "テクスチャのGPU転送に失敗しました (" + WideToUtf8(texturePaths[item.Index]) + "): " + e.what());
                        }
                    }
                    else
                    {
                        Core::Logger::Error("ModelLoader", "テクスチャの読み込みに失敗しました (" + WideToUtf8(texturePaths[item.Index]) + "): " + item.ErrorMessage);
                    }
                }

                for (auto& worker : workers)
                {
                    worker.join();
                }
            }

            RHI::IRHITexture* GetWhite()
            {
                return Acquire(m_White, m_SharedTextures ? &m_SharedTextures->White : nullptr, 255, 255, 255, 255);
            }

            RHI::IRHITexture* GetFlatNormal()
            {
                // タンジェント空間で(0,0,1)、すなわち「法線マップなし」を表す色
                return Acquire(m_FlatNormal, m_SharedTextures ? &m_SharedTextures->FlatNormal : nullptr, 128, 128, 255, 255);
            }

            // bent normalを持たないマテリアルのフォールバック。
            //
            // 【白ではなく黒】bent normalは「遮蔽なし」を定数テクスチャで表現できない ――
            // 遮蔽なしのbRawは法線Nそのもので、ピクセルごとに違うため。
            // アルファ0を「データ無し」の明示的なフラグとして使い、消費側で
            // axis = N / aoB = 1(遮蔽なし)へ落とさせる。長さ0を遮蔽なしと解釈させると
            // 完全遮蔽(SO=0)と区別がつかなくなる(34章)
            RHI::IRHITexture* GetBlack()
            {
                return Acquire(m_Black, m_SharedTextures ? &m_SharedTextures->Black : nullptr, 0, 0, 0, 0);
            }

            // 読み込みに失敗したBaseColor/MetallicRoughnessテクスチャの代替。目立つ色にすることで
            // モデル全体の読み込みは継続しつつ問題箇所が分かるようにする
            RHI::IRHITexture* GetMagentaPlaceholder()
            {
                return Acquire(m_Magenta, m_SharedTextures ? &m_SharedTextures->Magenta : nullptr, 255, 0, 255, 255);
            }

        private:
            // 1x1の定数テクスチャを1つ返す。共有プールがあればそこへ、無ければモデルへ所有させる。
            //
            // localCache はどちらの経路でも使う。共有プール経由でも、2回目以降にプールの
            // メンバを読みに行くコストを省ける(1モデルあたり最大4スロット×メッシュ数だけ呼ばれる)
            RHI::IRHITexture* Acquire(
                RHI::IRHITexture*& localCache,
                RHI::IRHITexture** sharedSlot,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a)
            {
                if (localCache)
                {
                    return localCache;
                }

                if (sharedSlot)
                {
                    if (!*sharedSlot)
                    {
                        auto texture = m_Device.CreateSolidColorTexture(r, g, b, a);
                        *sharedSlot = texture.get();
                        m_SharedTextures->Owned.push_back(std::move(texture));
                    }
                    localCache = *sharedSlot;
                    return localCache;
                }

                auto texture = m_Device.CreateSolidColorTexture(r, g, b, a);
                localCache = texture.get();
                m_Model.Textures.push_back(std::move(texture));
                return localCache;
            }

            RHI::IRHIDevice& m_Device;
            Model& m_Model;
            // 非nullなら1x1のフォールバックをここから借りる(所有もこちら)
            SharedTexturePool* m_SharedTextures = nullptr;
            RHI::IRHITexture* m_White = nullptr;
            RHI::IRHITexture* m_FlatNormal = nullptr;
            RHI::IRHITexture* m_Black = nullptr;
            RHI::IRHITexture* m_Magenta = nullptr;
        };

        // メッシュをマテリアル(3枚のテクスチャの組み合わせ)単位でまとめておく。
        // .kmodelはKurenaiPackerがシーングラフ巡回順のまま書き出しているため、DX12バックエンドの
        // 「直前の描画と同じテクスチャならSRVテーブルを使い回す」最適化(DX12CommandList::
        // FlushPendingSrvWrites)がヒットしやすいよう、読み込み後にソートしておく
        void SortMeshesByMaterial(Model& model)
        {
            std::sort(
                model.Meshes.begin(), model.Meshes.end(),
                [](const Mesh& a, const Mesh& b)
                {
                    const std::less<RHI::IRHITexture*> less;
                    if (a.BaseColorTexture != b.BaseColorTexture)
                    {
                        return less(a.BaseColorTexture, b.BaseColorTexture);
                    }
                    if (a.NormalTexture != b.NormalTexture)
                    {
                        return less(a.NormalTexture, b.NormalTexture);
                    }
                    if (a.MetallicRoughnessTexture != b.MetallicRoughnessTexture)
                    {
                        return less(a.MetallicRoughnessTexture, b.MetallicRoughnessTexture);
                    }
                    if (a.OcclusionTexture != b.OcclusionTexture)
                    {
                        return less(a.OcclusionTexture, b.OcclusionTexture);
                    }
                    return less(a.BentNormalTexture, b.BentNormalTexture);
                });
        }
    }

    Model LoadModel(RHI::IRHIDevice& device, const std::wstring& filePath, SharedTexturePool* sharedTextures)
    {
        const auto startTime = std::chrono::steady_clock::now();
        const std::wstring directory = GetDirectory(filePath);

        // 既定のstreambufバッファ(通常数百バイト~数KB)のままだと、Bistro級の.kgeom
        // (100MB超)を細切れのreadで読むことになりオーバーヘッドが無視できないため、
        // openより前に大きめ(1MB)のバッファを設定しておく。ioBufferはinより先に構築し
        // (=inより後に破棄され)、in使用中は常に有効な状態を保つ
        std::vector<char> manifestIoBuffer(1 << 20);
        std::ifstream in;
        in.rdbuf()->pubsetbuf(manifestIoBuffer.data(), static_cast<std::streamsize>(manifestIoBuffer.size()));
        in.open(filePath, std::ios::binary);
        if (!in.is_open())
        {
            throw std::runtime_error("モデルパッケージを開けませんでした: " + WideToUtf8(filePath));
        }

        PackageHeader header{};
        std::vector<TextureEntry> textureEntries;
        std::vector<MaterialEntry> materialEntries;
        std::vector<MeshEntry> meshEntries;
        std::vector<LightEntry> lightEntries;
        std::string stringPool;

        try
        {
            in.exceptions(std::ios::failbit | std::ios::badbit);

            in.read(reinterpret_cast<char*>(&header), sizeof(header));
            if (std::memcmp(header.Magic, kPackageMagic, sizeof(kPackageMagic)) != 0)
            {
                throw std::runtime_error("マジックナンバーが不正です");
            }
            if (header.Version != kPackageVersion)
            {
                throw std::runtime_error(
                    "バージョンが対応していません(ファイル: " + std::to_string(header.Version) +
                    ", ランタイム: " + std::to_string(kPackageVersion) + ")");
            }
            if (header.VertexStride != sizeof(Vertex) || header.IndexStride != sizeof(uint32_t))
            {
                throw std::runtime_error("頂点/インデックスのレイアウトが現在のランタイムと一致しません");
            }

            textureEntries.resize(header.TextureCount);
            if (header.TextureCount > 0)
            {
                in.read(reinterpret_cast<char*>(textureEntries.data()), static_cast<std::streamsize>(textureEntries.size() * sizeof(TextureEntry)));
            }

            // マテリアルはテクスチャ番号を参照するのでテクスチャの後ろ、メッシュの前
            // (v10で追加。ModelPackage.hのファイルレイアウト参照)
            materialEntries.resize(header.MaterialCount);
            if (header.MaterialCount > 0)
            {
                in.read(reinterpret_cast<char*>(materialEntries.data()), static_cast<std::streamsize>(materialEntries.size() * sizeof(MaterialEntry)));
            }

            meshEntries.resize(header.MeshCount);
            if (header.MeshCount > 0)
            {
                in.read(reinterpret_cast<char*>(meshEntries.data()), static_cast<std::streamsize>(meshEntries.size() * sizeof(MeshEntry)));
            }

            lightEntries.resize(header.LightCount);
            if (header.LightCount > 0)
            {
                in.read(reinterpret_cast<char*>(lightEntries.data()), static_cast<std::streamsize>(lightEntries.size() * sizeof(LightEntry)));
            }

            stringPool.resize(header.StringPoolSize);
            if (header.StringPoolSize > 0)
            {
                in.read(stringPool.data(), static_cast<std::streamsize>(stringPool.size()));
            }
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error("モデルパッケージの読み込みに失敗しました(" + WideToUtf8(filePath) + "): " + e.what());
        }

        if (meshEntries.empty())
        {
            throw std::runtime_error("モデルパッケージにメッシュが含まれていません: " + WideToUtf8(filePath));
        }

        // StringPoolからジオメトリ/テクスチャのパスを解決する(.kmodel自身のディレクトリからの相対パス)
        const std::wstring geometryPath = directory + Utf8ToWide(
            ReadPoolString(stringPool, header.GeometryPathOffset, header.GeometryPathLength, "GeometryPath"));

        std::vector<std::wstring> texturePaths(textureEntries.size());
        for (size_t i = 0; i < textureEntries.size(); ++i)
        {
            texturePaths[i] = directory + Utf8ToWide(
                ReadPoolString(stringPool, textureEntries[i].PathOffset, textureEntries[i].PathLength, "TexturePath"));
        }

        const auto manifestReadTime = std::chrono::steady_clock::now();

        // .kgeomを読み込む。Bistro級では100MBを超えるため、こちらにも大きめのI/Oバッファを設定する
        std::vector<char> geometryIoBuffer(1 << 20);
        std::ifstream geomIn;
        geomIn.rdbuf()->pubsetbuf(geometryIoBuffer.data(), static_cast<std::streamsize>(geometryIoBuffer.size()));
        geomIn.open(geometryPath, std::ios::binary);
        if (!geomIn.is_open())
        {
            throw std::runtime_error("ジオメトリファイルを開けませんでした: " + WideToUtf8(geometryPath));
        }

        std::vector<uint8_t> geometryPayload;
        try
        {
            geomIn.exceptions(std::ios::failbit | std::ios::badbit);

            GeometryHeader geomHeader{};
            geomIn.read(reinterpret_cast<char*>(&geomHeader), sizeof(geomHeader));
            if (std::memcmp(geomHeader.Magic, kGeometryMagic, sizeof(kGeometryMagic)) != 0)
            {
                throw std::runtime_error("マジックナンバーが不正です");
            }
            if (geomHeader.Version != kGeometryVersion)
            {
                throw std::runtime_error("バージョンが対応していません");
            }
            if (geomHeader.VertexStride != sizeof(Vertex) || geomHeader.IndexStride != sizeof(uint32_t))
            {
                throw std::runtime_error("頂点/インデックスのレイアウトが現在のランタイムと一致しません");
            }

            geometryPayload.resize(geomHeader.PayloadSize);
            if (geomHeader.PayloadSize > 0)
            {
                geomIn.read(reinterpret_cast<char*>(geometryPayload.data()), static_cast<std::streamsize>(geometryPayload.size()));
            }
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error("ジオメトリファイルの読み込みに失敗しました(" + WideToUtf8(geometryPath) + "): " + e.what());
        }

        // 各MeshEntryのオフセット/カウントがペイロード範囲内かを必ず検証する。不正な.kmodelを
        // 読んだ場合にバッファオーバーラン(境界外の頂点/インデックスデータを読む)を防ぐため
        for (size_t i = 0; i < meshEntries.size(); ++i)
        {
            const MeshEntry& mesh = meshEntries[i];
            const uint64_t vertexEnd = mesh.VertexOffset + static_cast<uint64_t>(mesh.VertexCount) * sizeof(Vertex);
            const uint64_t indexEnd = mesh.IndexOffset + static_cast<uint64_t>(mesh.IndexCount) * sizeof(uint32_t);
            // メッシュレットの3ブロックも同様に検証する。カウントが0の場合はオフセットが
            // ペイロード末尾を指しうるが、末尾ちょうどは範囲内として扱ってよい(0バイト読む)
            const uint64_t meshletEnd =
                mesh.MeshletOffset + static_cast<uint64_t>(mesh.MeshletCount) * sizeof(MeshletEntry);
            const uint64_t meshletVertexEnd =
                mesh.MeshletVertexOffset + static_cast<uint64_t>(mesh.MeshletVertexCount) * sizeof(uint32_t);
            const uint64_t meshletTriangleEnd =
                mesh.MeshletTriangleOffset + static_cast<uint64_t>(mesh.MeshletTriangleCount) * sizeof(uint32_t);
            if (vertexEnd > geometryPayload.size() || indexEnd > geometryPayload.size() ||
                meshletEnd > geometryPayload.size() || meshletVertexEnd > geometryPayload.size() ||
                meshletTriangleEnd > geometryPayload.size())
            {
                throw std::runtime_error(
                    "メッシュ[" + std::to_string(i) + "]がジオメトリペイロードの範囲外を参照しています: " + WideToUtf8(geometryPath));
            }
            // メッシュレットの段は、範囲がメッシュレット配列に収まっていなければならない。
            // 段の範囲が壊れていると、描画時に他のメッシュのメッシュレットを掴む
            if (mesh.MeshletLODCount > kMaxMeshletLODCount)
            {
                throw std::runtime_error(
                    "メッシュ[" + std::to_string(i) + "]のメッシュレットLODの段数が上限を超えています: " + WideToUtf8(filePath));
            }
            for (uint32_t lod = 0; lod < mesh.MeshletLODCount; ++lod)
            {
                const uint64_t lodEnd =
                    static_cast<uint64_t>(mesh.MeshletLODOffsets[lod]) + mesh.MeshletLODCounts[lod];
                if (lodEnd > mesh.MeshletCount)
                {
                    throw std::runtime_error(
                        "メッシュ[" + std::to_string(i) + "]のメッシュレットLOD[" + std::to_string(lod) +
                        "]がメッシュレット配列の範囲外です: " + WideToUtf8(filePath));
                }
            }

            if (mesh.MaterialIndex < 0 || mesh.MaterialIndex >= static_cast<int32_t>(materialEntries.size()))
            {
                throw std::runtime_error("メッシュ[" + std::to_string(i) + "]が範囲外のマテリアルを参照しています: " + WideToUtf8(filePath));
            }
        }

        // テクスチャ番号の検証はマテリアル側で行う(v10で材質がMeshEntryから移ったため)
        for (size_t i = 0; i < materialEntries.size(); ++i)
        {
            const MaterialEntry& material = materialEntries[i];
            if (material.BaseColorTextureIndex >= static_cast<int32_t>(textureEntries.size()) ||
                material.NormalTextureIndex >= static_cast<int32_t>(textureEntries.size()) ||
                material.MetallicRoughnessTextureIndex >= static_cast<int32_t>(textureEntries.size()) ||
                material.EmissiveTextureIndex >= static_cast<int32_t>(textureEntries.size()) ||
                material.OcclusionTextureIndex >= static_cast<int32_t>(textureEntries.size()) ||
                material.BentNormalTextureIndex >= static_cast<int32_t>(textureEntries.size()))
            {
                throw std::runtime_error("マテリアル[" + std::to_string(i) + "]が範囲外のテクスチャを参照しています: " + WideToUtf8(filePath));
            }
        }

        const auto geometryReadTime = std::chrono::steady_clock::now();

        Model model;
        model.BoundsMin[0] = header.BoundsMin[0];
        model.BoundsMin[1] = header.BoundsMin[1];
        model.BoundsMin[2] = header.BoundsMin[2];
        model.BoundsMax[0] = header.BoundsMax[0];
        model.BoundsMax[1] = header.BoundsMax[1];
        model.BoundsMax[2] = header.BoundsMax[2];

        TextureLoader textureLoader(device, model, sharedTextures);
        std::vector<RHI::IRHITexture*> resolvedTextures;
        textureLoader.LoadAll(texturePaths, resolvedTextures);

        const auto textureLoadTime = std::chrono::steady_clock::now();

        // -1(指定なし)は白/フラット法線、指定されていたのに読み込みに失敗した場合は
        // マゼンタ/フラット法線を使う(詳細はGetMagentaPlaceholder/GetFlatNormalのコメント参照)
        auto resolveBaseColorOrMetallicRoughness = [&](int32_t index) -> RHI::IRHITexture*
        {
            if (index == kNoTextureIndex)
            {
                return textureLoader.GetWhite();
            }
            RHI::IRHITexture* texture = resolvedTextures[static_cast<size_t>(index)];
            return texture ? texture : textureLoader.GetMagentaPlaceholder();
        };
        auto resolveNormal = [&](int32_t index) -> RHI::IRHITexture*
        {
            if (index == kNoTextureIndex)
            {
                return textureLoader.GetFlatNormal();
            }
            RHI::IRHITexture* texture = resolvedTextures[static_cast<size_t>(index)];
            return texture ? texture : textureLoader.GetFlatNormal();
        };
        // 読み込みに失敗した場合も黒(=有効フラグ0)へ落とす。マゼンタのような目立つ色にすると
        // bRawとして解釈された結果が不定になるため、ここは「データ無し」で縮退させるのが正しい
        auto resolveBentNormal = [&](int32_t index) -> RHI::IRHITexture*
        {
            if (index == kNoTextureIndex)
            {
                return textureLoader.GetBlack();
            }
            RHI::IRHITexture* texture = resolvedTextures[static_cast<size_t>(index)];
            return texture ? texture : textureLoader.GetBlack();
        };

        // レイトレーシング用の頂点属性・インデックスを作るか。デバイスが非対応なら作らない
        // (Bistro級では100MB規模になるため、使わない環境で確保しない)
        const bool buildRaytracingGeometry = device.SupportsRaytracing();
        if (buildRaytracingGeometry)
        {
            size_t totalVertexCount = 0;
            size_t totalIndexCount = 0;
            for (const MeshEntry& mesh : meshEntries)
            {
                totalVertexCount += mesh.VertexCount;
                totalIndexCount += mesh.IndexCount;
            }
            model.RaytracingAttributes.reserve(totalVertexCount);
            model.RaytracingIndices.reserve(totalIndexCount);
        }

        // メッシュレットのGPUバッファを作るか。デバイスがメッシュシェーダーに対応していない、
        // あるいは.kmodelが--no-meshletsで焼かれている場合は作らない
        // (読まれないバッファでVRAMを占有しないため。レイトレーシング用配列と同じ考え方)
        const bool buildMeshletGeometry = device.SupportsMeshShader();

        // 頂点/インデックスバッファへSRVを重ねて張り、bindlessで引けるようにするか。
        // 使うのはメッシュシェーダー経路(頂点のみ)とコンピュートシェーダーによる
        // 自前ラスタライザ経路(頂点+インデックス)。
        //
        // 【メッシュシェーダー対応と連動させない】SM 6.6には対応しているがメッシュシェーダーを
        // 持たないGPU(NVIDIA Pascal世代など)では、buildMeshletGeometryがfalseのまま
        // 自前ラスタライザだけが使える。連動させるとその環境でジオメトリを引けなくなる。
        //
        // 追加コストはメッシュあたりSRV 2本ぶんのディスクリプタだけで、
        // バッファ本体は頂点バッファビュー/インデックスバッファビューと同一リソースを共有する
        const bool shaderReadableGeometry = buildMeshletGeometry || device.SupportsSoftwareRaster();

        model.Meshes.reserve(meshEntries.size());
        for (const MeshEntry& mesh : meshEntries)
        {
            Mesh outMesh;

            RHI::BufferDesc vertexBufferDesc;
            vertexBufferDesc.Usage = RHI::BufferUsage::Vertex;
            vertexBufferDesc.SizeInBytes = static_cast<uint32_t>(mesh.VertexCount) * sizeof(Vertex);
            vertexBufferDesc.StrideInBytes = sizeof(Vertex);
            vertexBufferDesc.InitialData = geometryPayload.data() + mesh.VertexOffset;
            // メッシュシェーダーには入力アセンブラが無く、頂点は自分でバッファから読む。
            // 同じリソースへ頂点バッファビューとStructuredBuffer<Vertex>のSRVを重ねて張り、
            // 従来経路とメッシュシェーダー経路で1本の頂点バッファを共有する
            // (別に複製するとVRAMを二重に食う)
            vertexBufferDesc.ShaderReadable = shaderReadableGeometry;
            outMesh.VertexBuffer = device.CreateBuffer(vertexBufferDesc);

            RHI::BufferDesc indexBufferDesc;
            indexBufferDesc.Usage = RHI::BufferUsage::Index;
            indexBufferDesc.SizeInBytes = static_cast<uint32_t>(mesh.IndexCount) * sizeof(uint32_t);
            indexBufferDesc.StrideInBytes = sizeof(uint32_t);
            indexBufferDesc.InitialData = geometryPayload.data() + mesh.IndexOffset;
            // 頂点と同じ理由でインデックスバッファにもSRVを重ねる。自前ラスタライザは
            // 三角形番号からインデックスを3つ引くため、StructuredBuffer<uint>として読む
            // (メッシュシェーダー経路はメッシュレット側の間接テーブルを使うのでこれは読まない)
            indexBufferDesc.ShaderReadable = shaderReadableGeometry;
            outMesh.IndexBuffer = device.CreateBuffer(indexBufferDesc);
            outMesh.IndexCount = mesh.IndexCount;
            outMesh.VertexCount = mesh.VertexCount;

            // アセットが持つメッシュレット数。GPUバッファを作るかどうか(下)とは独立で、
            // メッシュシェーダー非対応の環境でもレイトレーシング側が使うため常に控える。
            //
            // 【全段の合計ではなくLOD0の個数を入れる】v10からメッシュレット配列は
            // 離散LODの全段を連結して持つ。描画もレイトレーシングもLOD0だけを見るので、
            // MeshEntry.MeshletCount(全段の合計)をそのまま渡すと、簡略化した段まで
            // 重ねて描かれる/三角形番号の対応が崩れる。段を選ぶのはメッシュレットLODの実装で行う
            outMesh.MeshletCount = mesh.MeshletLODCount > 0 ? mesh.MeshletLODCounts[0] : 0u;

            // 頂点/インデックスのbindless番号。メッシュシェーダー経路は頂点を、
            // 自前ラスタライザ経路は両方を、ResourceDescriptorHeap経由で読む。
            // 番号は描画時に定数バッファへ載せて渡すため、ここで一度だけ登録して
            // IRHIBuffer側に覚えさせる(GetBindlessIndexで取り出せる)。
            //
            // 【メッシュレットの有無と連動させない】メッシュレットを持たない.kmodelでも
            // 自前ラスタライザはジオメトリを引く必要がある
            if (shaderReadableGeometry)
            {
                device.RegisterBindless(outMesh.VertexBuffer.get());
                device.RegisterBindless(outMesh.IndexBuffer.get());
            }

            if (buildMeshletGeometry && mesh.MeshletCount > 0)
            {
                // 3本ともシーン読み込み時に一度書いたら変わらないためStructuredImmutable。
                // 内容は.kgeomのバイト列そのままで、読み込み後の加工は一切要らない
                const auto createImmutable = [&](uint64_t offset, uint32_t count, uint32_t stride) {
                    RHI::BufferDesc desc;
                    desc.Usage = RHI::BufferUsage::StructuredImmutable;
                    desc.SizeInBytes = count * stride;
                    desc.StrideInBytes = stride;
                    desc.InitialData = geometryPayload.data() + offset;
                    return device.CreateBuffer(desc);
                };

                outMesh.MeshletBuffer =
                    createImmutable(mesh.MeshletOffset, mesh.MeshletCount, sizeof(MeshletEntry));
                outMesh.MeshletVertexBuffer =
                    createImmutable(mesh.MeshletVertexOffset, mesh.MeshletVertexCount, sizeof(uint32_t));
                outMesh.MeshletTriangleBuffer =
                    createImmutable(mesh.MeshletTriangleOffset, mesh.MeshletTriangleCount, sizeof(uint32_t));

                // メッシュシェーダーはこの3本もResourceDescriptorHeap経由で読む
                // (頂点バッファは上で登録済み)
                device.RegisterBindless(outMesh.MeshletBuffer.get());
                device.RegisterBindless(outMesh.MeshletVertexBuffer.get());
                device.RegisterBindless(outMesh.MeshletTriangleBuffer.get());
            }

            if (buildRaytracingGeometry)
            {
                // geometryPayloadがまだ生存しているこの場でしか元データを読めないため、
                // ここでレイトレーシング用の圧縮属性を作っておく(位置は持たない。理由は
                // RaytracingGeometry.hのコメント参照)
                outMesh.RaytracingAttributeOffset = static_cast<uint32_t>(model.RaytracingAttributes.size());
                outMesh.RaytracingIndexOffset = static_cast<uint32_t>(model.RaytracingIndices.size());

                const auto* vertices = reinterpret_cast<const Vertex*>(geometryPayload.data() + mesh.VertexOffset);
                for (uint32_t v = 0; v < mesh.VertexCount; ++v)
                {
                    model.RaytracingAttributes.push_back(PackRaytracingVertexAttribute(vertices[v].Normal, vertices[v].UV));
                }

                const auto* indices = reinterpret_cast<const uint32_t*>(geometryPayload.data() + mesh.IndexOffset);
                model.RaytracingIndices.insert(model.RaytracingIndices.end(), indices, indices + mesh.IndexCount);

                // ヒットした三角形番号から所属メッシュレットを引くための表。
                // MeshletEntryのうちTriangleOffsetだけを抜き出して詰める
                // (理由はRaytracingScene::GetMeshletTriangleOffsetBufferのコメント参照)
                //
                // 【LOD0だけ詰める】三角形番号はインデックスバッファ上の番号で、
                // インデックスバッファにはLOD0の三角形しか入っていない(.kgeom v4)。
                // 簡略化した段のメッシュレットを混ぜると、TriangleOffsetが昇順でなくなり
                // 二分探索が破綻する
                outMesh.RaytracingMeshletOffset = static_cast<uint32_t>(model.RaytracingMeshletTriangleOffsets.size());
                const auto* meshlets = reinterpret_cast<const MeshletEntry*>(geometryPayload.data() + mesh.MeshletOffset);
                for (uint32_t m = 0; m < outMesh.MeshletCount; ++m)
                {
                    model.RaytracingMeshletTriangleOffsets.push_back(meshlets[m].TriangleOffset);
                }
            }

            // 材質はv10からMaterialEntry側にある。番号の範囲は上の検証で確認済み。
            //
            // 【Assets::Meshの持ち方は変えない】ランタイムの構造体はメッシュごとに材質を
            // 持ったままで、ここで転記する。描画側(レンダラ・シェーダー)に一切影響を出さず、
            // フォーマットの変更をこの1関数へ閉じ込めるため
            const MaterialEntry& material = materialEntries[static_cast<size_t>(mesh.MaterialIndex)];

            outMesh.BaseColorTexture = resolveBaseColorOrMetallicRoughness(material.BaseColorTextureIndex);
            outMesh.NormalTexture = resolveNormal(material.NormalTextureIndex);
            outMesh.MetallicRoughnessTexture = resolveBaseColorOrMetallicRoughness(material.MetallicRoughnessTextureIndex);
            outMesh.EmissiveTexture = resolveBaseColorOrMetallicRoughness(material.EmissiveTextureIndex);
            // 遮蔽マップも未指定なら白1x1(=遮蔽なし)へフォールバックさせればよいので、
            // BaseColor/MetallicRoughnessと同じ解決を再利用する
            outMesh.OcclusionTexture = resolveBaseColorOrMetallicRoughness(material.OcclusionTextureIndex);
            outMesh.OcclusionStrength = material.OcclusionStrength;
            // bent normalだけは白ではなく黒(=有効フラグ0)へ落とす。理由はGetBlackのコメント参照
            outMesh.BentNormalTexture = resolveBentNormal(material.BentNormalTextureIndex);
            outMesh.MetallicFactor = material.MetallicFactor;
            outMesh.RoughnessFactor = material.RoughnessFactor;
            outMesh.AlphaCutoff = material.AlphaCutoff;
            outMesh.Translucency = material.Translucency;
            outMesh.IsTransparent = (material.Flags & kMeshEntryFlagTransparent) != 0;
            outMesh.BaseColorFactor[0] = material.BaseColorFactor[0];
            outMesh.BaseColorFactor[1] = material.BaseColorFactor[1];
            outMesh.BaseColorFactor[2] = material.BaseColorFactor[2];
            outMesh.BaseColorFactor[3] = material.BaseColorFactor[3];
            outMesh.EmissiveFactor[0] = material.EmissiveFactor[0];
            outMesh.EmissiveFactor[1] = material.EmissiveFactor[1];
            outMesh.EmissiveFactor[2] = material.EmissiveFactor[2];

            model.Meshes.push_back(std::move(outMesh));
        }

        model.Lights.reserve(lightEntries.size());
        for (const LightEntry& entry : lightEntries)
        {
            Light light;
            light.Type = static_cast<LightType>(entry.Type);
            light.Position[0] = entry.Position[0];
            light.Position[1] = entry.Position[1];
            light.Position[2] = entry.Position[2];
            light.Direction[0] = entry.Direction[0];
            light.Direction[1] = entry.Direction[1];
            light.Direction[2] = entry.Direction[2];
            light.Color[0] = entry.Color[0];
            light.Color[1] = entry.Color[1];
            light.Color[2] = entry.Color[2];
            light.Intensity = entry.Intensity;
            light.Range = entry.Range;
            light.SpotInnerConeAngle = entry.SpotInnerConeAngle;
            light.SpotOuterConeAngle = entry.SpotOuterConeAngle;
            light.Enabled = entry.Enabled != 0;
            light.Name = ReadPoolString(stringPool, entry.NameOffset, entry.NameLength, "LightName");

            model.Lights.push_back(std::move(light));
        }

        SortMeshesByMaterial(model);

        const auto endTime = std::chrono::steady_clock::now();
        Core::Logger::Info(
            "ModelLoader",
            "モデル読み込み完了: " + WideToUtf8(filePath) +
            " (マニフェスト " + FormatMs(startTime, manifestReadTime) + "ms" +
            " / ジオメトリ " + FormatMs(manifestReadTime, geometryReadTime) + "ms" +
            " / テクスチャ " + FormatMs(geometryReadTime, textureLoadTime) + "ms" +
            " / 合計 " + FormatMs(startTime, endTime) + "ms" +
            ", テクスチャ要求 " + std::to_string(texturePaths.size()) + "件)");

        return model;
    }
}
