#include "PackageWriter.h"

#include <Windows.h>
#include <objbase.h>

#include <DirectXTex.h>

#include "RHI/TextureImage.h"
#include "Assets/ModelPackage.h"
#include "Core/StringUtil.h"

#include <algorithm>
#include <atomic>
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
        // 一時パスへ書いてから完了時のみ本来のパスへリネームする(ランタイム側の
        // .kmodelcache/.ktexcacheと同じ設計。処理が中断されても中途半端な一時
        // ファイルが残るだけで、既存の正常なファイルには影響しない)
        void WriteFileAtomic(const fs::path& path, const void* data, size_t size)
        {
            const fs::path tmp = fs::path(path).concat(L".tmp");
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
            if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                throw std::runtime_error("ファイルの確定(rename)に失敗しました: " + WideToUtf8(path.wstring()));
            }
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
            std::unordered_map<std::wstring, int>& externalNameCounts,
            bool& outIsExternal)
        {
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
            request.OutputKtexPath = ComputeKtexOutputPath(path, sourceModelDirectory, outputDirectory, sRGB, externalNameCounts, isExternal);
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

        std::vector<size_t> pendingIndices;
        for (size_t i = 0; i < requests.size(); ++i)
        {
            if (!options.Force && fs::exists(requests[i].OutputKtexPath))
            {
                ++result.TextureSkippedExisting;
                continue;
            }
            pendingIndices.push_back(i);
        }

        if (!pendingIndices.empty())
        {
            constexpr unsigned int kMaxWorkers = 8;
            const unsigned int hardwareThreads = options.JobCount != 0
                ? options.JobCount
                : std::min(kMaxWorkers, std::max(1u, std::thread::hardware_concurrency()));
            const unsigned int workerCount = std::min(hardwareThreads, static_cast<unsigned int>(pendingIndices.size()));

            std::atomic<size_t> nextPending{ 0 };
            std::mutex logMutex;
            std::atomic<size_t> generatedCount{ 0 };
            std::atomic<size_t> failedCount{ 0 };

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
                        Kurenai::RHI::TextureImage image = Kurenai::RHI::TextureImage::LoadFromFile(request.SourcePath, request.SRGB);

                        DirectX::Blob blob;
                        const HRESULT hr = DirectX::SaveToDDSMemory(
                            image.GetImage().GetImages(), image.GetImage().GetImageCount(),
                            image.GetImage().GetMetadata(), DirectX::DDS_FLAGS_NONE, blob);
                        if (FAILED(hr))
                        {
                            throw std::runtime_error("DDSエンコードに失敗しました");
                        }

                        PackedTextureHeader header{};
                        std::memcpy(header.Magic, kPackedTextureMagic, sizeof(kPackedTextureMagic));
                        header.Version = kPackedTextureVersion;
                        header.Flags = request.SRGB ? kPackedTextureFlagSRGB : 0u;
                        header.PayloadSize = blob.GetBufferSize();

                        fs::create_directories(request.OutputKtexPath.parent_path(), ec);

                        std::vector<uint8_t> fileBytes(sizeof(header) + blob.GetBufferSize());
                        std::memcpy(fileBytes.data(), &header, sizeof(header));
                        std::memcpy(fileBytes.data() + sizeof(header), blob.GetBufferPointer(), blob.GetBufferSize());
                        WriteFileAtomic(request.OutputKtexPath, fileBytes.data(), fileBytes.size());

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

            result.TextureGenerated = generatedCount.load();
            result.TextureFailed = failedCount.load();
        }

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
                    // R8_UNORMのような単一チャンネル形式を扱えず、E_FAILで落ちる(実際に発生)。
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

        // === 4. .kgeomを書き出す(メッシュ順に頂点/インデックスブロックを16バイト境界で連結) ===
        std::vector<uint8_t> geometryPayload;
        std::vector<MeshEntry> meshEntries(sourceModel.Meshes.size());
        for (size_t i = 0; i < sourceModel.Meshes.size(); ++i)
        {
            const SourceMesh& mesh = sourceModel.Meshes[i];
            MeshEntry& entry = meshEntries[i];

            entry.VertexOffset = geometryPayload.size();
            const size_t vertexBytes = mesh.Vertices.size() * sizeof(Vertex);
            geometryPayload.resize(geometryPayload.size() + vertexBytes);
            if (vertexBytes > 0)
            {
                std::memcpy(geometryPayload.data() + entry.VertexOffset, mesh.Vertices.data(), vertexBytes);
            }
            geometryPayload.resize(AlignUp(geometryPayload.size(), kGeometryBlockAlignment), 0);

            entry.IndexOffset = geometryPayload.size();
            const size_t indexBytes = mesh.Indices.size() * sizeof(uint32_t);
            geometryPayload.resize(geometryPayload.size() + indexBytes);
            if (indexBytes > 0)
            {
                std::memcpy(geometryPayload.data() + entry.IndexOffset, mesh.Indices.data(), indexBytes);
            }
            geometryPayload.resize(AlignUp(geometryPayload.size(), kGeometryBlockAlignment), 0);

            entry.VertexCount = static_cast<uint32_t>(mesh.Vertices.size());
            entry.IndexCount = static_cast<uint32_t>(mesh.Indices.size());
            entry.MetallicFactor = mesh.MetallicFactor;
            entry.RoughnessFactor = mesh.RoughnessFactor;
            entry.AlphaCutoff = mesh.AlphaCutoff;
            entry.EmissiveFactor[0] = mesh.EmissiveFactor[0];
            entry.EmissiveFactor[1] = mesh.EmissiveFactor[1];
            entry.EmissiveFactor[2] = mesh.EmissiveFactor[2];
            entry.BaseColorTextureIndex = resolveTextureIndex(meshTextureRefs[i].BaseColor);
            entry.NormalTextureIndex = resolveTextureIndex(meshTextureRefs[i].Normal);
            entry.MetallicRoughnessTextureIndex = resolveTextureIndex(meshTextureRefs[i].MetallicRoughness);
            entry.EmissiveTextureIndex = resolveTextureIndex(meshTextureRefs[i].Emissive);
            entry.Flags = mesh.IsTransparent ? kMeshEntryFlagTransparent : 0u;
            entry.Reserved = 0u;
            entry.BaseColorFactor[0] = mesh.BaseColorFactor[0];
            entry.BaseColorFactor[1] = mesh.BaseColorFactor[1];
            entry.BaseColorFactor[2] = mesh.BaseColorFactor[2];
            entry.BaseColorFactor[3] = mesh.BaseColorFactor[3];
            // ベイクした遮蔽マップがあればそちらを優先する(ソースモデル由来の
            // occlusionTextureはTEXCOORD0の空間にあり、ベイク時に生成したライトマップUVとは
            // 座標系が違うため併用できない。--bake-occlusionを指定した時点で
            // 「AOはこちらで作る」という意思表示とみなす)
            if (bakedOcclusionIndexByMesh[i] != kNoTextureIndex)
            {
                entry.OcclusionTextureIndex = bakedOcclusionIndexByMesh[i];
                // ベイク結果はそのまま使ってほしいので強度は1.0固定にする
                entry.OcclusionStrength = Kurenai::Assets::kDefaultOcclusionStrength;
            }
            else
            {
                entry.OcclusionTextureIndex = resolveTextureIndex(meshTextureRefs[i].Occlusion);
                entry.OcclusionStrength = mesh.OcclusionStrength;
            }

            result.VertexCount += mesh.Vertices.size();
            result.IndexCount += mesh.Indices.size();
        }

        const fs::path kgeomPath = fs::path(kmodelPath).replace_extension(L".kgeom");
        {
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
        header.TextureCount = static_cast<uint32_t>(finalTextureEntries.size());
        header.LightCount = static_cast<uint32_t>(lightEntries.size());
        header.GeometryPathOffset = geometryPathOffset;
        header.GeometryPathLength = static_cast<uint32_t>(geometryPathString.size());
        header.StringPoolSize = static_cast<uint32_t>(stringPool.size());

        std::vector<uint8_t> fileBytes;
        fileBytes.resize(
            sizeof(header) + finalTextureEntries.size() * sizeof(TextureEntry) + meshEntries.size() * sizeof(MeshEntry) +
            lightEntries.size() * sizeof(LightEntry) + stringPool.size());
        size_t writeOffset = 0;
        std::memcpy(fileBytes.data() + writeOffset, &header, sizeof(header));
        writeOffset += sizeof(header);
        if (!finalTextureEntries.empty())
        {
            std::memcpy(fileBytes.data() + writeOffset, finalTextureEntries.data(), finalTextureEntries.size() * sizeof(TextureEntry));
            writeOffset += finalTextureEntries.size() * sizeof(TextureEntry);
        }
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

        return result;
    }
}
