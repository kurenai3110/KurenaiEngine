#include "ModelLoader.h"

#include <Windows.h>

#include <assimp/GltfMaterial.h>
#include <assimp/Importer.hpp>
#include <assimp/light.h>
#include <assimp/metadata.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Core/Logger.h"
#include "RHI/TextureImage.h"
#include "Vertex.h"

namespace Kurenai::Assets
{
    namespace
    {
        std::string WideToUtf8(const std::wstring& wide)
        {
            if (wide.empty())
            {
                return {};
            }

            int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string narrow(length, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, narrow.data(), length, nullptr, nullptr);
            narrow.resize(length - 1);
            return narrow;
        }

        std::wstring Utf8ToWide(const std::string& narrow)
        {
            if (narrow.empty())
            {
                return {};
            }

            int length = MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, nullptr, 0);
            std::wstring wide(length, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, wide.data(), length);
            wide.resize(length - 1);
            return wide;
        }

        std::wstring GetDirectory(const std::wstring& filePath)
        {
            size_t pos = filePath.find_last_of(L"/\\");
            return pos == std::wstring::npos ? L"" : filePath.substr(0, pos + 1);
        }

        bool FileExists(const std::wstring& path)
        {
            return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        }

        // FBXは「Textures/xxx.dds」のようなパスやファイル名のみを格納している場合があり、
        // モデルからの相対パスをそのまま連結しただけでは見つからないことがあるため複数候補を試す
        std::wstring ResolveTexturePath(const std::wstring& directory, const std::wstring& rawPath)
        {
            std::wstring candidate = directory + rawPath;
            if (FileExists(candidate))
            {
                return candidate;
            }

            const size_t slashPos = rawPath.find_last_of(L"/\\");
            const std::wstring fileName = slashPos == std::wstring::npos ? rawPath : rawPath.substr(slashPos + 1);

            candidate = directory + fileName;
            if (FileExists(candidate))
            {
                return candidate;
            }

            candidate = directory + L"Textures\\" + fileName;
            if (FileExists(candidate))
            {
                return candidate;
            }

            return directory + rawPath;
        }

        // 位置・法線が一致する頂点は、assimp内部では複数の面にまたがって別インスタンスとして
        // 複製されていても本来同一の滑らかな表面点であるはずだが、CalcTangentSpace相当の計算を
        // 面ごとに独立して行うと数値誤差で接線がばらつき、UV面積がほぼ0の縮退三角形が絡むと
        // 接線がほぼ正反対になることすらある(法線マップがパッチワーク状に破綻して見える)。
        // そのため「位置+法線」をキーに接線を蓄積・平均化して補う。
        // UVはキーに含めない: 円筒状UV展開の継ぎ目(位置・法線は連続だがUVだけジャンプする、
        // ごく普通のシームレス継ぎ目)ではUVが異なっても平均化すべきであり、キーにUVを含めると
        // そこで平均化がブロックされて継ぎ目が硬い線として見えてしまう(実際にBistroのワイン
        // グラスで確認済み)。ミラーUV(左右反転コピー)のような本当に別方向を向く継ぎ目は
        // 通常「位置」自体が完全一致しない(モデリング上、鏡像コピーは別ジオメトリになる)ため、
        // このキーで誤って混ざることは実用上ほぼない
        struct TangentAccumKey
        {
            float Px, Py, Pz, Nx, Ny, Nz;

            bool operator==(const TangentAccumKey& other) const
            {
                return Px == other.Px && Py == other.Py && Pz == other.Pz &&
                    Nx == other.Nx && Ny == other.Ny && Nz == other.Nz;
            }
        };

        struct TangentAccumKeyHash
        {
            size_t operator()(const TangentAccumKey& key) const
            {
                const std::hash<float> hasher;
                size_t seed = hasher(key.Px);
                for (float f : { key.Py, key.Pz, key.Nx, key.Ny, key.Nz })
                {
                    seed ^= hasher(f) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
                }
                return seed;
            }
        };

        void CollectMeshNodes(
            const aiScene* scene,
            const aiNode* node,
            const aiMatrix4x4& parentTransform,
            std::vector<std::pair<const aiMesh*, aiMatrix4x4>>& out)
        {
            aiMatrix4x4 transform = parentTransform * node->mTransformation;

            for (unsigned int i = 0; i < node->mNumMeshes; ++i)
            {
                out.emplace_back(scene->mMeshes[node->mMeshes[i]], transform);
            }

            for (unsigned int i = 0; i < node->mNumChildren; ++i)
            {
                CollectMeshNodes(scene, node->mChildren[i], transform, out);
            }
        }

        // aiLight::mName に入っているノード名からワールド変換行列(と、Rangeのメタデータを読むための
        // aiNode自体)を引くための走査。glTF2/FBXの両インポータともaiLight::mPositionは(0,0,0)固定、
        // mDirectionはノードローカルの固定値のままで、ライトが付いていたノードの名前がmNameに入る
        // だけなので、位置・向きはこの経路(ノードのワールド変換を掛ける)で求める必要がある。
        // なおaiProcess_ConvertToLeftHandedはaiScene::mLights自体を変換しないが、ノード変換は
        // メッシュと同じく左手系へミラー済みなので、この経路なら正しい結果になる
        struct LightNodeRecord
        {
            const aiNode* Node = nullptr;
            aiMatrix4x4 WorldTransform;
        };

        void CollectNodeWorldTransforms(
            const aiNode* node,
            const aiMatrix4x4& parentTransform,
            std::unordered_map<std::string, LightNodeRecord>& out)
        {
            aiMatrix4x4 transform = parentTransform * node->mTransformation;
            // 同名ノードが複数ある場合はemplaceにより最初に見つかったものを採用する
            out.emplace(std::string(node->mName.C_Str()), LightNodeRecord{ node, transform });

            for (unsigned int i = 0; i < node->mNumChildren; ++i)
            {
                CollectNodeWorldTransforms(node->mChildren[i], transform, out);
            }
        }

        // aiScene::mLightsをAssets::Lightへ変換する。呼び出し元はscene->mNumLights > 0のときだけ呼ぶ
        void ImportLights(const aiScene* scene, const std::wstring& filePath, std::vector<Light>& outLights)
        {
            // FBXにはglTFのKHR_lights_punctualのような物理単位(カンデラ/ルクス)が無く、
            // assimpはDCC側のIntensity(相対値)/100を色に畳み込むだけのため、後段でカンデラ相当の
            // 近似値として扱っていることをInfoログで明示する(事実と異なる精度を主張しないため)
            const bool isFbxSource = filePath.size() >= 4 &&
                _wcsicmp(filePath.c_str() + filePath.size() - 4, L".fbx") == 0;

            std::unordered_map<std::string, LightNodeRecord> nodeTransforms;
            CollectNodeWorldTransforms(scene->mRootNode, aiMatrix4x4(), nodeTransforms);

            // KurenaiEngine3D::m_SceneExposureEV100の既定値と合わせる(Range未指定時の推定にのみ使う)
            constexpr float kDefaultExposureEV100 = 15.0f;
            constexpr float kCutoffRadiance = 0.01f;
            constexpr float kHalfPi = 1.57079632679f;

            for (unsigned int i = 0; i < scene->mNumLights; ++i)
            {
                const aiLight* light = scene->mLights[i];
                const std::string name = light->mName.C_Str();

                LightType type;
                bool enabledByDefault = true;
                switch (light->mType)
                {
                case aiLightSource_POINT:
                    type = LightType::Point;
                    break;
                case aiLightSource_SPOT:
                    type = LightType::Spot;
                    break;
                case aiLightSource_DIRECTIONAL:
                    // b0の太陽(平行光)との二重照明を避けるため既定で無効にする。ImGuiから有効化できる
                    type = LightType::Directional;
                    enabledByDefault = false;
                    break;
                default:
                    // aiLightSource_AMBIENT(環境光はAmbientColorが担当)・aiLightSource_AREA
                    // (エリアライトは今回未実装。FBXのエリアライトはassimpがUNDEFINEDへ落とす)・
                    // aiLightSource_UNDEFINEDはここでまとめてスキップする
                    Core::Logger::Warning("ModelLoader", "未対応のライト種別のためスキップします: " + name);
                    continue;
                }

                const auto nodeIt = nodeTransforms.find(name);
                if (nodeIt == nodeTransforms.end())
                {
                    Core::Logger::Warning("ModelLoader", "ライトに対応するノードが見つかりません: " + name);
                    continue;
                }
                const aiMatrix4x4& transform = nodeIt->second.WorldTransform;

                const aiVector3D worldPosition = transform * aiVector3D(0.0f, 0.0f, 0.0f);
                const aiMatrix3x3 rotation(transform);
                aiVector3D worldDirection = rotation * light->mDirection;
                if (worldDirection.SquareLength() < 1e-12f)
                {
                    // ポイントライトはmDirectionが(0,0,0)のままなので、既定の下向きにフォールバックする
                    worldDirection = aiVector3D(0.0f, -1.0f, 0.0f);
                }
                worldDirection.Normalize();

                // 色/強度分離: 最大成分を測光量(カンデラ/ルクス)、残りを最大成分1に正規化した色として
                // 保持する。glTFはこの値が仕様どおり正確な測光量、FBXはDCC側のIntensityを近似したもの
                const aiColor3D& diffuse = light->mColorDiffuse;
                const float maxComponent = std::max({ diffuse.r, diffuse.g, diffuse.b });
                if (maxComponent <= 0.0f)
                {
                    Core::Logger::Warning("ModelLoader", "ライトの色/強度が0のためスキップします: " + name);
                    continue;
                }

                Light outLight;
                outLight.Type = type;
                outLight.Position[0] = worldPosition.x;
                outLight.Position[1] = worldPosition.y;
                outLight.Position[2] = worldPosition.z;
                outLight.Direction[0] = worldDirection.x;
                outLight.Direction[1] = worldDirection.y;
                outLight.Direction[2] = worldDirection.z;
                outLight.Color[0] = diffuse.r / maxComponent;
                outLight.Color[1] = diffuse.g / maxComponent;
                outLight.Color[2] = diffuse.b / maxComponent;
                outLight.Intensity = maxComponent;
                outLight.Enabled = enabledByDefault;
                outLight.Name = name;

                if (isFbxSource)
                {
                    Core::Logger::Info(
                        "ModelLoader",
                        "ライト\"" + name + "\"の強度はFBXのIntensityから近似したカンデラ相当値です(精度は保証されません)");
                }

                // Range: glTFの場合はノードのメタデータ"PBR_LightRange"を優先する。無ければ、
                // 打ち切り輝度(既定EV100=15.0での露出後の値)から逆算した推定値を使う。
                // mAttenuationConstant/Linear/Quadraticはformatによって意味が異なる(glTFは常に
                // (0,0,1)、FBXはDecayTypeに応じて2/decayや2/decay^2)ため、正規化には使わない
                float range = 0.0f;
                const bool hasExplicitRange =
                    nodeIt->second.Node->mMetaData != nullptr && nodeIt->second.Node->mMetaData->Get("PBR_LightRange", range) && range > 0.0f;
                if (!hasExplicitRange)
                {
                    const float exposure = 1.0f / (1.2f * std::pow(2.0f, kDefaultExposureEV100));
                    const float effectiveRadiance = outLight.Intensity * exposure;
                    range = std::clamp(std::sqrt(effectiveRadiance / kCutoffRadiance), 0.1f, 1000.0f);
                    Core::Logger::Info("ModelLoader", "ライト\"" + name + "\"のRangeを推定しました: " + std::to_string(range));
                }
                outLight.Range = range;

                // コーン角はSPOTのときだけ読む(aiLightの既定値は2πなのでPOINTで読むと壊れる)
                if (type == LightType::Spot)
                {
                    float inner = light->mAngleInnerCone;
                    float outer = light->mAngleOuterCone;
                    if (inner > outer)
                    {
                        std::swap(inner, outer);
                        Core::Logger::Warning("ModelLoader", "スポットライトの内側角が外側角より大きいため入れ替えました: " + name);
                    }
                    outer = std::clamp(outer, 0.0f, kHalfPi);
                    inner = std::clamp(inner, 0.0f, outer);
                    outLight.SpotInnerConeAngle = inner;
                    outLight.SpotOuterConeAngle = outer;
                }

                outLights.push_back(std::move(outLight));
            }
        }

        // テクスチャの読み込みとキャッシュ・共有インスタンス(白/フラット法線)の管理を、
        // assimp経由の読み込みとバイナリキャッシュ経由の読み込みの両方で共通して使う
        class TextureLoader
        {
        public:
            TextureLoader(RHI::IRHIDevice& device, std::wstring directory, Model& model)
                : m_Device(device)
                , m_Directory(std::move(directory))
                , m_Model(model)
            {
            }

            RHI::IRHITexture* Load(const std::string& path, bool sRGB)
            {
                auto it = m_Cache.find(path);
                if (it != m_Cache.end())
                {
                    return it->second;
                }

                RHI::IRHITexture* rawPtr = nullptr;
                try
                {
                    const std::wstring fullPath = ResolveTexturePath(m_Directory, Utf8ToWide(path));
                    auto texture = m_Device.CreateTextureFromFile(fullPath, sRGB);
                    rawPtr = texture.get();
                    m_Model.Textures.push_back(std::move(texture));
                }
                catch (const std::exception& e)
                {
                    // 読み込みに失敗したテクスチャは目立つ色のプレースホルダーで代替し、モデル全体の読み込みは継続する
                    Core::Logger::Error("ModelLoader", "テクスチャの読み込みに失敗しました (" + path + "): " + e.what());
                    auto texture = m_Device.CreateSolidColorTexture(255, 0, 255, 255);
                    rawPtr = texture.get();
                    m_Model.Textures.push_back(std::move(texture));
                }

                m_Cache.emplace(path, rawPtr);
                return rawPtr;
            }

            // 法線マップ専用のロード関数。読み込みに失敗した場合、Load()と同じマゼンタの
            // プレースホルダーにフォールバックすると、ピクセルシェーダーがそれをタンジェント
            // 空間法線(1,0,1)として解釈してしまい、幾何学的にありえない方向の法線が
            // 生成されてしまう(実際にBistroのTransparentGlass_Normal.ddsが1x1のBC5圧縮という
            // 不正な形式でSRV生成に失敗し、この問題を引き起こしていたことをRenderDocで確認済み)。
            // そのため法線マップの読み込み失敗時は「法線マップなし」を表す平坦法線にフォールバックする
            RHI::IRHITexture* LoadNormal(const std::string& path)
            {
                auto it = m_Cache.find(path);
                if (it != m_Cache.end())
                {
                    return it->second;
                }

                RHI::IRHITexture* rawPtr = nullptr;
                try
                {
                    const std::wstring fullPath = ResolveTexturePath(m_Directory, Utf8ToWide(path));
                    auto texture = m_Device.CreateTextureFromFile(fullPath, false);
                    rawPtr = texture.get();
                    m_Model.Textures.push_back(std::move(texture));
                }
                catch (const std::exception& e)
                {
                    Core::Logger::Error("ModelLoader", "法線マップの読み込みに失敗しました (" + path + "): " + e.what());
                    rawPtr = GetFlatNormal();
                }

                m_Cache.emplace(path, rawPtr);
                return rawPtr;
            }

            // Prefetchに渡すテクスチャ読み込み要求。IsNormalMapは失敗時のフォールバック先
            // (マゼンタの単色テクスチャ or フラット法線)を選ぶためだけに使う
            struct PrefetchRequest
            {
                std::string Path;
                bool SRGB = false;
                bool IsNormalMap = false;
            };

            // requestsに含まれるテクスチャを論理コア数ぶんのワーカースレッドで並列にデコードし、
            // 完了したものから随時このスレッド(呼び出し元)でGPUリソース化してm_Cacheへ登録する。
            // デコード(TextureImage::LoadFromFile、CPU処理でDirectXTexのWIC/BC7圧縮を含む)は
            // GPUデバイスを必要としないためワーカーで並列化できるが、GPUリソース作成
            // (device.CreateTextureFromImage)はデバイスに紐づく処理のためこのスレッドで直列に行う。
            // 呼び出し後は、Load/LoadNormalが同じpathに対してキャッシュヒットで即座に返るようになる
            // (フォールバック処理自体はLoad/LoadNormalと同じ内容をここでも個別に行っており、
            // 両者の分岐を変えたわけではない)
            void Prefetch(const std::vector<PrefetchRequest>& requests)
            {
                std::vector<const PrefetchRequest*> pending;
                std::unordered_set<std::string> seen;
                for (const auto& request : requests)
                {
                    if (request.Path.empty() || m_Cache.find(request.Path) != m_Cache.end())
                    {
                        continue;
                    }
                    if (!seen.insert(request.Path).second)
                    {
                        continue;
                    }
                    pending.push_back(&request);
                }
                if (pending.empty())
                {
                    return;
                }

                // BC7圧縮はGPU(コンピュートシェーダー)で行うようになった(TextureImage::CompressBC7)ため、
                // ここでの並列化はもはやCPUの奪い合いにならない: 各ワーカーが行うWICデコード/ミップ生成は
                // 純粋なCPU作業でコア数ぶん安全に並列化できる一方、GPU圧縮そのものは共有のGPUデバイスを
                // 使うためTextureImage側のミューテックスで自動的に直列化される(イミディエイトコンテキストは
                // 複数スレッドから同時に使えないため)。論理コア数をそのまま使うと(BC7ソフトウェア圧縮への
                // フォールバック時など)過剰になりうるため、上限を設けておく
                constexpr unsigned int kMaxPrefetchWorkers = 8;
                const unsigned int hardwareThreads = std::min(kMaxPrefetchWorkers, std::max(1u, std::thread::hardware_concurrency()));
                const unsigned int workerCount = std::min(hardwareThreads, static_cast<unsigned int>(pending.size()));

                struct CompletedItem
                {
                    const PrefetchRequest* Request = nullptr;
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
                        if (index >= pending.size())
                        {
                            break;
                        }
                        const PrefetchRequest* request = pending[index];

                        CompletedItem item;
                        item.Request = request;
                        try
                        {
                            const std::wstring fullPath = ResolveTexturePath(m_Directory, Utf8ToWide(request->Path));
                            RHI::TextureImage image = RHI::TextureImage::LoadFromFile(fullPath, request->SRGB);
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

                for (size_t consumed = 0; consumed < pending.size(); ++consumed)
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
                        RHI::IRHITexture* rawPtr = nullptr;
                        try
                        {
                            auto texture = m_Device.CreateTextureFromImage(*item.Image);
                            rawPtr = texture.get();
                            m_Model.Textures.push_back(std::move(texture));
                        }
                        catch (const std::exception& e)
                        {
                            Core::Logger::Error("ModelLoader", "テクスチャのGPU転送に失敗しました (" + item.Request->Path + "): " + e.what());
                            if (item.Request->IsNormalMap)
                            {
                                rawPtr = GetFlatNormal();
                            }
                            else
                            {
                                auto placeholder = m_Device.CreateSolidColorTexture(255, 0, 255, 255);
                                rawPtr = placeholder.get();
                                m_Model.Textures.push_back(std::move(placeholder));
                            }
                        }
                        m_Cache.emplace(item.Request->Path, rawPtr);
                    }
                    else if (item.Request->IsNormalMap)
                    {
                        Core::Logger::Error("ModelLoader", "法線マップの読み込みに失敗しました (" + item.Request->Path + "): " + item.ErrorMessage);
                        m_Cache.emplace(item.Request->Path, GetFlatNormal());
                    }
                    else
                    {
                        Core::Logger::Error("ModelLoader", "テクスチャの読み込みに失敗しました (" + item.Request->Path + "): " + item.ErrorMessage);
                        auto texture = m_Device.CreateSolidColorTexture(255, 0, 255, 255);
                        RHI::IRHITexture* rawPtr = texture.get();
                        m_Model.Textures.push_back(std::move(texture));
                        m_Cache.emplace(item.Request->Path, rawPtr);
                    }
                }

                for (auto& worker : workers)
                {
                    worker.join();
                }
            }

            RHI::IRHITexture* GetWhite()
            {
                if (!m_White)
                {
                    auto texture = m_Device.CreateSolidColorTexture(255, 255, 255, 255);
                    m_White = texture.get();
                    m_Model.Textures.push_back(std::move(texture));
                }
                return m_White;
            }

            RHI::IRHITexture* GetFlatNormal()
            {
                if (!m_FlatNormal)
                {
                    // タンジェント空間で(0,0,1)、すなわち「法線マップなし」を表す色
                    auto texture = m_Device.CreateSolidColorTexture(128, 128, 255, 255);
                    m_FlatNormal = texture.get();
                    m_Model.Textures.push_back(std::move(texture));
                }
                return m_FlatNormal;
            }

        private:
            RHI::IRHIDevice& m_Device;
            std::wstring m_Directory;
            Model& m_Model;
            std::unordered_map<std::string, RHI::IRHITexture*> m_Cache;
            RHI::IRHITexture* m_White = nullptr;
            RHI::IRHITexture* m_FlatNormal = nullptr;
        };

        // === モデル読み込みキャッシュ ===
        // assimpによるFBX/glTFの解析(特にJoinIdenticalVerticesを外した後も残る純粋なパース処理)は
        // 大規模アセットで数秒〜十数秒かかるため、一度読み込んだモデルの頂点/インデックス/マテリアル
        // 参照をバイナリファイルにキャッシュし、ソースファイルが更新されていない限り2回目以降の
        // 読み込みではassimpを経由せずキャッシュから直接構築する

        constexpr char kCacheMagic[4] = { 'K', 'M', 'C', '1' };
        // v11: エミッシブ(自発光)テクスチャ・係数とアルファカットアウト(alphaCutoff)への対応に伴い、
        // メッシュレコードのフィールド構成が変わったため加算
        // v12: aiScene::mLightsをキャッシュへ追加したため加算。バージョン不一致の古いキャッシュは
        // TryLoadModelFromCacheが即座に拒否して丸ごと再生成するため、レイアウト互換性は不要
        constexpr uint32_t kCacheVersion = 12;

        struct CacheHeader
        {
            char Magic[4];
            uint32_t Version;
            uint64_t SourceFileTime;
            uint64_t SourceFileSize;
            float BoundsMin[3];
            float BoundsMax[3];
            uint32_t MeshCount;
            uint32_t LightCount;
        };

        // Light(Model.h)のPOD部分の1対1対応。std::string Nameを含むためLightそのものはmemcpy
        // できず、Nameだけ既存のWriteCacheString/ReadCacheStringで別途書く(下記参照)
        struct CachedLightRecord
        {
            uint32_t Type;
            float Position[3];
            float Direction[3];
            float Color[3];
            float Intensity;
            float Range;
            float SpotInnerConeAngle;
            float SpotOuterConeAngle;
            uint32_t Enabled;
        };

        std::wstring GetCachePath(const std::wstring& filePath)
        {
            return filePath + L".kmodelcache";
        }

        // 書き込み中のキャッシュはこの一時パスに書き、完了後にGetCachePath()へリネームする
        // (下のLoadModel参照)
        std::wstring GetCacheTempPath(const std::wstring& filePath)
        {
            return GetCachePath(filePath) + L".tmp";
        }

        bool GetFileTimeAndSize(const std::wstring& path, uint64_t& outTime, uint64_t& outSize)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            {
                return false;
            }
            outTime = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
            outSize = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            return true;
        }

        // 2時点間の経過時間をミリ秒の整数文字列にする(ログ表示用)
        std::string FormatMs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
        {
            return std::to_string(static_cast<long long>(std::chrono::duration<double, std::milli>(end - start).count()));
        }

        void WriteCacheString(std::ofstream& out, const std::string& s)
        {
            const uint32_t length = static_cast<uint32_t>(s.size());
            out.write(reinterpret_cast<const char*>(&length), sizeof(length));
            if (length > 0)
            {
                out.write(s.data(), length);
            }
        }

        std::string ReadCacheString(std::ifstream& in)
        {
            uint32_t length = 0;
            in.read(reinterpret_cast<char*>(&length), sizeof(length));
            std::string s;
            if (length > 0)
            {
                s.resize(length);
                in.read(s.data(), static_cast<std::streamsize>(length));
            }
            return s;
        }

        // メッシュをマテリアル(3枚のテクスチャの組み合わせ)単位でまとめておく。
        // assimpのシーングラフ巡回順のままだと同じマテリアルのメッシュが離れて並びがちで、
        // DX12バックエンドの「直前の描画と同じテクスチャならSRVテーブルを使い回す」最適化
        // (DX12CommandList::FlushPendingSrvWrites)がヒットしにくいため、読み込み後にソートしておく
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
                    return less(a.MetallicRoughnessTexture, b.MetallicRoughnessTexture);
                });
        }

        // キャッシュから読み込む。ソースファイルの更新日時/サイズが一致しない、
        // またはキャッシュが存在しない/読み込み中に壊れていることが分かった場合はfalseを返し、
        // 呼び出し側はassimp経由の通常の読み込みにフォールバックする
        bool TryLoadModelFromCache(RHI::IRHIDevice& device, const std::wstring& filePath, Model& outModel)
        {
            uint64_t sourceTime = 0;
            uint64_t sourceSize = 0;
            if (!GetFileTimeAndSize(filePath, sourceTime, sourceSize))
            {
                return false;
            }

            // 既定のstreambufバッファ(通常数百バイト~数KB)のままだと、Bistro級(100MB超)の
            // キャッシュを細切れのreadで読むことになりオーバーヘッドが無視できないため、
            // openより前に大きめ(1MB)のバッファを設定しておく。ioBufferはinより先に構築し
            // (=inより後に破棄され)、in使用中は常に有効な状態を保つ
            std::vector<char> ioBuffer(1 << 20);
            std::ifstream in;
            in.rdbuf()->pubsetbuf(ioBuffer.data(), static_cast<std::streamsize>(ioBuffer.size()));
            in.open(GetCachePath(filePath), std::ios::binary);
            if (!in.is_open())
            {
                return false;
            }

            const auto startTime = std::chrono::steady_clock::now();

            try
            {
                in.exceptions(std::ios::failbit | std::ios::badbit);

                CacheHeader header{};
                in.read(reinterpret_cast<char*>(&header), sizeof(header));
                if (std::memcmp(header.Magic, kCacheMagic, sizeof(kCacheMagic)) != 0 ||
                    header.Version != kCacheVersion ||
                    header.SourceFileTime != sourceTime ||
                    header.SourceFileSize != sourceSize)
                {
                    return false;
                }

                // MeshCount==0はヘッダーだけ書かれたプレースホルダー(LoadModelが書き込み中に中断された
                // 場合に残る状態。詳細はLoadModel側のGetCacheTempPathのコメント参照)である可能性が高く、
                // 有効なモデルでは通常あり得ないため、壊れたキャッシュとして扱いassimp経由のフォールバックへ回す
                if (header.MeshCount == 0)
                {
                    return false;
                }

                Model model;
                model.BoundsMin[0] = header.BoundsMin[0];
                model.BoundsMin[1] = header.BoundsMin[1];
                model.BoundsMin[2] = header.BoundsMin[2];
                model.BoundsMax[0] = header.BoundsMax[0];
                model.BoundsMax[1] = header.BoundsMax[1];
                model.BoundsMax[2] = header.BoundsMax[2];

                TextureLoader textureLoader(device, GetDirectory(filePath), model);
                model.Meshes.reserve(header.MeshCount);

                // 1周目: メッシュデータ・テクスチャパス文字列をすべてメモリへ読み切る(GPUバッファ作成・
                // テクスチャ読み込みはまだ行わない)。これにより、テクスチャパスが出揃った時点で
                // Prefetch()にまとめて渡し、並列デコードしてからバッファ作成へ進める
                struct CachedMeshRecord
                {
                    std::vector<Vertex> Vertices;
                    std::vector<uint32_t> Indices;
                    float MetallicFactor = 0.0f;
                    float RoughnessFactor = 0.7f;
                    float AlphaCutoff = 0.0f;
                    float EmissiveFactor[3] = { 0.0f, 0.0f, 0.0f };
                    std::string BaseColorPath;
                    std::string NormalPath;
                    std::string MetallicRoughnessPath;
                    std::string EmissivePath;
                };

                std::vector<CachedMeshRecord> records(header.MeshCount);
                for (CachedMeshRecord& record : records)
                {
                    uint32_t vertexCount = 0;
                    in.read(reinterpret_cast<char*>(&vertexCount), sizeof(vertexCount));
                    record.Vertices.resize(vertexCount);
                    if (vertexCount > 0)
                    {
                        in.read(reinterpret_cast<char*>(record.Vertices.data()), static_cast<std::streamsize>(vertexCount * sizeof(Vertex)));
                    }

                    uint32_t indexCount = 0;
                    in.read(reinterpret_cast<char*>(&indexCount), sizeof(indexCount));
                    record.Indices.resize(indexCount);
                    if (indexCount > 0)
                    {
                        in.read(reinterpret_cast<char*>(record.Indices.data()), static_cast<std::streamsize>(indexCount * sizeof(uint32_t)));
                    }

                    in.read(reinterpret_cast<char*>(&record.MetallicFactor), sizeof(record.MetallicFactor));
                    in.read(reinterpret_cast<char*>(&record.RoughnessFactor), sizeof(record.RoughnessFactor));
                    in.read(reinterpret_cast<char*>(&record.AlphaCutoff), sizeof(record.AlphaCutoff));
                    in.read(reinterpret_cast<char*>(record.EmissiveFactor), sizeof(record.EmissiveFactor));

                    record.BaseColorPath = ReadCacheString(in);
                    record.NormalPath = ReadCacheString(in);
                    record.MetallicRoughnessPath = ReadCacheString(in);
                    record.EmissivePath = ReadCacheString(in);
                }

                // ライトはファイル末尾(全メッシュレコードの後)に書かれている。テクスチャの
                // 読み込みを伴わないためPrefetchより前にここで読み切ってしまってよい
                std::vector<Light> lights(header.LightCount);
                for (Light& light : lights)
                {
                    CachedLightRecord record{};
                    in.read(reinterpret_cast<char*>(&record), sizeof(record));
                    light.Type = static_cast<LightType>(record.Type);
                    light.Position[0] = record.Position[0];
                    light.Position[1] = record.Position[1];
                    light.Position[2] = record.Position[2];
                    light.Direction[0] = record.Direction[0];
                    light.Direction[1] = record.Direction[1];
                    light.Direction[2] = record.Direction[2];
                    light.Color[0] = record.Color[0];
                    light.Color[1] = record.Color[1];
                    light.Color[2] = record.Color[2];
                    light.Intensity = record.Intensity;
                    light.Range = record.Range;
                    light.SpotInnerConeAngle = record.SpotInnerConeAngle;
                    light.SpotOuterConeAngle = record.SpotOuterConeAngle;
                    light.Enabled = record.Enabled != 0;
                    light.Name = ReadCacheString(in);
                }

                const auto geometryReadTime = std::chrono::steady_clock::now();

                std::vector<TextureLoader::PrefetchRequest> prefetchRequests;
                prefetchRequests.reserve(records.size() * 4);
                for (const CachedMeshRecord& record : records)
                {
                    if (!record.BaseColorPath.empty())
                    {
                        prefetchRequests.push_back({ record.BaseColorPath, true, false });
                    }
                    if (!record.NormalPath.empty())
                    {
                        prefetchRequests.push_back({ record.NormalPath, false, true });
                    }
                    if (!record.MetallicRoughnessPath.empty())
                    {
                        prefetchRequests.push_back({ record.MetallicRoughnessPath, false, false });
                    }
                    if (!record.EmissivePath.empty())
                    {
                        prefetchRequests.push_back({ record.EmissivePath, true, false });
                    }
                }
                textureLoader.Prefetch(prefetchRequests);

                const auto textureLoadTime = std::chrono::steady_clock::now();

                // 2周目: GPUバッファを作成し、テクスチャはPrefetch済みのキャッシュヒットで即座に解決する
                for (CachedMeshRecord& record : records)
                {
                    Mesh outMesh;

                    RHI::BufferDesc vertexBufferDesc;
                    vertexBufferDesc.Usage = RHI::BufferUsage::Vertex;
                    vertexBufferDesc.SizeInBytes = static_cast<uint32_t>(record.Vertices.size() * sizeof(Vertex));
                    vertexBufferDesc.StrideInBytes = sizeof(Vertex);
                    vertexBufferDesc.InitialData = record.Vertices.data();
                    outMesh.VertexBuffer = device.CreateBuffer(vertexBufferDesc);

                    RHI::BufferDesc indexBufferDesc;
                    indexBufferDesc.Usage = RHI::BufferUsage::Index;
                    indexBufferDesc.SizeInBytes = static_cast<uint32_t>(record.Indices.size() * sizeof(uint32_t));
                    indexBufferDesc.StrideInBytes = sizeof(uint32_t);
                    indexBufferDesc.InitialData = record.Indices.data();
                    outMesh.IndexBuffer = device.CreateBuffer(indexBufferDesc);
                    outMesh.IndexCount = static_cast<uint32_t>(record.Indices.size());

                    outMesh.BaseColorTexture = record.BaseColorPath.empty() ? textureLoader.GetWhite() : textureLoader.Load(record.BaseColorPath, true);
                    outMesh.NormalTexture = record.NormalPath.empty() ? textureLoader.GetFlatNormal() : textureLoader.LoadNormal(record.NormalPath);
                    outMesh.MetallicRoughnessTexture = record.MetallicRoughnessPath.empty() ? textureLoader.GetWhite() : textureLoader.Load(record.MetallicRoughnessPath, false);
                    // EmissiveFactorが0の場合はテクスチャの有無によらず結果は黒になるため、
                    // BaseColor等と同様に白のプレースホルダーへフォールバックしてよい
                    outMesh.EmissiveTexture = record.EmissivePath.empty() ? textureLoader.GetWhite() : textureLoader.Load(record.EmissivePath, true);
                    outMesh.MetallicFactor = record.MetallicFactor;
                    outMesh.RoughnessFactor = record.RoughnessFactor;
                    outMesh.AlphaCutoff = record.AlphaCutoff;
                    outMesh.EmissiveFactor[0] = record.EmissiveFactor[0];
                    outMesh.EmissiveFactor[1] = record.EmissiveFactor[1];
                    outMesh.EmissiveFactor[2] = record.EmissiveFactor[2];

                    model.Meshes.push_back(std::move(outMesh));
                }

                model.Lights = std::move(lights);

                SortMeshesByMaterial(model);
                outModel = std::move(model);

                const auto endTime = std::chrono::steady_clock::now();
                Core::Logger::Info(
                    "ModelLoader",
                    "モデル読み込み完了(キャッシュ): " + WideToUtf8(filePath) +
                    " (ジオメトリ " + FormatMs(startTime, geometryReadTime) + "ms" +
                    " / テクスチャ " + FormatMs(geometryReadTime, textureLoadTime) + "ms" +
                    " / 合計 " + FormatMs(startTime, endTime) + "ms" +
                    ", テクスチャ要求 " + std::to_string(prefetchRequests.size()) + "件" +
                    ", ライト " + std::to_string(header.LightCount) + "灯)");

                return true;
            }
            catch (const std::exception& e)
            {
                // キャッシュが壊れている/バージョン不一致などの場合は通常経路にフォールバックする
                Core::Logger::Warning("ModelLoader", "モデルキャッシュの読み込みに失敗したため、通常経路にフォールバックします: " + std::string(e.what()));
                return false;
            }
        }
    }

    Model LoadModel(RHI::IRHIDevice& device, const std::wstring& filePath)
    {
        Model cachedModel;
        if (TryLoadModelFromCache(device, filePath, cachedModel))
        {
            return cachedModel;
        }

        const auto startTime = std::chrono::steady_clock::now();

        Assimp::Importer importer;
        // GenSmoothNormalsは対象アセットは全メッシュが法線を持つため実質ノーオップであり、
        // 万一法線を持たないメッシュがあった場合のみ後段のフォールバック(上向き固定法線)が使われる。
        // 接線はassimpのaiProcess_CalcTangentSpaceに任せず自前で計算する(下記の頂点ループ内、
        // TangentAccumKey関連のコード参照)。CalcTangentSpaceはUV面積がほぼ0(縮退)の三角形で
        // 接線が数値的に不安定になり、位置・法線・UVが完全に同一の頂点間でさえ接線がほぼ正反対に
        // なることがある(Bistroのグラスメッシュで実際に確認済み)。JoinIdenticalVerticesは
        // 以前は「頂点バッファがやや冗長になるだけ」という理由で付けていなかったが、重複頂点を
        // 減らせるため付けておく(自前の接線平均化は位置+法線をキーにしており重複頂点の有無に
        // 依存しないため、平均化の正しさ自体には影響しない)
        const aiScene* scene = importer.ReadFile(
            WideToUtf8(filePath),
            aiProcess_Triangulate | aiProcess_ConvertToLeftHanded | aiProcess_JoinIdenticalVertices);

        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
        {
            throw std::runtime_error(std::string("モデルの読み込みに失敗しました: ") + importer.GetErrorString());
        }

        const std::wstring directory = GetDirectory(filePath);

        Model model;
        TextureLoader textureLoader(device, directory, model);

        std::vector<std::pair<const aiMesh*, aiMatrix4x4>> meshNodes;
        CollectMeshNodes(scene, scene->mRootNode, aiMatrix4x4(), meshNodes);

        // 現行アセット(Sponza/Bistro/MaterialTest)はライト0件のため、ここでガードすることで
        // 既存の読み込み時間に一切影響しない
        if (scene->mNumLights > 0)
        {
            ImportLights(scene, filePath, model.Lights);
        }

        bool boundsInitialized = false;

        // マテリアルインデックスごとに頂点・インデックスを結合してから1つのバッファ/ドローコールに
        // まとめる。OBJ形式のように同一マテリアルの三角形群が(usemtlの切り替えのたびに)大量の
        // 小さなaiMeshへ分割されているアセットでは、aiMeshごとに個別のバッファ/ドローコールを
        // 発行すると数万件規模になり、GPU側のドライバウォッチドッグ(TDR)によるハングを
        // 引き起こしうる(実際にBistroのMcGuire版OBJ配布(usemtl切り替え22,396回、実質132
        // マテリアル)で確認済み)ため、マテリアル単位でまとめてドローコール数を実質マテリアル数まで
        // 削減する
        struct MergedMeshAccumulator
        {
            std::vector<Vertex> Vertices;
            std::vector<uint32_t> Indices;
        };
        std::unordered_map<unsigned int, MergedMeshAccumulator> meshesByMaterial;

        // キャッシュ書き込み用ファイル。ヘッダーはbounds/meshCountが確定してから
        // 先頭にシークして書き直すため、まずプレースホルダーを書いておく。
        // 本来のキャッシュパス(GetCachePath)ではなく一時パス(GetCacheTempPath)に書き、
        // 書き込みが最後まで成功した場合にのみ本来のパスへリネームする。処理がここで中断された
        // (アプリの強制終了など)場合でも、中途半端な一時ファイルが残るだけで本来のキャッシュ
        // ファイル(既存のもの、または今回分)には影響しないため、次回以降のTryLoadModelFromCacheが
        // 「ヘッダーだけのプレースホルダー」を正常なキャッシュとして誤読することがない
        uint64_t sourceTime = 0;
        uint64_t sourceSize = 0;
        const bool haveSourceStat = GetFileTimeAndSize(filePath, sourceTime, sourceSize);
        std::ofstream cacheOut;
        bool cacheWritable = false;
        std::wstring cacheTempPath;
        if (haveSourceStat)
        {
            cacheTempPath = GetCacheTempPath(filePath);
            cacheOut.open(cacheTempPath, std::ios::binary | std::ios::trunc);
            cacheWritable = cacheOut.is_open();
        }
        if (cacheWritable)
        {
            CacheHeader placeholder{};
            std::memcpy(placeholder.Magic, kCacheMagic, sizeof(kCacheMagic));
            placeholder.Version = kCacheVersion;
            placeholder.SourceFileTime = sourceTime;
            placeholder.SourceFileSize = sourceSize;
            cacheOut.write(reinterpret_cast<const char*>(&placeholder), sizeof(placeholder));
            cacheWritable = static_cast<bool>(cacheOut);
        }

        for (const auto& [mesh, transform] : meshNodes)
        {
            if (!mesh->HasPositions() || !mesh->HasFaces())
            {
                continue;
            }

            // 法線を位置と同じ行列でそのまま変換すると、回転と非一様スケールが組み合わさった場合に
            // 方向が歪んだり反転したりするため、逆行列の転置(inverse-transpose)を用いる。
            // 特異行列(スケール0など)で逆行列が求まらない場合は、3x3行列をそのまま使う簡易フォールバックとする
            aiMatrix3x3 normalMatrix(transform);
            if (normalMatrix.Determinant() != 0.0f)
            {
                normalMatrix.Inverse();
                normalMatrix.Transpose();
            }

            // 接線は面上の方向ベクトル(位置の差分に相当)なので、法線と異なりinverse-transposeではなく
            // 位置と同じ3x3行列で変換する。非一様スケールで法線との直交性が崩れるため、変換後に
            // 法線に対してGram-Schmidt再直交化を行う
            aiMatrix3x3 tangentMatrix(transform);

            // assimpのCalcTangentSpace(aiProcess_CalcTangentSpace)が返す頂点接線は、UV面積が
            // ほぼ0(縮退)な三角形では不安定になり、位置・法線・UVが完全に同一の頂点間でさえ
            // 接線がほぼ正反対になることがある(実際にBistroのグラスメッシュで確認済み)。
            // そのため頂点接線は使わず、三角形ごとに自前で接線を計算し、UV面積がほぼ0の
            // 三角形は寄与から除外したうえで「位置+法線」をキーに蓄積・平均化する
            std::unordered_map<TangentAccumKey, aiVector3D, TangentAccumKeyHash> tangentAccum;
            std::unordered_map<TangentAccumKey, aiVector3D, TangentAccumKeyHash> bitangentAccum;
            if (mesh->HasTextureCoords(0))
            {
                tangentAccum.reserve(mesh->mNumVertices);
                bitangentAccum.reserve(mesh->mNumVertices);
                for (unsigned int f = 0; f < mesh->mNumFaces; ++f)
                {
                    const aiFace& face = mesh->mFaces[f];
                    if (face.mNumIndices != 3)
                    {
                        continue;
                    }

                    const unsigned int i0 = face.mIndices[0];
                    const unsigned int i1 = face.mIndices[1];
                    const unsigned int i2 = face.mIndices[2];

                    const aiVector3D& p0 = mesh->mVertices[i0];
                    const aiVector3D edge1 = mesh->mVertices[i1] - p0;
                    const aiVector3D edge2 = mesh->mVertices[i2] - p0;

                    const aiVector2D uv0(mesh->mTextureCoords[0][i0].x, mesh->mTextureCoords[0][i0].y);
                    const aiVector2D duv1(mesh->mTextureCoords[0][i1].x - uv0.x, mesh->mTextureCoords[0][i1].y - uv0.y);
                    const aiVector2D duv2(mesh->mTextureCoords[0][i2].x - uv0.x, mesh->mTextureCoords[0][i2].y - uv0.y);

                    const float denom = duv1.x * duv2.y - duv2.x * duv1.y;
                    if (std::abs(denom) < 1e-8f)
                    {
                        // UV空間で面積がほぼ0の三角形(UVフォールディング/縮退)は接線方向が
                        // 数値的に不安定になるため、平均化への寄与から除外する
                        continue;
                    }

                    // 従法線は連立方程式(edge = duv.x*T + duv.y*B)を素直に解いた符号(+V方向)を使う。
                    // (assimpのCalcTangentsProcessに合わせて符号反転させたことがあるが、
                    // 実際のBistroグラスの法線マップでは明暗が改善しなかったため元に戻した)
                    const float r = 1.0f / denom;
                    const aiVector3D faceTangent = (edge1 * duv2.y - edge2 * duv1.y) * r;
                    const aiVector3D faceBitangent = (edge2 * duv1.x - edge1 * duv2.x) * r;
                    if (faceTangent.SquareLength() < 1e-12f)
                    {
                        continue;
                    }

                    for (unsigned int idx : { i0, i1, i2 })
                    {
                        const aiVector3D& p = mesh->mVertices[idx];
                        const aiVector3D localNormal = mesh->HasNormals() ? mesh->mNormals[idx] : aiVector3D(0.0f, 1.0f, 0.0f);
                        const TangentAccumKey key{ p.x, p.y, p.z, localNormal.x, localNormal.y, localNormal.z };
                        tangentAccum[key] += faceTangent;
                        bitangentAccum[key] += faceBitangent;
                    }
                }
            }

            std::vector<Vertex> vertices;
            vertices.reserve(mesh->mNumVertices);
            for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
            {
                aiVector3D position = transform * mesh->mVertices[v];
                aiVector3D normal = mesh->HasNormals() ? (normalMatrix * mesh->mNormals[v]) : aiVector3D(0.0f, 1.0f, 0.0f);
                normal.Normalize();

                aiVector3D tangent(1.0f, 0.0f, 0.0f);
                float tangentSign = 1.0f;
                if (mesh->HasTextureCoords(0))
                {
                    const aiVector3D& localNormalForKey = mesh->HasNormals() ? mesh->mNormals[v] : aiVector3D(0.0f, 1.0f, 0.0f);
                    const TangentAccumKey key{
                        mesh->mVertices[v].x, mesh->mVertices[v].y, mesh->mVertices[v].z,
                        localNormalForKey.x, localNormalForKey.y, localNormalForKey.z };
                    const auto tangentIt = tangentAccum.find(key);
                    const auto bitangentIt = bitangentAccum.find(key);
                    const bool hasAccum = tangentIt != tangentAccum.end() && tangentIt->second.SquareLength() > 1e-12f;

                    aiVector3D rawTangent = hasAccum ? (tangentMatrix * tangentIt->second) : aiVector3D(0.0f, 0.0f, 0.0f);

                    tangent = rawTangent - normal * (normal * rawTangent);
                    if (hasAccum && tangent.SquareLength() > 1e-12f)
                    {
                        tangent.Normalize();
                        const aiVector3D rawBitangent = bitangentIt != bitangentAccum.end()
                            ? (tangentMatrix * bitangentIt->second)
                            : (normal ^ tangent);
                        tangentSign = ((normal ^ tangent) * rawBitangent) < 0.0f ? -1.0f : 1.0f;
                    }
                    else
                    {
                        // 全ての隣接三角形がUV縮退などで接線寄与を持たない場合、法線に直交する適当な軸で代用する
                        aiVector3D fallbackAxis = std::abs(normal.y) < 0.99f ? aiVector3D(0.0f, 1.0f, 0.0f) : aiVector3D(1.0f, 0.0f, 0.0f);
                        tangent = (fallbackAxis ^ normal);
                        tangent.Normalize();
                        tangentSign = 1.0f;
                    }
                }

                Vertex vertex{};
                vertex.Position[0] = position.x;
                vertex.Position[1] = position.y;
                vertex.Position[2] = position.z;
                vertex.Normal[0] = normal.x;
                vertex.Normal[1] = normal.y;
                vertex.Normal[2] = normal.z;
                vertex.Tangent[0] = tangent.x;
                vertex.Tangent[1] = tangent.y;
                vertex.Tangent[2] = tangent.z;
                vertex.Tangent[3] = tangentSign;
                if (mesh->HasTextureCoords(0))
                {
                    vertex.UV[0] = mesh->mTextureCoords[0][v].x;
                    vertex.UV[1] = mesh->mTextureCoords[0][v].y;
                }
                else
                {
                    vertex.UV[0] = 0.0f;
                    vertex.UV[1] = 0.0f;
                }
                vertices.push_back(vertex);

                if (!boundsInitialized)
                {
                    model.BoundsMin[0] = model.BoundsMax[0] = position.x;
                    model.BoundsMin[1] = model.BoundsMax[1] = position.y;
                    model.BoundsMin[2] = model.BoundsMax[2] = position.z;
                    boundsInitialized = true;
                }
                else
                {
                    model.BoundsMin[0] = std::min(model.BoundsMin[0], position.x);
                    model.BoundsMin[1] = std::min(model.BoundsMin[1], position.y);
                    model.BoundsMin[2] = std::min(model.BoundsMin[2], position.z);
                    model.BoundsMax[0] = std::max(model.BoundsMax[0], position.x);
                    model.BoundsMax[1] = std::max(model.BoundsMax[1], position.y);
                    model.BoundsMax[2] = std::max(model.BoundsMax[2], position.z);
                }
            }

            std::vector<uint32_t> indices;
            indices.reserve(static_cast<size_t>(mesh->mNumFaces) * 3);
            for (unsigned int f = 0; f < mesh->mNumFaces; ++f)
            {
                const aiFace& face = mesh->mFaces[f];
                for (unsigned int idx = 0; idx < face.mNumIndices; ++idx)
                {
                    indices.push_back(face.mIndices[idx]);
                }
            }

            // GPU側のバッファ作成・テクスチャ読み込みはここでは行わず、マテリアルインデックスごとに
            // 頂点・インデックスを結合するだけにとどめる(実際のバッファ作成は全aiMeshを走査し
            // 終えた後、マテリアルごとに1回だけ行う。下記参照)
            MergedMeshAccumulator& accum = meshesByMaterial[mesh->mMaterialIndex];
            const uint32_t indexBase = static_cast<uint32_t>(accum.Vertices.size());
            accum.Vertices.insert(accum.Vertices.end(), vertices.begin(), vertices.end());
            accum.Indices.reserve(accum.Indices.size() + indices.size());
            for (uint32_t idx : indices)
            {
                accum.Indices.push_back(indexBase + idx);
            }
        }

        const auto geometryTime = std::chrono::steady_clock::now();

        // マテリアルインデックスの昇順(assimpのマテリアル配列順)に処理することで、同じソースから
        // 生成したキャッシュのメッシュ順が実行のたびに変わらないようにする。
        // 1周目: テクスチャパスの解決だけを行い、Prefetch()にまとめて渡して並列デコードさせる
        // (GPUバッファ作成やLoad/LoadNormal呼び出しはまだ行わない)
        struct MaterialTexturePaths
        {
            std::string BaseColorPath;
            std::string NormalPath;
            std::string MetallicRoughnessPath;
            std::string EmissivePath;
        };
        std::unordered_map<unsigned int, MaterialTexturePaths> materialTexturePaths;
        std::vector<TextureLoader::PrefetchRequest> prefetchRequests;

        for (unsigned int materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex)
        {
            const auto accumIt = meshesByMaterial.find(materialIndex);
            if (accumIt == meshesByMaterial.end() || accumIt->second.Indices.empty())
            {
                continue;
            }

            const aiMaterial* material = scene->mMaterials[materialIndex];
            aiString texPath;
            MaterialTexturePaths paths;

            if (material->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath) == AI_SUCCESS ||
                material->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS)
            {
                paths.BaseColorPath = texPath.C_Str();
                prefetchRequests.push_back({ paths.BaseColorPath, true, false });
            }

            // OBJ形式は法線マップをaiTextureType_NORMALSではなくmap_bump(aiTextureType_HEIGHT)として
            // 格納する慣習があるため、NORMALSが無い場合はHEIGHTにもフォールバックする
            if (material->GetTexture(aiTextureType_NORMALS, 0, &texPath) == AI_SUCCESS ||
                material->GetTexture(aiTextureType_HEIGHT, 0, &texPath) == AI_SUCCESS)
            {
                paths.NormalPath = texPath.C_Str();
                prefetchRequests.push_back({ paths.NormalPath, false, true });
            }

            // glTFのmetallicRoughnessテクスチャはG=ラフネス、B=メタリックを1枚に格納しており、
            // assimpはこれをROUGHNESS/METALNESSの両方のテクスチャタイプとして同じ画像を指す
            if (material->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &texPath) == AI_SUCCESS ||
                material->GetTexture(aiTextureType_METALNESS, 0, &texPath) == AI_SUCCESS)
            {
                paths.MetallicRoughnessPath = texPath.C_Str();
                prefetchRequests.push_back({ paths.MetallicRoughnessPath, false, false });
            }

            if (material->GetTexture(aiTextureType_EMISSIVE, 0, &texPath) == AI_SUCCESS)
            {
                paths.EmissivePath = texPath.C_Str();
                prefetchRequests.push_back({ paths.EmissivePath, true, false });
            }

            materialTexturePaths.emplace(materialIndex, std::move(paths));
        }

        textureLoader.Prefetch(prefetchRequests);

        const auto textureTime = std::chrono::steady_clock::now();

        // 2周目: GPUバッファを作成し、テクスチャはPrefetch済みのキャッシュヒットで即座に解決する
        for (unsigned int materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex)
        {
            const auto accumIt = meshesByMaterial.find(materialIndex);
            if (accumIt == meshesByMaterial.end() || accumIt->second.Indices.empty())
            {
                continue;
            }
            const std::vector<Vertex>& vertices = accumIt->second.Vertices;
            const std::vector<uint32_t>& indices = accumIt->second.Indices;
            const MaterialTexturePaths& paths = materialTexturePaths.at(materialIndex);

            Mesh outMesh;

            RHI::BufferDesc vertexBufferDesc;
            vertexBufferDesc.Usage = RHI::BufferUsage::Vertex;
            vertexBufferDesc.SizeInBytes = static_cast<uint32_t>(vertices.size() * sizeof(Vertex));
            vertexBufferDesc.StrideInBytes = sizeof(Vertex);
            vertexBufferDesc.InitialData = vertices.data();
            outMesh.VertexBuffer = device.CreateBuffer(vertexBufferDesc);

            RHI::BufferDesc indexBufferDesc;
            indexBufferDesc.Usage = RHI::BufferUsage::Index;
            indexBufferDesc.SizeInBytes = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));
            indexBufferDesc.StrideInBytes = sizeof(uint32_t);
            indexBufferDesc.InitialData = indices.data();
            outMesh.IndexBuffer = device.CreateBuffer(indexBufferDesc);
            outMesh.IndexCount = static_cast<uint32_t>(indices.size());

            const aiMaterial* material = scene->mMaterials[materialIndex];

            outMesh.BaseColorTexture = paths.BaseColorPath.empty() ? textureLoader.GetWhite() : textureLoader.Load(paths.BaseColorPath, true);
            outMesh.NormalTexture = paths.NormalPath.empty() ? textureLoader.GetFlatNormal() : textureLoader.LoadNormal(paths.NormalPath);
            outMesh.MetallicRoughnessTexture = paths.MetallicRoughnessPath.empty() ? textureLoader.GetWhite() : textureLoader.Load(paths.MetallicRoughnessPath, false);
            // EmissiveFactorが0の場合はテクスチャの有無によらず結果は黒になるため、
            // BaseColor等と同様に白のプレースホルダーへフォールバックしてよい
            outMesh.EmissiveTexture = paths.EmissivePath.empty() ? textureLoader.GetWhite() : textureLoader.Load(paths.EmissivePath, true);

            // FBXなどPBRメタリック/ラフネスの係数を持たない形式では既定値(非金属・やや粗め)のままになる
            material->Get(AI_MATKEY_METALLIC_FACTOR, outMesh.MetallicFactor);
            material->Get(AI_MATKEY_ROUGHNESS_FACTOR, outMesh.RoughnessFactor);
            // FBXの古いPhong系マテリアル(Shininessのみ持つ)をassimpがPBRラフネスへ変換する際、
            // 変換式が破綻して[0,1]範囲外の値(Bistroのガラス系マテリアルで実測: -2.2)を返すことがある。
            // シェーダー側でclampされて常に最小ラフネス(ほぼ鏡面)に張り付き、SSRの反射が
            // 単一サンプルで粗くなって不自然に破綻して見えるため、範囲外の値は既定値にフォールバックする
            if (!(outMesh.RoughnessFactor >= 0.0f && outMesh.RoughnessFactor <= 1.0f))
            {
                outMesh.RoughnessFactor = 0.7f;
            }

            aiColor3D emissiveColor(0.0f, 0.0f, 0.0f);
            material->Get(AI_MATKEY_COLOR_EMISSIVE, emissiveColor);
            outMesh.EmissiveFactor[0] = emissiveColor.r;
            outMesh.EmissiveFactor[1] = emissiveColor.g;
            outMesh.EmissiveFactor[2] = emissiveColor.b;

            // アルファカットアウトはglTFのalphaMode拡張情報でのみ判定する(FBX/OBJ等には概念自体がない)。
            // MASK以外(既定のOPAQUE、または半透明のBLEND)ではAlphaCutoff=0のままにし、
            // GBuffer.hlsl側のclip()を発火させない(BLENDの半透明合成はDeferredでは別途対応が必要なため、
            // このエンジンでは現状不透明として扱う)
            aiString alphaMode;
            float alphaCutoff = 0.5f;
            material->Get(AI_MATKEY_GLTF_ALPHACUTOFF, alphaCutoff);
            outMesh.AlphaCutoff =
                (material->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS && std::strcmp(alphaMode.C_Str(), "MASK") == 0)
                ? alphaCutoff
                : 0.0f;

            if (cacheWritable)
            {
                const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
                const uint32_t indexCount = static_cast<uint32_t>(indices.size());
                cacheOut.write(reinterpret_cast<const char*>(&vertexCount), sizeof(vertexCount));
                cacheOut.write(reinterpret_cast<const char*>(vertices.data()), static_cast<std::streamsize>(vertices.size() * sizeof(Vertex)));
                cacheOut.write(reinterpret_cast<const char*>(&indexCount), sizeof(indexCount));
                cacheOut.write(reinterpret_cast<const char*>(indices.data()), static_cast<std::streamsize>(indices.size() * sizeof(uint32_t)));
                cacheOut.write(reinterpret_cast<const char*>(&outMesh.MetallicFactor), sizeof(float));
                cacheOut.write(reinterpret_cast<const char*>(&outMesh.RoughnessFactor), sizeof(float));
                cacheOut.write(reinterpret_cast<const char*>(&outMesh.AlphaCutoff), sizeof(float));
                cacheOut.write(reinterpret_cast<const char*>(outMesh.EmissiveFactor), sizeof(outMesh.EmissiveFactor));
                WriteCacheString(cacheOut, paths.BaseColorPath);
                WriteCacheString(cacheOut, paths.NormalPath);
                WriteCacheString(cacheOut, paths.MetallicRoughnessPath);
                WriteCacheString(cacheOut, paths.EmissivePath);
                cacheWritable = static_cast<bool>(cacheOut);
            }

            model.Meshes.push_back(std::move(outMesh));
        }

        // ライトはファイル末尾(全メッシュレコードの後)に追記する。こうすればメッシュ読み込み/
        // 書き出しループには一切手を入れずに済む
        if (cacheWritable)
        {
            for (const Light& light : model.Lights)
            {
                CachedLightRecord record{};
                record.Type = static_cast<uint32_t>(light.Type);
                record.Position[0] = light.Position[0];
                record.Position[1] = light.Position[1];
                record.Position[2] = light.Position[2];
                record.Direction[0] = light.Direction[0];
                record.Direction[1] = light.Direction[1];
                record.Direction[2] = light.Direction[2];
                record.Color[0] = light.Color[0];
                record.Color[1] = light.Color[1];
                record.Color[2] = light.Color[2];
                record.Intensity = light.Intensity;
                record.Range = light.Range;
                record.SpotInnerConeAngle = light.SpotInnerConeAngle;
                record.SpotOuterConeAngle = light.SpotOuterConeAngle;
                record.Enabled = light.Enabled ? 1u : 0u;
                cacheOut.write(reinterpret_cast<const char*>(&record), sizeof(record));
                WriteCacheString(cacheOut, light.Name);
                cacheWritable = static_cast<bool>(cacheOut);
            }
        }

        if (cacheWritable)
        {
            CacheHeader header{};
            std::memcpy(header.Magic, kCacheMagic, sizeof(kCacheMagic));
            header.Version = kCacheVersion;
            header.SourceFileTime = sourceTime;
            header.SourceFileSize = sourceSize;
            header.BoundsMin[0] = model.BoundsMin[0];
            header.BoundsMin[1] = model.BoundsMin[1];
            header.BoundsMin[2] = model.BoundsMin[2];
            header.BoundsMax[0] = model.BoundsMax[0];
            header.BoundsMax[1] = model.BoundsMax[1];
            header.BoundsMax[2] = model.BoundsMax[2];
            header.MeshCount = static_cast<uint32_t>(model.Meshes.size());
            header.LightCount = static_cast<uint32_t>(model.Lights.size());
            cacheOut.seekp(0);
            cacheOut.write(reinterpret_cast<const char*>(&header), sizeof(header));
            cacheWritable = static_cast<bool>(cacheOut);
            cacheOut.close();

            // 一時ファイルへの書き込みが最後まで成功した場合にのみ、本来のキャッシュパスへ差し替える
            // (同一ボリューム上でのリネームなので、この置き換え自体は事実上原子的)。MeshCount==0
            // (アセット側に有効なメッシュが1つもなかった場合)はTryLoadModelFromCache側でも常に
            // 拒否されるだけなので書き出さない
            if (cacheWritable && header.MeshCount > 0)
            {
                MoveFileExW(cacheTempPath.c_str(), GetCachePath(filePath).c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        }

        // キャッシュファイルへはシーングラフ巡回順のまま書き出し済みのため、ソートは
        // メモリ上のモデルに対してのみ行う(キャッシュから読み込む側はTryLoadModelFromCache側で行う)
        SortMeshesByMaterial(model);

        const auto endTime = std::chrono::steady_clock::now();
        Core::Logger::Info(
            "ModelLoader",
            "モデル読み込み完了: " + WideToUtf8(filePath) +
            " (ジオメトリ " + FormatMs(startTime, geometryTime) + "ms" +
            " / テクスチャ " + FormatMs(geometryTime, textureTime) + "ms" +
            " / 合計 " + FormatMs(startTime, endTime) + "ms" +
            ", テクスチャ要求 " + std::to_string(prefetchRequests.size()) + "件" +
            ", ライト " + std::to_string(model.Lights.size()) + "灯)");

        return model;
    }
}
