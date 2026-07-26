#include "ModelSource.h"

#include <Windows.h>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

#include "Core/StringUtil.h"

using Kurenai::Assets::Vertex;
using Kurenai::Core::Utf8ToWide;
using Kurenai::Core::WideToUtf8;

namespace KurenaiPacker
{
    namespace
    {
        // glTFのテクスチャURI(aiMaterial::GetTextureが返すaiString)はRFC 3986のURIであり、
        // ファイル名中の空白などは"%20"のようにパーセントエンコードされている。assimpは
        // このデコードを行わず生のURI文字列をそのまま返すため、デコードせずファイルパスとして
        // 扱うと実ファイル名と一致せず読み込みに失敗する(実際にBistroの
        // "Metal_ RollDoor_01_diff.png"や"Paris_ShopSign_ties shop_diff.png"で確認済み。
        // glTF側は"Metal_%20RollDoor_01_diff.png"のようにエンコードして格納している)。
        // FBX/OBJ等の非URIパスに対しても一律に適用しているが、ファイル名に本物の
        // "%XX"(16進2桁)を含むケースは実用上ほぼ無いため実害はない
        std::string UriDecode(const std::string& uri)
        {
            std::string result;
            result.reserve(uri.size());
            for (size_t i = 0; i < uri.size(); ++i)
            {
                if (uri[i] == '%' && i + 2 < uri.size() &&
                    std::isxdigit(static_cast<unsigned char>(uri[i + 1])) &&
                    std::isxdigit(static_cast<unsigned char>(uri[i + 2])))
                {
                    const std::string hex = uri.substr(i + 1, 2);
                    result.push_back(static_cast<char>(std::stoi(hex, nullptr, 16)));
                    i += 2;
                }
                else
                {
                    result.push_back(uri[i]);
                }
            }
            return result;
        }

        std::wstring GetDirectory(const std::wstring& filePath)
        {
            const size_t pos = filePath.find_last_of(L"/\\");
            return pos == std::wstring::npos ? L"" : filePath.substr(0, pos + 1);
        }

        bool FileExists(const std::wstring& path)
        {
            return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        }

        // FBXは「Textures/xxx.dds」のようなパスやファイル名のみを格納している場合があり、
        // モデルからの相対パスをそのまま連結しただけでは見つからないことがあるため複数候補を試す
        // (KurenaiEngine/Source/Library/Assets/ModelLoader.cppの旧ResolveTexturePathと同じロジック。
        // 従来は実行時に毎回この探索を行っていたが、パック時に一度だけ確定させることで
        // ランタイムからパス探索そのものが消える)
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

        struct MergedMeshAccumulator
        {
            std::vector<Vertex> Vertices;
            std::vector<uint32_t> Indices;
        };
    }

    SourceModel LoadSourceModel(const std::wstring& filePath)
    {
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

        SourceModel model;

        std::vector<std::pair<const aiMesh*, aiMatrix4x4>> meshNodes;
        CollectMeshNodes(scene, scene->mRootNode, aiMatrix4x4(), meshNodes);

        bool boundsInitialized = false;

        // マテリアルインデックスごとに頂点・インデックスを結合してから1つのメッシュにまとめる。
        // OBJ形式のように同一マテリアルの三角形群が(usemtlの切り替えのたびに)大量の
        // 小さなaiMeshへ分割されているアセットでは、aiMeshごとに個別のバッファ/ドローコールを
        // 発行すると数万件規模になり、GPU側のドライバウォッチドッグ(TDR)によるハングを
        // 引き起こしうる(実際にBistroのMcGuire版OBJ配布(usemtl切り替え22,396回、実質132
        // マテリアル)で確認済み)ため、マテリアル単位でまとめてドローコール数を実質マテリアル数まで
        // 削減する
        std::unordered_map<unsigned int, MergedMeshAccumulator> meshesByMaterial;

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

            MergedMeshAccumulator& accum = meshesByMaterial[mesh->mMaterialIndex];
            const uint32_t indexBase = static_cast<uint32_t>(accum.Vertices.size());
            accum.Vertices.insert(accum.Vertices.end(), vertices.begin(), vertices.end());
            accum.Indices.reserve(accum.Indices.size() + indices.size());
            for (uint32_t idx : indices)
            {
                accum.Indices.push_back(indexBase + idx);
            }
        }

        // マテリアルインデックスの昇順(assimpのマテリアル配列順)に処理することで、
        // 生成される.kmodelのメッシュ順が実行のたびに変わらないようにする
        for (unsigned int materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex)
        {
            const auto accumIt = meshesByMaterial.find(materialIndex);
            if (accumIt == meshesByMaterial.end() || accumIt->second.Indices.empty())
            {
                continue;
            }

            const aiMaterial* material = scene->mMaterials[materialIndex];
            aiString texPath;

            SourceMesh outMesh;
            outMesh.Vertices = std::move(accumIt->second.Vertices);
            outMesh.Indices = std::move(accumIt->second.Indices);

            if (material->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath) == AI_SUCCESS ||
                material->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS)
            {
                outMesh.BaseColorPath = ResolveTexturePath(directory, Utf8ToWide(UriDecode(texPath.C_Str())));
            }

            // OBJ形式は法線マップをaiTextureType_NORMALSではなくmap_bump(aiTextureType_HEIGHT)として
            // 格納する慣習があるため、NORMALSが無い場合はHEIGHTにもフォールバックする
            if (material->GetTexture(aiTextureType_NORMALS, 0, &texPath) == AI_SUCCESS ||
                material->GetTexture(aiTextureType_HEIGHT, 0, &texPath) == AI_SUCCESS)
            {
                outMesh.NormalPath = ResolveTexturePath(directory, Utf8ToWide(UriDecode(texPath.C_Str())));
            }

            // glTFのmetallicRoughnessテクスチャはG=ラフネス、B=メタリックを1枚に格納しており、
            // assimpはこれをROUGHNESS/METALNESSの両方のテクスチャタイプとして同じ画像を指す
            if (material->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &texPath) == AI_SUCCESS ||
                material->GetTexture(aiTextureType_METALNESS, 0, &texPath) == AI_SUCCESS)
            {
                outMesh.MetallicRoughnessPath = ResolveTexturePath(directory, Utf8ToWide(UriDecode(texPath.C_Str())));
            }

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

            model.Meshes.push_back(std::move(outMesh));
        }

        return model;
    }
}
