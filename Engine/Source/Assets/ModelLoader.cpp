#include "ModelLoader.h"

#include <Windows.h>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
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
    }

    Model LoadModel(RHI::IRHIDevice& device, const std::wstring& filePath)
    {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(
            WideToUtf8(filePath),
            aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices | aiProcess_ConvertToLeftHanded);

        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
        {
            throw std::runtime_error(std::string("モデルの読み込みに失敗しました: ") + importer.GetErrorString());
        }

        const std::wstring directory = GetDirectory(filePath);

        Model model;
        std::unordered_map<std::string, RHI::IRHITexture*> textureCache;
        RHI::IRHITexture* whiteTexture = nullptr;

        auto loadTexture = [&](const aiString& texPath, bool sRGB) -> RHI::IRHITexture*
        {
            // sRGB指定違いで同じパスを再利用することは想定していないため、キーはパス文字列のみで良い
            const std::string key = texPath.C_Str();
            auto it = textureCache.find(key);
            if (it != textureCache.end())
            {
                return it->second;
            }

            RHI::IRHITexture* rawPtr = nullptr;
            try
            {
                const std::wstring fullPath = ResolveTexturePath(directory, Utf8ToWide(key));
                auto texture = device.CreateTextureFromFile(fullPath, sRGB);
                rawPtr = texture.get();
                model.Textures.push_back(std::move(texture));
            }
            catch (const std::exception&)
            {
                // 読み込みに失敗したテクスチャは目立つ色のプレースホルダーで代替し、モデル全体の読み込みは継続する
                auto texture = device.CreateSolidColorTexture(255, 0, 255, 255);
                rawPtr = texture.get();
                model.Textures.push_back(std::move(texture));
            }

            textureCache.emplace(key, rawPtr);
            return rawPtr;
        };

        auto getWhiteTexture = [&]() -> RHI::IRHITexture*
        {
            if (!whiteTexture)
            {
                auto texture = device.CreateSolidColorTexture(255, 255, 255, 255);
                whiteTexture = texture.get();
                model.Textures.push_back(std::move(texture));
            }
            return whiteTexture;
        };

        RHI::IRHITexture* flatNormalTexture = nullptr;
        auto getFlatNormalTexture = [&]() -> RHI::IRHITexture*
        {
            if (!flatNormalTexture)
            {
                // タンジェント空間で(0,0,1)、すなわち「法線マップなし」を表す色
                auto texture = device.CreateSolidColorTexture(128, 128, 255, 255);
                flatNormalTexture = texture.get();
                model.Textures.push_back(std::move(texture));
            }
            return flatNormalTexture;
        };

        std::vector<std::pair<const aiMesh*, aiMatrix4x4>> meshNodes;
        CollectMeshNodes(scene, scene->mRootNode, aiMatrix4x4(), meshNodes);

        bool boundsInitialized = false;

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
            if (material->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath) == AI_SUCCESS ||
                material->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS)
            {
                outMesh.BaseColorTexture = loadTexture(texPath, true);
            }
            else
            {
                outMesh.BaseColorTexture = getWhiteTexture();
            }

            if (material->GetTexture(aiTextureType_NORMALS, 0, &texPath) == AI_SUCCESS)
            {
                outMesh.NormalTexture = loadTexture(texPath, false);
            }
            else
            {
                outMesh.NormalTexture = getFlatNormalTexture();
            }

            // glTFのmetallicRoughnessテクスチャはG=ラフネス、B=メタリックを1枚に格納しており、
            // assimpはこれをROUGHNESS/METALNESSの両方のテクスチャタイプとして同じ画像を指す
            if (material->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &texPath) == AI_SUCCESS ||
                material->GetTexture(aiTextureType_METALNESS, 0, &texPath) == AI_SUCCESS)
            {
                outMesh.MetallicRoughnessTexture = loadTexture(texPath, false);
            }
            else
            {
                outMesh.MetallicRoughnessTexture = getWhiteTexture();
            }

            // FBXなどPBRメタリック/ラフネスの係数を持たない形式では既定値(非金属・やや粗め)のままになる
            material->Get(AI_MATKEY_METALLIC_FACTOR, outMesh.MetallicFactor);
            material->Get(AI_MATKEY_ROUGHNESS_FACTOR, outMesh.RoughnessFactor);

            model.Meshes.push_back(std::move(outMesh));
        }

        return model;
    }
}
