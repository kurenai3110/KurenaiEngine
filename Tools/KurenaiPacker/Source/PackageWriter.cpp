#include "PackageWriter.h"

#include <Windows.h>
#include <objbase.h>

#include <DirectXTex.h>

#include "RHI/TextureImage.h"
#include "Assets/ModelPackage.h"
#include "Core/StringUtil.h"
#include "MeshletBuilder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

using Kurenai::Assets::GeometryHeader;
using Kurenai::Assets::kGeometryBlockAlignment;
using Kurenai::Assets::kGeometryMagic;
using Kurenai::Assets::kGeometryVersion;
using Kurenai::Assets::kMeshEntryFlagTransparent;
using Kurenai::Assets::kNoTextureIndex;
using Kurenai::Assets::kPackageMagic;
using Kurenai::Assets::kPackageVersion;
using Kurenai::Assets::kPackedTextureFlagSRGB;
using Kurenai::Assets::kPackedTextureMagic;
using Kurenai::Assets::kPackedTextureVersion;
using Kurenai::Assets::kTextureEntryFlagSRGB;
using Kurenai::Assets::LightEntry;
using Kurenai::Assets::MaterialEntry;
using Kurenai::Assets::MeshEntry;
using Kurenai::Assets::PackageHeader;
using Kurenai::Assets::PackedTextureHeader;
using Kurenai::Assets::TextureEntry;
using Kurenai::Assets::Vertex;
using Kurenai::Core::WideToUtf8;
namespace fs = std::filesystem;

namespace KurenaiPacker
{
    namespace
    {
        // === 計測 ==============================================================
        //
        // OcclusionBaker.cppのBakeTimingsと同じ考え方。書き出しは数十ms〜数分と幅があるが、
        // now()を呼ぶのはフェーズ境界とメッシュ/テクスチャ1件ごとだけなので影響は無視できる
        using PhaseClock = std::chrono::steady_clock;

        double PhaseSecondsSince(const PhaseClock::time_point& start)
        {
            return std::chrono::duration<double>(PhaseClock::now() - start).count();
        }

        // スコープを抜けるときに累計へ足す。早期continue/例外があっても取りこぼさない
        struct ScopedPhase
        {
            double& Target;
            PhaseClock::time_point Start;

            explicit ScopedPhase(double& target) : Target(target), Start(PhaseClock::now()) {}
            ~ScopedPhase() { Target += PhaseSecondsSince(Start); }

            ScopedPhase(const ScopedPhase&) = delete;
            ScopedPhase& operator=(const ScopedPhase&) = delete;
        };

        // ワーカースレッドから積むので、こちらはアトミック。doubleのfetch_addはC++17に無いため
        // ナノ秒の整数で持つ(実時間の桁でオーバーフローしない)
        void AddNanos(std::atomic<uint64_t>& target, const PhaseClock::time_point& start)
        {
            target.fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    PhaseClock::now() - start).count()),
                std::memory_order_relaxed);
        }

        // 一時パスへ書いてから完了時のみ本来のパスへリネームする(ランタイム側の
        // .kmodelcache/.ktexcacheと同じ設計。処理が中断されても中途半端な一時
        // ファイルが残るだけで、既存の正常なファイルには影響しない)
        void WriteFileAtomic(const fs::path& path, const void* data, size_t size)
        {
            // 【一時ファイル名はプロセスと呼び出しで一意にする】以前は出力パス + ".tmp" 固定で、
            // 同じ.ktexパスへ向かうパッカーを2つ同時に走らせると**両方が同じ.tmpをtruncで開いて
            // 交互に書き**、先に終わった側がMoveFileExでリネームした結果、もう片方は
            // 「消えた.tmp」をリネームしようとして失敗する。その失敗は
            // 「テクスチャの処理に失敗しました(フォールバックします)」で握り潰されるため、
            // **終了コードは0のまま.kmodelのテクスチャだけが減る**(実測: Sponzaを2プロセス
            // 同時にパックして69枚→61枚。警告は出るがexit codeは0)。
            // 一意にすれば、両者が別々の.tmpへ書いて順にリネームするだけになり、
            // 最後に書いた側の内容が残る(同じ入力なら中身は同一)
            static std::atomic<uint64_t> tmpCounter{ 0 };
            const fs::path tmp = fs::path(path).concat(
                L"." + std::to_wstring(GetCurrentProcessId()) +
                L"." + std::to_wstring(tmpCounter.fetch_add(1)) + L".tmp");
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out.is_open())
                {
                    throw std::runtime_error("ファイルを書き込めませんでした: " + WideToUtf8(path.wstring()));
                }
                if (size > 0)
                {
                    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
                }
                if (!out)
                {
                    throw std::runtime_error("ファイルの書き込みに失敗しました: " + WideToUtf8(path.wstring()));
                }
            }
            // 【同じ出力パスへ2プロセスが同時にリネームすると一時的に失敗する】
            // MoveFileExはリネーム先を一瞬掴むため、別プロセスが同じ瞬間に同じ先へ
            // リネームしていると ERROR_SHARING_VIOLATION / ERROR_ACCESS_DENIED が返る。
            // これは恒久的な失敗ではないので短い間隔で数回やり直す。
            // 諦めた場合は一時ファイルを残さない(名前が実行ごとに変わるため掃除しづらい)
            constexpr int kRenameAttempts = 10;
            DWORD lastError = ERROR_SUCCESS;
            for (int attempt = 0; attempt < kRenameAttempts; ++attempt)
            {
                if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
                {
                    return;
                }
                lastError = GetLastError();
                if (lastError != ERROR_SHARING_VIOLATION && lastError != ERROR_ACCESS_DENIED)
                {
                    break;
                }
                Sleep(5 * (attempt + 1));
            }

            std::error_code removeEc;
            fs::remove(tmp, removeEc);
            throw std::runtime_error(
                "ファイルの確定(rename)に失敗しました(Win32エラー " + std::to_string(lastError) + "): "
                + WideToUtf8(path.wstring()));
        }

        // pathのバイト表現を'/'区切りへ正規化して返す(.kmodel内のStringPoolに書く形式)
        std::string ToPackagePathString(const fs::path& relativePath)
        {
            std::wstring generic = relativePath.generic_wstring();
            return WideToUtf8(generic);
        }

        struct TextureRequest
        {
            std::wstring SourcePath;
            bool SRGB = false;
            fs::path OutputKtexPath;
            bool Failed = false;
        };

        // メッシュ1つが参照するテクスチャ要求(requests配列)へのインデックス。
        // 空パスのスロットはkNoRequestを持つ
        constexpr size_t kNoRequest = static_cast<size_t>(-1);
        struct MeshTextureRefs
        {
            size_t BaseColor = kNoRequest;
            size_t Normal = kNoRequest;
            size_t MetallicRoughness = kNoRequest;
            size_t Emissive = kNoRequest;
            size_t Occlusion = kNoRequest;
        };

        // texturePathをsourceModelDirectoryからの相対パスとしてoutputDirectory配下へ
        // ミラーした.ktex出力パスを計算する。sourceModelDirectory外を指す場合
        // (テクスチャが入力モデルと別ツリーにある場合)は_External/へ平坦化し、
        // 同名衝突時は連番を付ける
        fs::path ComputeKtexOutputPath(
            const std::wstring& texturePath,
            const fs::path& sourceModelDirectory,
            const fs::path& outputDirectory,
            bool sRGB,
            const std::wstring& embeddedTextureDirectory,
            const std::wstring& outputModelStem,
            std::unordered_map<std::wstring, int>& externalNameCounts,
            bool& outIsExternal)
        {
            // 埋め込みテクスチャは%TEMP%配下の一時ファイルなので、入力モデルからの相対を取ると
            // 必ず「外」になる。_External/(外部参照の警告付き)ではなく_Embedded/へ分ける ――
            // 元のモデルファイルの中にあったものであって、参照が外を向いていたわけではないため。
            //
            // 【モデルごとのサブディレクトリへ入れる】埋め込みテクスチャの名前は
            // 「シーン内の配列番号 + 元のファイル名」でモデルの中でしか一意でない。
            // PLATEAUの取り込みのように**複数のモデルを同じ出力ディレクトリへパックする**運用では、
            // 別のタイルの emb0000_17336.jpg が同名になりうる。そして--forceを付けない限り
            // 既存の.ktexはスキップされるため、**後から来たタイルが前のタイルのテクスチャを
            // 黙って使う**(エラーも警告も出ない)。.kmodelの名前で名前空間を分けて塞ぐ
            if (!embeddedTextureDirectory.empty() &&
                texturePath.rfind(embeddedTextureDirectory, 0) == 0)
            {
                outIsExternal = false;
                fs::path embeddedPath = outputDirectory / L"_Embedded" / outputModelStem / fs::path(texturePath).filename();
                embeddedPath += (sRGB ? L".srgb.ktex" : L".linear.ktex");
                return embeddedPath;
            }

            std::error_code ec;
            const fs::path texAbs = fs::weakly_canonical(fs::path(texturePath), ec);
            const fs::path srcDirAbs = fs::weakly_canonical(sourceModelDirectory, ec);

            fs::path relative = ec ? fs::path() : fs::relative(texAbs, srcDirAbs, ec);
            const std::wstring relativeGeneric = ec ? L".." : relative.generic_wstring();

            fs::path outPath;
            outIsExternal = ec || relativeGeneric.empty() || relativeGeneric.rfind(L"..", 0) == 0;
            if (!outIsExternal)
            {
                outPath = outputDirectory / relative;
            }
            else
            {
                const std::wstring baseName = fs::path(texturePath).filename().wstring();
                const int count = externalNameCounts[baseName]++;
                const std::wstring name = count == 0
                    ? baseName
                    : (fs::path(baseName).stem().wstring() + L"_" + std::to_wstring(count) + fs::path(baseName).extension().wstring());
                outPath = outputDirectory / L"_External" / name;
            }

            outPath += (sRGB ? L".srgb.ktex" : L".linear.ktex");
            return outPath;
        }

        // ブロック圧縮(BC1〜BC7)は4x4ピクセルを1ブロックとして符号化するため、幅または高さが
        // 4未満のテクスチャはD3D11/DX12のシェーダリソースビュー作成がE_INVALIDARGで失敗する。
        //
        // 【なぜパック時に弾くのか】配布アセットには「法線マップ無し」を表す1x1のダミーが
        // ブロック圧縮のまま置かれていることがある(NVIDIA Emerald Squareの法線マップ115枚中
        // 6枚が1x1のATI2)。これをそのまま.ktexにすると、.kmodelは正常なテクスチャとして
        // 参照し続け、実行のたびにModelLoaderがGPU転送に失敗してエラーログを出す。
        // パックの時点で分かることを実行時のエラーに先送りしない
        bool IsUnsupportedBlockCompressed(const DirectX::TexMetadata& metadata)
        {
            return DirectX::IsCompressed(metadata.format) && (metadata.width < 4 || metadata.height < 4);
        }

        // 既存の.ktexの中身が上記の条件に当たるかを、DDSペイロードのメタデータから判定する。
        // 判定できない場合(読めない・壊れている)はfalseを返す ―― ここは「使えないものを
        // 見つける」ための検査であり、読めないことを理由に既存の正常なテクスチャを
        // 捨ててはいけない
        bool ExistingKtexIsUnsupported(const fs::path& ktexPath)
        {
            std::ifstream file(ktexPath, std::ios::binary);
            if (!file)
            {
                return false;
            }

            Kurenai::Assets::PackedTextureHeader header{};
            file.read(reinterpret_cast<char*>(&header), sizeof(header));
            if (!file || std::memcmp(header.Magic, Kurenai::Assets::kPackedTextureMagic, sizeof(header.Magic)) != 0)
            {
                return false;
            }

            // DDSヘッダだけ読めれば足りる(DXT10拡張ヘッダを含めても148バイト)
            constexpr size_t kDdsHeaderProbeBytes = 256;
            const size_t probeSize = static_cast<size_t>(
                std::min<uint64_t>(header.PayloadSize, kDdsHeaderProbeBytes));
            std::vector<uint8_t> probe(probeSize);
            file.read(reinterpret_cast<char*>(probe.data()), static_cast<std::streamsize>(probeSize));
            if (!file)
            {
                return false;
            }

            DirectX::TexMetadata metadata{};
            if (FAILED(DirectX::GetMetadataFromDDSMemory(probe.data(), probe.size(), DirectX::DDS_FLAGS_NONE, metadata)))
            {
                return false;
            }
            return IsUnsupportedBlockCompressed(metadata);
        }

        // 頂点/インデックスブロックの境界を16バイトへ切り上げる
        uint64_t AlignUp(uint64_t value, uint64_t alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }
    }

    PackResult WriteModelPackage(
        const SourceModel& sourceModel,
        const std::wstring& outputKModelPath,
        const std::wstring& sourceModelDirectory,
        const PackOptions& options)
    {
        if (sourceModel.Meshes.empty())
        {
            throw std::runtime_error("パック対象のメッシュが1つもありません(入力モデルにジオメトリが含まれていない可能性があります)");
        }

        const fs::path kmodelPath = fs::path(outputKModelPath);
        const fs::path outputDirectory = kmodelPath.parent_path();
        std::error_code ec;
        fs::create_directories(outputDirectory, ec);

        WriteTimings timings;
        const auto collectStart = PhaseClock::now();

        // === 1. ユニークな(パス,sRGB)テクスチャ要求を、メッシュ走査順で決定的に収集する ===
        std::vector<TextureRequest> requests;
        // key = パス + "|srgb"/"|linear" で重複排除。値はrequests内のインデックス
        std::unordered_map<std::wstring, size_t> requestIndexByKey;
        std::unordered_map<std::wstring, int> externalNameCounts;

        // 各メッシュのテクスチャスロットが指すrequestsのインデックス(空パスはkNoRequestで「なし」)
        std::vector<MeshTextureRefs> meshTextureRefs(sourceModel.Meshes.size());

        auto registerRequest = [&](const std::wstring& path, bool sRGB) -> size_t
        {
            if (path.empty())
            {
                return kNoRequest;
            }
            const std::wstring key = path + (sRGB ? L"|srgb" : L"|linear");
            const auto it = requestIndexByKey.find(key);
            if (it != requestIndexByKey.end())
            {
                return it->second;
            }

            TextureRequest request;
            request.SourcePath = path;
            request.SRGB = sRGB;
            bool isExternal = false;
            request.OutputKtexPath = ComputeKtexOutputPath(
                path, sourceModelDirectory, outputDirectory, sRGB,
                options.EmbeddedTextureDirectory, kmodelPath.stem().wstring(), externalNameCounts, isExternal);
            if (isExternal)
            {
                // std::wcerrは出力先が実コンソールでない場合(リダイレクト等)に失敗しうるため
                // 使わない(Main.cppのコメント参照)。パスはWideToUtf8でUTF-8へ変換してから出す
                std::cerr << "[KurenaiPacker][Warning] テクスチャが入力モデルのディレクトリ外を参照しています。_External/へ平坦化します: " << WideToUtf8(path) << "\n";
            }

            const size_t index = requests.size();
            requests.push_back(std::move(request));
            requestIndexByKey.emplace(key, index);
            return index;
        };

        for (size_t i = 0; i < sourceModel.Meshes.size(); ++i)
        {
            const SourceMesh& mesh = sourceModel.Meshes[i];
            meshTextureRefs[i].BaseColor = registerRequest(mesh.BaseColorPath, true);
            meshTextureRefs[i].Normal = registerRequest(mesh.NormalPath, false);
            meshTextureRefs[i].MetallicRoughness = registerRequest(mesh.MetallicRoughnessPath, false);
            meshTextureRefs[i].Emissive = registerRequest(mesh.EmissivePath, true);
            // 遮蔽マップは色ではなく遮蔽率(スカラー)なのでリニア(sRGB=false)で扱う
            meshTextureRefs[i].Occlusion = registerRequest(mesh.OcclusionPath, false);
        }

        // === 2. テクスチャをワーカースレッドで並列処理する ===
        // (デコード・ミップ生成・GPU BC7圧縮はTextureImage::LoadFromFileが行う。GPU圧縮自体は
        // TextureImage内部の専用D3D11デバイスとミューテックスで自動的に直列化されるため、
        // ここでの並列化はWICデコード等の純粋なCPU作業を並列化する意味しか持たないが、
        // それでも大量のテクスチャを持つアセットでは十分な効果がある)
        PackResult result;
        result.MeshCount = sourceModel.Meshes.size();
        result.TextureRequested = requests.size();

        timings.CollectSeconds += PhaseSecondsSince(collectStart);

        const auto skipStart = PhaseClock::now();
        std::vector<size_t> pendingIndices;
        for (size_t i = 0; i < requests.size(); ++i)
        {
            if (!options.Force && fs::exists(requests[i].OutputKtexPath))
            {
                // 既存を再利用する場合も、中身がGPUで扱えない寸法でないかは確かめる。
                // --forceを付けたときにしか検査しない作りにすると、一度生成してしまった
                // 不正な.ktexを.kmodelが参照し続け、実行のたびに転送失敗が出る
                if (ExistingKtexIsUnsupported(requests[i].OutputKtexPath))
                {
                    requests[i].Failed = true;
                    ++result.TextureFailed;
                    std::cerr << "[KurenaiPacker][Warning] 既存の.ktexがブロック圧縮で4x4未満のため参照しません(フォールバックします): "
                        << WideToUtf8(requests[i].SourcePath) << "\n";
                    continue;
                }
                ++result.TextureSkippedExisting;
                continue;
            }
            pendingIndices.push_back(i);
        }

        timings.SkipCheckSeconds += PhaseSecondsSince(skipStart);

        const auto textureStart = PhaseClock::now();
        if (!pendingIndices.empty())
        {
            constexpr unsigned int kMaxWorkers = 8;
            const unsigned int hardwareThreads = options.JobCount != 0
                ? options.JobCount
                : std::min(kMaxWorkers, std::max(1u, std::thread::hardware_concurrency()));
            const unsigned int workerCount = std::min(hardwareThreads, static_cast<unsigned int>(pendingIndices.size()));

            // 【実時間ではなく全ワーカーの累計を取る】和が実時間×ワーカー数に近ければ
            // 全員が働いており、実時間×1に近ければ1本を残して全員が待っている。
            // BC7圧縮はTextureImage内部のミューテックスで直列化されるため、この比が
            // 「スレッドを増やして意味があるのか」を直接決める
            // このモデルのぶんだけを測るため、ワーカーを起こす直前に0へ戻す
            Kurenai::RHI::ResetTextureLoadStats();

            std::atomic<uint64_t> loadNanos{ 0 };
            std::atomic<uint64_t> ddsNanos{ 0 };
            std::atomic<uint64_t> writeNanos{ 0 };

            std::atomic<size_t> nextPending{ 0 };
            std::mutex logMutex;
            std::atomic<size_t> generatedCount{ 0 };
            std::atomic<size_t> failedCount{ 0 };
            std::atomic<size_t> completedCount{ 0 };

            // 【逐次進捗を出す理由】PLATEAUのLOD2は1タイルで1,714枚あり、BC7圧縮は
            // TextureImage内部のミューテックスで直列化される。従来は完了サマリしか出さないため、
            // 数分〜十数分のあいだ「動いているのか止まっているのか」が区別できなかった。
            // 何枚ごとに出すかは総数に応じて決める(小さいアセットで無駄に行を増やさない)
            const size_t progressStep = std::max<size_t>(1, pendingIndices.size() / 20);

            auto workerFn = [&]()
            {
                // WICデコードはCOMを使用するため、ワーカースレッドごとに初期化が必要
                // (未初期化のままだとWIC呼び出しがハングする。ModelLoader::Prefetchの
                // 教訓を踏まえ、パッカーでは最初から入れておく)
                const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                const bool comInitialized = SUCCEEDED(coHr);

                for (;;)
                {
                    const size_t pendingSlot = nextPending.fetch_add(1);
                    if (pendingSlot >= pendingIndices.size())
                    {
                        break;
                    }
                    TextureRequest& request = requests[pendingIndices[pendingSlot]];

                    try
                    {
                        const auto loadStart = PhaseClock::now();
                        Kurenai::RHI::TextureImage image = Kurenai::RHI::TextureImage::LoadFromFile(request.SourcePath, request.SRGB);
                        AddNanos(loadNanos, loadStart);

                        // ブロック圧縮で4x4に満たないものは、.ktexにしてもGPUが受け付けない。
                        // ここで例外にして、下のcatchで「フォールバックする」経路へ流す
                        const DirectX::TexMetadata& metadata = image.GetImage().GetMetadata();
                        if (IsUnsupportedBlockCompressed(metadata))
                        {
                            throw std::runtime_error(
                                "ブロック圧縮テクスチャの寸法が4x4未満のためGPUが扱えません("
                                + std::to_string(metadata.width) + "x" + std::to_string(metadata.height) + ")");
                        }

                        const auto ddsStart = PhaseClock::now();
                        DirectX::Blob blob;
                        const HRESULT hr = DirectX::SaveToDDSMemory(
                            image.GetImage().GetImages(), image.GetImage().GetImageCount(),
                            image.GetImage().GetMetadata(), DirectX::DDS_FLAGS_NONE, blob);
                        if (FAILED(hr))
                        {
                            throw std::runtime_error("DDSエンコードに失敗しました");
                        }
                        AddNanos(ddsNanos, ddsStart);

                        PackedTextureHeader header{};
                        std::memcpy(header.Magic, kPackedTextureMagic, sizeof(kPackedTextureMagic));
                        header.Version = kPackedTextureVersion;
                        header.Flags = request.SRGB ? kPackedTextureFlagSRGB : 0u;
                        header.PayloadSize = blob.GetBufferSize();

                        fs::create_directories(request.OutputKtexPath.parent_path(), ec);

                        std::vector<uint8_t> fileBytes(sizeof(header) + blob.GetBufferSize());
                        std::memcpy(fileBytes.data(), &header, sizeof(header));
                        std::memcpy(fileBytes.data() + sizeof(header), blob.GetBufferPointer(), blob.GetBufferSize());
                        const auto ktexWriteStart = PhaseClock::now();
                        WriteFileAtomic(request.OutputKtexPath, fileBytes.data(), fileBytes.size());
                        AddNanos(writeNanos, ktexWriteStart);

                        generatedCount.fetch_add(1);
                    }
                    catch (const std::exception& e)
                    {
                        request.Failed = true;
                        failedCount.fetch_add(1);
                        std::lock_guard<std::mutex> lock(logMutex);
                        std::cerr << "[KurenaiPacker][Warning] テクスチャの処理に失敗しました(フォールバックします): "
                            << WideToUtf8(request.SourcePath) << " : " << e.what() << "\n";
                    }

                    const size_t done = completedCount.fetch_add(1) + 1;
                    if (done % progressStep == 0 || done == pendingIndices.size())
                    {
                        std::lock_guard<std::mutex> lock(logMutex);
                        std::cout << "[KurenaiPacker]   テクスチャ " << done << "/" << pendingIndices.size()
                            << " (生成 " << generatedCount.load() << " / 失敗 " << failedCount.load() << ")\n";
                    }
                }

                if (comInitialized)
                {
                    CoUninitialize();
                }
            };

            std::vector<std::thread> workers;
            workers.reserve(workerCount);
            for (unsigned int w = 0; w < workerCount; ++w)
            {
                workers.emplace_back(workerFn);
            }
            for (auto& worker : workers)
            {
                worker.join();
            }

            const Kurenai::RHI::TextureLoadStats texStats = Kurenai::RHI::GetTextureLoadStats();
            timings.TexDecodeSeconds = texStats.DecodeSeconds;
            timings.TexMipSeconds = texStats.MipSeconds;
            timings.TexBC7WaitSeconds = texStats.BC7WaitSeconds;
            timings.TexBC7CompressSeconds = texStats.BC7CompressSeconds;
            timings.TexDeviceCreateSeconds = texStats.DeviceCreateSeconds;

            timings.WorkerCount = workerCount;
            timings.WorkerLoadSeconds = static_cast<double>(loadNanos.load()) / 1e9;
            timings.WorkerDdsSeconds = static_cast<double>(ddsNanos.load()) / 1e9;
            timings.WorkerWriteSeconds = static_cast<double>(writeNanos.load()) / 1e9;

            result.TextureGenerated = generatedCount.load();
            // 既存.ktexの検査(上のループ)で数えた分に足し込む。代入にすると消える
            result.TextureFailed += failedCount.load();
        }

        timings.TextureSeconds += PhaseSecondsSince(textureStart);

        const auto entryStart = PhaseClock::now();
        // === 3. TextureEntryを確定させる(失敗したものは除外し、-1として扱う) ===
        std::vector<TextureEntry> textureEntries;
        std::vector<std::string> texturePathStrings; // StringPoolへ書く前段(順序保持)
        std::vector<int32_t> finalIndexByRequest(requests.size(), kNoTextureIndex);
        for (size_t i = 0; i < requests.size(); ++i)
        {
            if (requests[i].Failed)
            {
                continue;
            }
            const fs::path relativeToModel = fs::relative(requests[i].OutputKtexPath, outputDirectory, ec);
            if (ec)
            {
                requests[i].Failed = true;
                ++result.TextureFailed;
                continue;
            }

            TextureEntry entry{};
            entry.Flags = requests[i].SRGB ? kTextureEntryFlagSRGB : 0u;
            texturePathStrings.push_back(ToPackagePathString(relativeToModel));

            finalIndexByRequest[i] = static_cast<int32_t>(textureEntries.size());
            textureEntries.push_back(entry);
        }

        auto resolveTextureIndex = [&](size_t requestIndex) -> int32_t
        {
            return requestIndex == kNoRequest ? kNoTextureIndex : finalIndexByRequest[requestIndex];
        };

        timings.EntrySeconds += PhaseSecondsSince(entryStart);

        const auto occlusionStart = PhaseClock::now();
        // === 3.5 ベイクした遮蔽マップを.ktexとして書き出す ===
        //
        // 元画像を持たない生成物なので、TextureRequest経由(TextureImage::LoadFromFile)の
        // 経路には乗らない。R8のグレースケールからDirectXTexで直接ミップ生成+圧縮する。
        //
        // 圧縮形式はBC4_UNORM(1チャンネル、4bpp)。他のテクスチャが使うBC7は3〜4チャンネル
        // 向けで、遮蔽率のような単一チャンネルには容量も品質も無駄が大きい。BC4ならCPU圧縮でも
        // 十分速いため、BC7で必要だったGPU圧縮デバイスも要らない
        std::vector<int32_t> bakedOcclusionIndexByMesh(sourceModel.Meshes.size(), kNoTextureIndex);
        if (options.BakedOcclusion != nullptr && options.BakedOcclusion->Resolution > 0)
        {
            const OcclusionBakeResult& baked = *options.BakedOcclusion;
            const uint32_t resolution = baked.Resolution;
            const fs::path occlusionDirectory = outputDirectory / L"_Occlusion";

            for (size_t meshIndex = 0; meshIndex < sourceModel.Meshes.size(); ++meshIndex)
            {
                if (meshIndex >= baked.MeshTextures.size() || baked.MeshTextures[meshIndex].empty())
                {
                    continue;
                }
                const std::vector<uint8_t>& pixels = baked.MeshTextures[meshIndex];

                try
                {
                    DirectX::ScratchImage source;
                    HRESULT hr = source.Initialize2D(DXGI_FORMAT_R8_UNORM, resolution, resolution, 1, 1);
                    if (FAILED(hr))
                    {
                        throw std::runtime_error("遮蔽マップの画像確保に失敗しました");
                    }
                    // 行ピッチは要求した幅と一致するとは限らないため、必ず行単位でコピーする
                    const DirectX::Image* destImage = source.GetImage(0, 0, 0);
                    for (uint32_t y = 0; y < resolution; ++y)
                    {
                        std::memcpy(destImage->pixels + y * destImage->rowPitch, pixels.data() + static_cast<size_t>(y) * resolution, resolution);
                    }

                    // TEX_FILTER_FORCE_NON_WIC を必ず付ける。既定のWIC経由の縮小は
                    // R8_UNORMのような単一チャンネル形式を扱えず、E_FAILで落ちる。
                    // 非WICのボックスフィルタなら同じ形式のまま縮小できる
                    DirectX::ScratchImage mipChain;
                    hr = DirectX::GenerateMipMaps(
                        *destImage, DirectX::TEX_FILTER_DEFAULT | DirectX::TEX_FILTER_FORCE_NON_WIC, 0, mipChain);
                    if (FAILED(hr))
                    {
                        throw std::runtime_error("遮蔽マップのミップ生成に失敗しました");
                    }

                    DirectX::ScratchImage compressed;
                    hr = DirectX::Compress(
                        mipChain.GetImages(), mipChain.GetImageCount(), mipChain.GetMetadata(),
                        DXGI_FORMAT_BC4_UNORM, DirectX::TEX_COMPRESS_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, compressed);
                    if (FAILED(hr))
                    {
                        throw std::runtime_error("遮蔽マップのBC4圧縮に失敗しました");
                    }

                    DirectX::Blob blob;
                    hr = DirectX::SaveToDDSMemory(
                        compressed.GetImages(), compressed.GetImageCount(), compressed.GetMetadata(),
                        DirectX::DDS_FLAGS_NONE, blob);
                    if (FAILED(hr))
                    {
                        throw std::runtime_error("遮蔽マップのDDSエンコードに失敗しました");
                    }

                    PackedTextureHeader texHeader{};
                    std::memcpy(texHeader.Magic, kPackedTextureMagic, sizeof(kPackedTextureMagic));
                    texHeader.Version = kPackedTextureVersion;
                    texHeader.Flags = 0u; // 遮蔽率は色ではないのでリニア
                    texHeader.PayloadSize = blob.GetBufferSize();

                    // 出力する.kmodelの名前を接頭辞に入れる。同じディレクトリへ複数のモデルを
                    // パックする(同一ジオメトリのマテリアル違いを並べる検証シーンなど)と、
                    // メッシュ番号だけでは互いの遮蔽マップを上書きしてしまうため
                    const fs::path ktexPath = occlusionDirectory /
                        (fs::path(outputKModelPath).stem().wstring() + L"_Mesh" + std::to_wstring(meshIndex) + L".ktex");
                    fs::create_directories(occlusionDirectory, ec);

                    std::vector<uint8_t> fileBytes(sizeof(texHeader) + blob.GetBufferSize());
                    std::memcpy(fileBytes.data(), &texHeader, sizeof(texHeader));
                    std::memcpy(fileBytes.data() + sizeof(texHeader), blob.GetBufferPointer(), blob.GetBufferSize());
                    WriteFileAtomic(ktexPath, fileBytes.data(), fileBytes.size());

                    const fs::path relativeToModel = fs::relative(ktexPath, outputDirectory, ec);
                    if (ec)
                    {
                        throw std::runtime_error("遮蔽マップの相対パス計算に失敗しました");
                    }

                    TextureEntry entry{};
                    entry.Flags = 0u;
                    texturePathStrings.push_back(ToPackagePathString(relativeToModel));
                    bakedOcclusionIndexByMesh[meshIndex] = static_cast<int32_t>(textureEntries.size());
                    textureEntries.push_back(entry);
                    ++result.OcclusionBaked;
                }
                catch (const std::exception& e)
                {
                    std::cerr << "[KurenaiPacker][Warning] 遮蔽マップの書き出しに失敗しました(遮蔽なしとして扱います) メッシュ["
                        << meshIndex << "]: " << e.what() << "\n";
                }
            }
        }

        timings.OcclusionSeconds += PhaseSecondsSince(occlusionStart);

        const auto bentStart = PhaseClock::now();
        // === bent normal(RGBA16F、圧縮なし)の書き出し ===========================
        //
        // 遮蔽マップと違って圧縮しない。BC4/BC7はいずれも符号なしで、bRawの負の成分を表現できない
        // (BC6H_SF16なら符号付きで扱えるが、第2段階として先送りする)。
        //
        // fp32ではなくfp16なのは、仮数11bitあればモンテカルロノイズ(256本で数%)より
        // 桁違いに細かく、精度が要る検証はベイカー内のfloat32で済ませているため。容量は半分になる
        std::vector<int32_t> bentNormalIndexByMesh(sourceModel.Meshes.size(), kNoTextureIndex);
        if (options.BakedOcclusion != nullptr && options.BakedOcclusion->Resolution > 0)
        {
            const OcclusionBakeResult& baked = *options.BakedOcclusion;
            const uint32_t resolution = baked.Resolution;
            const fs::path bentDirectory = outputDirectory / L"_BentNormal";

            for (size_t meshIndex = 0; meshIndex < sourceModel.Meshes.size(); ++meshIndex)
            {
                if (meshIndex >= baked.MeshBentNormals.size() || baked.MeshBentNormals[meshIndex].empty())
                {
                    continue;
                }
                const std::vector<float>& pixels = baked.MeshBentNormals[meshIndex];

                try
                {
                    // ミップはfp32のまま生成してから一括でfp16へ落とす。
                    // ボックスフィルタが「ベクトルの平均」になる順序であることが重要で、
                    // 長さを取ってから平均するとJensenの不等式より必ず過大評価になる(34章)
                    DirectX::ScratchImage source;
                    HRESULT hr = source.Initialize2D(DXGI_FORMAT_R32G32B32A32_FLOAT, resolution, resolution, 1, 1);
                    if (FAILED(hr))
                    {
                        throw std::runtime_error("bent normalの画像確保に失敗しました");
                    }
                    const DirectX::Image* destImage = source.GetImage(0, 0, 0);
                    const size_t rowBytes = static_cast<size_t>(resolution) * 4 * sizeof(float);
                    for (uint32_t y = 0; y < resolution; ++y)
                    {
                        std::memcpy(
                            destImage->pixels + y * destImage->rowPitch,
                            pixels.data() + static_cast<size_t>(y) * resolution * 4,
                            rowBytes);
                    }

                    // 遮蔽マップと同じ理由でTEX_FILTER_FORCE_NON_WICを必ず付ける
                    // (WIC経路は非8bit形式を扱えずE_FAILで落ちる)
                    DirectX::ScratchImage mipChain;
                    hr = DirectX::GenerateMipMaps(
                        *destImage, DirectX::TEX_FILTER_DEFAULT | DirectX::TEX_FILTER_FORCE_NON_WIC, 0, mipChain);
                    if (FAILED(hr))
                    {
                        throw std::runtime_error("bent normalのミップ生成に失敗しました");
                    }

                    DirectX::ScratchImage half;
                    hr = DirectX::Convert(
                        mipChain.GetImages(), mipChain.GetImageCount(), mipChain.GetMetadata(),
                        DXGI_FORMAT_R16G16B16A16_FLOAT,
                        DirectX::TEX_FILTER_DEFAULT | DirectX::TEX_FILTER_FORCE_NON_WIC,
                        DirectX::TEX_THRESHOLD_DEFAULT, half);
                    if (FAILED(hr))
                    {
                        throw std::runtime_error("bent normalのfp16変換に失敗しました");
                    }

                    DirectX::Blob blob;
                    hr = DirectX::SaveToDDSMemory(
                        half.GetImages(), half.GetImageCount(), half.GetMetadata(),
                        DirectX::DDS_FLAGS_NONE, blob);
                    if (FAILED(hr))
                    {
                        throw std::runtime_error("bent normalのDDSエンコードに失敗しました");
                    }

                    PackedTextureHeader texHeader{};
                    std::memcpy(texHeader.Magic, kPackedTextureMagic, sizeof(kPackedTextureMagic));
                    texHeader.Version = kPackedTextureVersion;
                    texHeader.Flags = 0u; // 方向ベクトルは色ではないのでリニア
                    texHeader.PayloadSize = blob.GetBufferSize();

                    const fs::path ktexPath = bentDirectory /
                        (fs::path(outputKModelPath).stem().wstring() + L"_Mesh" + std::to_wstring(meshIndex) + L".ktex");
                    fs::create_directories(bentDirectory, ec);

                    std::vector<uint8_t> fileBytes(sizeof(texHeader) + blob.GetBufferSize());
                    std::memcpy(fileBytes.data(), &texHeader, sizeof(texHeader));
                    std::memcpy(fileBytes.data() + sizeof(texHeader), blob.GetBufferPointer(), blob.GetBufferSize());
                    WriteFileAtomic(ktexPath, fileBytes.data(), fileBytes.size());

                    const fs::path relativeToModel = fs::relative(ktexPath, outputDirectory, ec);
                    if (ec)
                    {
                        throw std::runtime_error("bent normalの相対パス計算に失敗しました");
                    }

                    TextureEntry entry{};
                    entry.Flags = 0u;
                    texturePathStrings.push_back(ToPackagePathString(relativeToModel));
                    bentNormalIndexByMesh[meshIndex] = static_cast<int32_t>(textureEntries.size());
                    textureEntries.push_back(entry);
                    ++result.BentNormalBaked;
                }
                catch (const std::exception& e)
                {
                    std::cerr << "[KurenaiPacker][Warning] bent normalの書き出しに失敗しました(bent normal無しとして扱います) メッシュ["
                        << meshIndex << "]: " << e.what() << "\n";
                }
            }
        }

        timings.BentNormalSeconds += PhaseSecondsSince(bentStart);

        // === 4. .kgeomを書き出す ===
        //
        // メッシュ順に、メッシュごとの
        // [頂点][インデックス][メッシュレット][メッシュレット頂点][メッシュレット三角形]
        // を16バイト境界で連結する。
        //
        // メッシュレット化は頂点の並びとインデックスの並びの両方を変えるため、
        // 書き出すのは入力のmesh.Vertices/mesh.Indicesではなくこの結果のほう
        std::vector<uint8_t> geometryPayload;
        std::vector<MeshEntry> meshEntries(sourceModel.Meshes.size());
        // マテリアルはメッシュと1対1(パッカーがマテリアル単位で結合するため。ModelPackage.h参照)。
        // 番号が一致するのは実装の都合であって規約ではないので、MeshEntry.MaterialIndexへ明示的に書く
        std::vector<MaterialEntry> materialEntries(sourceModel.Meshes.size());

        // 任意の型の配列を1ブロックとして追記し、書き込み開始位置を返す。
        // ブロックの直後は必ず16バイト境界まで0で埋める(kGeometryBlockAlignment)
        const auto appendBlock = [&geometryPayload](const void* data, size_t byteCount) -> uint64_t {
            const uint64_t offset = geometryPayload.size();
            geometryPayload.resize(geometryPayload.size() + byteCount);
            if (byteCount > 0)
            {
                std::memcpy(geometryPayload.data() + offset, data, byteCount);
            }
            geometryPayload.resize(AlignUp(geometryPayload.size(), kGeometryBlockAlignment), 0);
            return offset;
        };

        for (size_t i = 0; i < sourceModel.Meshes.size(); ++i)
        {
            const SourceMesh& mesh = sourceModel.Meshes[i];
            MeshEntry& entry = meshEntries[i];

            const auto meshletStart = PhaseClock::now();
            MeshletBuildResult meshlets = BuildMeshlets(
                mesh.Vertices, mesh.Indices, options.EnableMeshlets, options.MeshletLODCount);
            timings.MeshletSeconds += PhaseSecondsSince(meshletStart);

            // メッシュレットへ材質番号を焼き込む。MeshletBuilderは材質を知らない(知る必要も無い)ので、
            // 「このメッシュの材質」をここで転記する。メッシュとマテリアルは1対1なのでメッシュ番号でよい
            for (Kurenai::Assets::MeshletEntry& meshlet : meshlets.Meshlets)
            {
                meshlet.MaterialIndex = static_cast<uint32_t>(i);
            }

            const auto appendStart = PhaseClock::now();
            entry.VertexOffset = appendBlock(meshlets.Vertices.data(), meshlets.Vertices.size() * sizeof(Vertex));
            entry.IndexOffset = appendBlock(meshlets.Indices.data(), meshlets.Indices.size() * sizeof(uint32_t));
            entry.MeshletOffset =
                appendBlock(meshlets.Meshlets.data(), meshlets.Meshlets.size() * sizeof(Kurenai::Assets::MeshletEntry));
            entry.MeshletVertexOffset =
                appendBlock(meshlets.MeshletVertices.data(), meshlets.MeshletVertices.size() * sizeof(uint32_t));
            entry.MeshletTriangleOffset =
                appendBlock(meshlets.MeshletTriangles.data(), meshlets.MeshletTriangles.size() * sizeof(uint32_t));
            timings.AppendSeconds += PhaseSecondsSince(appendStart);

            entry.MeshletCount = static_cast<uint32_t>(meshlets.Meshlets.size());
            entry.MeshletVertexCount = static_cast<uint32_t>(meshlets.MeshletVertices.size());
            entry.MeshletTriangleCount = static_cast<uint32_t>(meshlets.MeshletTriangles.size());
            entry.MeshletLODCount = meshlets.LODCount;
            for (uint32_t lod = 0; lod < Kurenai::Assets::kMaxMeshletLODCount; ++lod)
            {
                entry.MeshletLODOffsets[lod] = meshlets.LODMeshletOffsets[lod];
                entry.MeshletLODCounts[lod] = meshlets.LODMeshletCounts[lod];
            }
            result.MeshletCount += meshlets.Meshlets.size();
            result.MeshletLOD0Count += meshlets.LODMeshletCounts[0];
            for (uint32_t lod = 0; lod < Kurenai::Assets::kMaxMeshletLODCount; ++lod)
            {
                result.MeshletTrianglesByLOD[lod] += meshlets.LODTriangleCounts[lod];
            }

            entry.VertexCount = static_cast<uint32_t>(meshlets.Vertices.size());
            entry.IndexCount = static_cast<uint32_t>(meshlets.Indices.size());
            entry.MaterialIndex = static_cast<int32_t>(i);
            entry.Reserved = 0u;

            // === メッシュ単位のAABB(v10で追加) ===
            //
            // **書き出す頂点から作る。** 入力のmesh.Verticesではなくメッシュレット化を通した
            // meshlets.Verticesを見るのは、頂点フェッチ最適化でどの三角形からも参照されない頂点が
            // 落ちることがあり、実際に描かれる範囲はこちらだから。
            //
            // PackageHeaderのAABBはModelSourceが全頂点から作った値をそのまま使い、**ここから
            // 導出しない。**別々に作った2つの値が一致することを検証で確かめられるようにするため
            // (片方をもう片方から作ると、その検証は何も言っていないことになる)
            if (meshlets.Vertices.empty())
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    entry.BoundsMin[axis] = 0.0f;
                    entry.BoundsMax[axis] = 0.0f;
                }
            }
            else
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    entry.BoundsMin[axis] = meshlets.Vertices[0].Position[axis];
                    entry.BoundsMax[axis] = meshlets.Vertices[0].Position[axis];
                }
                for (const Vertex& vertex : meshlets.Vertices)
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        entry.BoundsMin[axis] = std::min(entry.BoundsMin[axis], vertex.Position[axis]);
                        entry.BoundsMax[axis] = std::max(entry.BoundsMax[axis], vertex.Position[axis]);
                    }
                }
            }

            // === 材質はMaterialEntryへ(v10) ===
            MaterialEntry& material = materialEntries[i];
            material.MetallicFactor = mesh.MetallicFactor;
            material.RoughnessFactor = mesh.RoughnessFactor;
            material.AlphaCutoff = mesh.AlphaCutoff;
            material.Translucency = mesh.Translucency;
            material.EmissiveFactor[0] = mesh.EmissiveFactor[0];
            material.EmissiveFactor[1] = mesh.EmissiveFactor[1];
            material.EmissiveFactor[2] = mesh.EmissiveFactor[2];
            material.BaseColorTextureIndex = resolveTextureIndex(meshTextureRefs[i].BaseColor);
            material.NormalTextureIndex = resolveTextureIndex(meshTextureRefs[i].Normal);
            material.MetallicRoughnessTextureIndex = resolveTextureIndex(meshTextureRefs[i].MetallicRoughness);
            material.EmissiveTextureIndex = resolveTextureIndex(meshTextureRefs[i].Emissive);
            material.Flags = mesh.IsTransparent ? kMeshEntryFlagTransparent : 0u;
            material.BaseColorFactor[0] = mesh.BaseColorFactor[0];
            material.BaseColorFactor[1] = mesh.BaseColorFactor[1];
            material.BaseColorFactor[2] = mesh.BaseColorFactor[2];
            material.BaseColorFactor[3] = mesh.BaseColorFactor[3];
            // ベイクした遮蔽マップがあればそちらを優先する(ソースモデル由来の
            // occlusionTextureはTEXCOORD0の空間にあり、ベイク時に生成したライトマップUVとは
            // 座標系が違うため併用できない。--bake-occlusionを指定した時点で
            // 「AOはこちらで作る」という意思表示とみなす)。
            // **ベイク結果はメッシュ単位**なので、材質とメッシュが1対1であることに依存している
            if (bakedOcclusionIndexByMesh[i] != kNoTextureIndex)
            {
                material.OcclusionTextureIndex = bakedOcclusionIndexByMesh[i];
                // ベイク結果はそのまま使ってほしいので強度は1.0固定にする
                material.OcclusionStrength = Kurenai::Assets::kDefaultOcclusionStrength;
            }
            else
            {
                material.OcclusionTextureIndex = resolveTextureIndex(meshTextureRefs[i].Occlusion);
                material.OcclusionStrength = mesh.OcclusionStrength;
            }
            // bent normalはベイクでしか作られない(ソースモデル由来のものは存在しない)
            material.BentNormalTextureIndex = bentNormalIndexByMesh[i];
            material.Reserved = 0u;

            // 入力ではなく書き出した側を数える。頂点キャッシュ最適化で
            // どの三角形からも参照されない頂点が落ちるため、入力より少なくなることがある
            result.VertexCount += meshlets.Vertices.size();
            result.IndexCount += meshlets.Indices.size();
        }

        const fs::path kgeomPath = fs::path(kmodelPath).replace_extension(L".kgeom");
        {
            const ScopedPhase timeGeometryWrite(timings.GeometryWriteSeconds);
            GeometryHeader header{};
            std::memcpy(header.Magic, kGeometryMagic, sizeof(kGeometryMagic));
            header.Version = kGeometryVersion;
            header.VertexStride = sizeof(Vertex);
            header.IndexStride = sizeof(uint32_t);
            header.PayloadSize = geometryPayload.size();

            std::vector<uint8_t> fileBytes(sizeof(header) + geometryPayload.size());
            std::memcpy(fileBytes.data(), &header, sizeof(header));
            if (!geometryPayload.empty())
            {
                std::memcpy(fileBytes.data() + sizeof(header), geometryPayload.data(), geometryPayload.size());
            }
            WriteFileAtomic(kgeomPath, fileBytes.data(), fileBytes.size());
        }

        const auto modelWriteStart = PhaseClock::now();
        // === 5. .kmodelを書き出す(StringPoolを構築してからヘッダ/テーブルをまとめて書く) ===
        std::string stringPool;
        std::vector<TextureEntry> finalTextureEntries = textureEntries;
        for (size_t i = 0; i < finalTextureEntries.size(); ++i)
        {
            finalTextureEntries[i].PathOffset = static_cast<uint32_t>(stringPool.size());
            finalTextureEntries[i].PathLength = static_cast<uint32_t>(texturePathStrings[i].size());
            stringPool += texturePathStrings[i];
        }

        // ライト名(StringPool)を先に確定させる。ライトのPosition/Direction等は
        // ワールド空間ではなくモデルのローカル空間のまま(SceneLoaderがModelInstance::Worldで
        // 変換する。Assets/SceneLoader.cpp参照)そのまま書き出せばよい
        std::vector<LightEntry> lightEntries(sourceModel.Lights.size());
        for (size_t i = 0; i < sourceModel.Lights.size(); ++i)
        {
            const SourceLight& light = sourceModel.Lights[i];
            LightEntry& entry = lightEntries[i];
            entry.Type = static_cast<uint32_t>(light.Type);
            entry.Position[0] = light.Position[0];
            entry.Position[1] = light.Position[1];
            entry.Position[2] = light.Position[2];
            entry.Direction[0] = light.Direction[0];
            entry.Direction[1] = light.Direction[1];
            entry.Direction[2] = light.Direction[2];
            entry.Color[0] = light.Color[0];
            entry.Color[1] = light.Color[1];
            entry.Color[2] = light.Color[2];
            entry.Intensity = light.Intensity;
            entry.Range = light.Range;
            entry.SpotInnerConeAngle = light.SpotInnerConeAngle;
            entry.SpotOuterConeAngle = light.SpotOuterConeAngle;
            entry.Enabled = light.Enabled ? 1u : 0u;
            entry.NameOffset = static_cast<uint32_t>(stringPool.size());
            entry.NameLength = static_cast<uint32_t>(light.Name.size());
            stringPool += light.Name;
        }

        const std::string geometryPathString = ToPackagePathString(kgeomPath.filename());
        const uint32_t geometryPathOffset = static_cast<uint32_t>(stringPool.size());
        stringPool += geometryPathString;

        PackageHeader header{};
        std::memcpy(header.Magic, kPackageMagic, sizeof(kPackageMagic));
        header.Version = kPackageVersion;
        header.VertexStride = sizeof(Vertex);
        header.IndexStride = sizeof(uint32_t);
        header.BoundsMin[0] = sourceModel.BoundsMin[0];
        header.BoundsMin[1] = sourceModel.BoundsMin[1];
        header.BoundsMin[2] = sourceModel.BoundsMin[2];
        header.BoundsMax[0] = sourceModel.BoundsMax[0];
        header.BoundsMax[1] = sourceModel.BoundsMax[1];
        header.BoundsMax[2] = sourceModel.BoundsMax[2];
        header.MeshCount = static_cast<uint32_t>(meshEntries.size());
        header.MaterialCount = static_cast<uint32_t>(materialEntries.size());
        header.TextureCount = static_cast<uint32_t>(finalTextureEntries.size());
        header.LightCount = static_cast<uint32_t>(lightEntries.size());
        header.GeometryPathOffset = geometryPathOffset;
        header.GeometryPathLength = static_cast<uint32_t>(geometryPathString.size());
        header.StringPoolSize = static_cast<uint32_t>(stringPool.size());
        header.Reserved = 0u;

        std::vector<uint8_t> fileBytes;
        fileBytes.resize(
            sizeof(header) + finalTextureEntries.size() * sizeof(TextureEntry) +
            materialEntries.size() * sizeof(MaterialEntry) + meshEntries.size() * sizeof(MeshEntry) +
            lightEntries.size() * sizeof(LightEntry) + stringPool.size());
        size_t writeOffset = 0;
        std::memcpy(fileBytes.data() + writeOffset, &header, sizeof(header));
        writeOffset += sizeof(header);
        if (!finalTextureEntries.empty())
        {
            std::memcpy(fileBytes.data() + writeOffset, finalTextureEntries.data(), finalTextureEntries.size() * sizeof(TextureEntry));
            writeOffset += finalTextureEntries.size() * sizeof(TextureEntry);
        }
        // マテリアルはテクスチャ番号を参照するのでテクスチャの後ろ、メッシュの前(ModelPackage.h参照)
        std::memcpy(fileBytes.data() + writeOffset, materialEntries.data(), materialEntries.size() * sizeof(MaterialEntry));
        writeOffset += materialEntries.size() * sizeof(MaterialEntry);
        std::memcpy(fileBytes.data() + writeOffset, meshEntries.data(), meshEntries.size() * sizeof(MeshEntry));
        writeOffset += meshEntries.size() * sizeof(MeshEntry);
        if (!lightEntries.empty())
        {
            std::memcpy(fileBytes.data() + writeOffset, lightEntries.data(), lightEntries.size() * sizeof(LightEntry));
            writeOffset += lightEntries.size() * sizeof(LightEntry);
        }
        if (!stringPool.empty())
        {
            std::memcpy(fileBytes.data() + writeOffset, stringPool.data(), stringPool.size());
            writeOffset += stringPool.size();
        }

        WriteFileAtomic(kmodelPath, fileBytes.data(), fileBytes.size());
        timings.ModelWriteSeconds += PhaseSecondsSince(modelWriteStart);

        result.Timings = timings;
        return result;
    }
}
