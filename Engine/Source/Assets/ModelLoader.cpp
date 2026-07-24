#include "ModelLoader.h"

#include <Windows.h>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

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
                catch (const std::exception&)
                {
                    // 読み込みに失敗したテクスチャは目立つ色のプレースホルダーで代替し、モデル全体の読み込みは継続する
                    auto texture = m_Device.CreateSolidColorTexture(255, 0, 255, 255);
                    rawPtr = texture.get();
                    m_Model.Textures.push_back(std::move(texture));
                }

                m_Cache.emplace(path, rawPtr);
                return rawPtr;
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
        constexpr uint32_t kCacheVersion = 1;

        struct CacheHeader
        {
            char Magic[4];
            uint32_t Version;
            uint64_t SourceFileTime;
            uint64_t SourceFileSize;
            float BoundsMin[3];
            float BoundsMax[3];
            uint32_t MeshCount;
        };

        std::wstring GetCachePath(const std::wstring& filePath)
        {
            return filePath + L".kmodelcache";
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

            std::ifstream in(GetCachePath(filePath), std::ios::binary);
            if (!in.is_open())
            {
                return false;
            }

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

                Model model;
                model.BoundsMin[0] = header.BoundsMin[0];
                model.BoundsMin[1] = header.BoundsMin[1];
                model.BoundsMin[2] = header.BoundsMin[2];
                model.BoundsMax[0] = header.BoundsMax[0];
                model.BoundsMax[1] = header.BoundsMax[1];
                model.BoundsMax[2] = header.BoundsMax[2];

                TextureLoader textureLoader(device, GetDirectory(filePath), model);
                model.Meshes.reserve(header.MeshCount);

                for (uint32_t i = 0; i < header.MeshCount; ++i)
                {
                    uint32_t vertexCount = 0;
                    in.read(reinterpret_cast<char*>(&vertexCount), sizeof(vertexCount));
                    std::vector<Vertex> vertices(vertexCount);
                    if (vertexCount > 0)
                    {
                        in.read(reinterpret_cast<char*>(vertices.data()), static_cast<std::streamsize>(vertexCount * sizeof(Vertex)));
                    }

                    uint32_t indexCount = 0;
                    in.read(reinterpret_cast<char*>(&indexCount), sizeof(indexCount));
                    std::vector<uint32_t> indices(indexCount);
                    if (indexCount > 0)
                    {
                        in.read(reinterpret_cast<char*>(indices.data()), static_cast<std::streamsize>(indexCount * sizeof(uint32_t)));
                    }

                    float metallicFactor = 0.0f;
                    float roughnessFactor = 0.7f;
                    in.read(reinterpret_cast<char*>(&metallicFactor), sizeof(metallicFactor));
                    in.read(reinterpret_cast<char*>(&roughnessFactor), sizeof(roughnessFactor));

                    const std::string baseColorPath = ReadCacheString(in);
                    const std::string normalPath = ReadCacheString(in);
                    const std::string metallicRoughnessPath = ReadCacheString(in);

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

                    outMesh.BaseColorTexture = baseColorPath.empty() ? textureLoader.GetWhite() : textureLoader.Load(baseColorPath, true);
                    outMesh.NormalTexture = normalPath.empty() ? textureLoader.GetFlatNormal() : textureLoader.Load(normalPath, false);
                    outMesh.MetallicRoughnessTexture = metallicRoughnessPath.empty() ? textureLoader.GetWhite() : textureLoader.Load(metallicRoughnessPath, false);
                    outMesh.MetallicFactor = metallicFactor;
                    outMesh.RoughnessFactor = roughnessFactor;

                    model.Meshes.push_back(std::move(outMesh));
                }

                SortMeshesByMaterial(model);
                outModel = std::move(model);
                return true;
            }
            catch (const std::exception&)
            {
                // キャッシュが壊れている/バージョン不一致などの場合は通常経路にフォールバックする
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

        Assimp::Importer importer;
        // JoinIdenticalVerticesは大規模メッシュで数十秒単位のロード時間増になる一方、
        // このエンジンでは各メッシュがassimp側で既にインデックス化された状態で読み込まれるため
        // 頂点共有を行わなくても描画結果には影響しない(頂点バッファがやや冗長になるのみ)ので付けない。
        // GenSmoothNormalsも対象アセットは全メッシュが法線を持つため実質ノーオップであり、
        // 万一法線を持たないメッシュがあった場合のみ後段のフォールバック(上向き固定法線)が使われる
        const aiScene* scene = importer.ReadFile(
            WideToUtf8(filePath),
            aiProcess_Triangulate | aiProcess_ConvertToLeftHanded);

        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
        {
            throw std::runtime_error(std::string("モデルの読み込みに失敗しました: ") + importer.GetErrorString());
        }

        const std::wstring directory = GetDirectory(filePath);

        Model model;
        TextureLoader textureLoader(device, directory, model);

        std::vector<std::pair<const aiMesh*, aiMatrix4x4>> meshNodes;
        CollectMeshNodes(scene, scene->mRootNode, aiMatrix4x4(), meshNodes);

        bool boundsInitialized = false;

        // キャッシュ書き込み用ファイル。ヘッダーはbounds/meshCountが確定してから
        // 先頭にシークして書き直すため、まずプレースホルダーを書いておく
        uint64_t sourceTime = 0;
        uint64_t sourceSize = 0;
        const bool haveSourceStat = GetFileTimeAndSize(filePath, sourceTime, sourceSize);
        std::ofstream cacheOut;
        bool cacheWritable = false;
        if (haveSourceStat)
        {
            cacheOut.open(GetCachePath(filePath), std::ios::binary | std::ios::trunc);
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

            std::vector<Vertex> vertices;
            vertices.reserve(mesh->mNumVertices);
            for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
            {
                aiVector3D position = transform * mesh->mVertices[v];
                aiVector3D normal = mesh->HasNormals() ? (normalMatrix * mesh->mNormals[v]) : aiVector3D(0.0f, 1.0f, 0.0f);
                normal.Normalize();

                Vertex vertex{};
                vertex.Position[0] = position.x;
                vertex.Position[1] = position.y;
                vertex.Position[2] = position.z;
                vertex.Normal[0] = normal.x;
                vertex.Normal[1] = normal.y;
                vertex.Normal[2] = normal.z;
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

            const aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
            aiString texPath;

            std::string baseColorPath;
            if (material->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath) == AI_SUCCESS ||
                material->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS)
            {
                baseColorPath = texPath.C_Str();
                outMesh.BaseColorTexture = textureLoader.Load(baseColorPath, true);
            }
            else
            {
                outMesh.BaseColorTexture = textureLoader.GetWhite();
            }

            std::string normalPath;
            if (material->GetTexture(aiTextureType_NORMALS, 0, &texPath) == AI_SUCCESS)
            {
                normalPath = texPath.C_Str();
                outMesh.NormalTexture = textureLoader.Load(normalPath, false);
            }
            else
            {
                outMesh.NormalTexture = textureLoader.GetFlatNormal();
            }

            // glTFのmetallicRoughnessテクスチャはG=ラフネス、B=メタリックを1枚に格納しており、
            // assimpはこれをROUGHNESS/METALNESSの両方のテクスチャタイプとして同じ画像を指す
            std::string metallicRoughnessPath;
            if (material->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &texPath) == AI_SUCCESS ||
                material->GetTexture(aiTextureType_METALNESS, 0, &texPath) == AI_SUCCESS)
            {
                metallicRoughnessPath = texPath.C_Str();
                outMesh.MetallicRoughnessTexture = textureLoader.Load(metallicRoughnessPath, false);
            }
            else
            {
                outMesh.MetallicRoughnessTexture = textureLoader.GetWhite();
            }

            // FBXなどPBRメタリック/ラフネスの係数を持たない形式では既定値(非金属・やや粗め)のままになる
            material->Get(AI_MATKEY_METALLIC_FACTOR, outMesh.MetallicFactor);
            material->Get(AI_MATKEY_ROUGHNESS_FACTOR, outMesh.RoughnessFactor);

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
                WriteCacheString(cacheOut, baseColorPath);
                WriteCacheString(cacheOut, normalPath);
                WriteCacheString(cacheOut, metallicRoughnessPath);
                cacheWritable = static_cast<bool>(cacheOut);
            }

            model.Meshes.push_back(std::move(outMesh));
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
            cacheOut.seekp(0);
            cacheOut.write(reinterpret_cast<const char*>(&header), sizeof(header));
        }

        // キャッシュファイルへはシーングラフ巡回順のまま書き出し済みのため、ソートは
        // メモリ上のモデルに対してのみ行う(キャッシュから読み込む側はTryLoadModelFromCache側で行う)
        SortMeshesByMaterial(model);

        return model;
    }
}
