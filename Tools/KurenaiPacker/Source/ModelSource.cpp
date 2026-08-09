#include "ModelSource.h"

#include <Windows.h>

#include <assimp/GltfMaterial.h>
#include <assimp/Importer.hpp>
#include <assimp/ObjMaterial.h>
#include <assimp/light.h>
#include <assimp/metadata.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

#include "Core/Logger.h"
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
        // 扱うと実ファイル名と一致せず読み込みに失敗する(ファイル名に空白を含むテクスチャで確認済み。
        // glTF側は空白を"%20"のようにエンコードして格納している)。
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
        // (パック時に一度だけ確定させるため、ランタイムにはパス探索そのものが無い)
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
        // そこで平均化がブロックされて継ぎ目が硬い線として見えてしまう(ガラス製小物のような、
        // 円筒状UV展開を持つメッシュで実際に確認済み)。ミラーUV(左右反転コピー)のような本当に別方向を向く継ぎ目は
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

        // aiScene::mLightsをSourceLightへ変換する。呼び出し元はscene->mNumLights > 0のときだけ呼ぶ
        void ImportLights(const aiScene* scene, const std::wstring& filePath, std::vector<SourceLight>& outLights)
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

                SourceLightType type;
                bool enabledByDefault = true;
                switch (light->mType)
                {
                case aiLightSource_POINT:
                    type = SourceLightType::Point;
                    break;
                case aiLightSource_SPOT:
                    type = SourceLightType::Spot;
                    break;
                case aiLightSource_DIRECTIONAL:
                    // b0の太陽(平行光)との二重照明を避けるため既定で無効にする。ImGuiから有効化できる
                    type = SourceLightType::Directional;
                    enabledByDefault = false;
                    break;
                default:
                    // aiLightSource_AMBIENT(環境光はAmbientColorが担当)・aiLightSource_AREA
                    // (エリアライトは今回未実装。FBXのエリアライトはassimpがUNDEFINEDへ落とす)・
                    // aiLightSource_UNDEFINEDはここでまとめてスキップする
                    Kurenai::Core::Logger::Warning("ModelSource", "未対応のライト種別のためスキップします: " + name);
                    continue;
                }

                const auto nodeIt = nodeTransforms.find(name);
                if (nodeIt == nodeTransforms.end())
                {
                    Kurenai::Core::Logger::Warning("ModelSource", "ライトに対応するノードが見つかりません: " + name);
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
                    Kurenai::Core::Logger::Warning("ModelSource", "ライトの色/強度が0のためスキップします: " + name);
                    continue;
                }

                SourceLight outLight;
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
                    Kurenai::Core::Logger::Info(
                        "ModelSource",
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
                    Kurenai::Core::Logger::Info("ModelSource", "ライト\"" + name + "\"のRangeを推定しました: " + std::to_string(range));
                }
                outLight.Range = range;

                // コーン角はSPOTのときだけ読む(aiLightの既定値は2πなのでPOINTで読むと壊れる)
                if (type == SourceLightType::Spot)
                {
                    float inner = light->mAngleInnerCone;
                    float outer = light->mAngleOuterCone;
                    if (inner > outer)
                    {
                        std::swap(inner, outer);
                        Kurenai::Core::Logger::Warning("ModelSource", "スポットライトの内側角が外側角より大きいため入れ替えました: " + name);
                    }
                    outer = std::clamp(outer, 0.0f, kHalfPi);
                    inner = std::clamp(inner, 0.0f, outer);
                    outLight.SpotInnerConeAngle = inner;
                    outLight.SpotOuterConeAngle = outer;
                }

                outLights.push_back(std::move(outLight));
            }
        }
    }

    SourceModel LoadSourceModel(const std::wstring& filePath, float scale, const MaterialOverride& materialOverride)
    {
        Assimp::Importer importer;
        // GenSmoothNormalsは対象アセットは全メッシュが法線を持つため実質ノーオップであり、
        // 万一法線を持たないメッシュがあった場合のみ後段のフォールバック(上向き固定法線)が使われる。
        // 接線はassimpのaiProcess_CalcTangentSpaceに任せず自前で計算する(下記の頂点ループ内、
        // TangentAccumKey関連のコード参照)。CalcTangentSpaceはUV面積がほぼ0(縮退)の三角形で
        // 接線が数値的に不安定になり、位置・法線・UVが完全に同一の頂点間でさえ接線がほぼ正反対に
        // なることがある。JoinIdenticalVerticesは重複頂点を減らせるため付けておく
        // (自前の接線平均化は位置+法線をキーにしており重複頂点の有無に依存しないため、
        // 平均化の正しさ自体には影響しない)
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
        // 引き起こしうる(実測した大規模なOBJ配布では、実質132マテリアルに対して
        // usemtl切り替えが22,396回あった)ため、マテリアル単位でまとめてドローコール数を実質マテリアル数まで
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
            // 接線がほぼ正反対になることがある(ガラス製小物のメッシュで実際に確認済み)。
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
                aiVector3D position = (transform * mesh->mVertices[v]) * scale;
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
                // ライトマップUVの既定値はマテリアルUVの複製にする。--bake-occlusionで
                // xatlasの展開結果に差し替えられるまではこの値が使われ、glTFの
                // occlusionTextureのように元からTEXCOORD0の空間で作られた遮蔽マップが
                // そのまま正しく引ける(Vertex.hのUV1のコメント参照)
                vertex.UV1[0] = vertex.UV[0];
                vertex.UV1[1] = vertex.UV[1];
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

            if (material->GetTexture(aiTextureType_EMISSIVE, 0, &texPath) == AI_SUCCESS)
            {
                outMesh.EmissivePath = ResolveTexturePath(directory, Utf8ToWide(UriDecode(texPath.C_Str())));
            }

            // ベイク済みアンビエントオクルージョン(遮蔽マップ)。
            // assimpはglTFのocclusionTextureをaiTextureType_LIGHTMAPへマップする
            // (ThirdParty/assimp/code/AssetLib/glTF2/glTF2Importer.cpp参照)。明示的な
            // AMBIENT_OCCLUSIONスロットを持つ形式もあるため、そちらもフォールバックとして見る。
            //
            // 【aiTextureType_AMBIENT(OBJのmap_Ka)は意図的に見ない】法線マップのNORMALS→HEIGHT
            // フォールバックと同じ発想で最初は含めていたが、WavefrontMTLのmap_Kaは「アンビエント色の
            // マップ」であって遮蔽率ではなく、実際にはmap_Kdと同じ拡散テクスチャを指す慣習になっている。
            // 同梱のBistro(exterior.mtl/interior.mtl)も全マテリアルのmap_Kaが*_diff.pngを指していた。
            // これを遮蔽率として採用すると、アルベドがそのまま環境光の減衰係数として掛かり、
            // 色付きで極端に暗くなってしまう
            if (material->GetTexture(aiTextureType_LIGHTMAP, 0, &texPath) == AI_SUCCESS ||
                material->GetTexture(aiTextureType_AMBIENT_OCCLUSION, 0, &texPath) == AI_SUCCESS)
            {
                outMesh.OcclusionPath = ResolveTexturePath(directory, Utf8ToWide(UriDecode(texPath.C_Str())));
            }

            // glTFのocclusionTexture.strength。ラフネス係数と異なり既定値がglTF仕様で1.0と
            // 明記されているため、取得できない場合は無効値ではなくその既定値を採用する
            // (assimpはLIGHTMAPスロットのプロパティとして格納する)
            outMesh.OcclusionStrength = Kurenai::Assets::kDefaultOcclusionStrength;

            float occlusionStrength = 0.0f;
            if (material->Get(AI_MATKEY_GLTF_TEXTURE_STRENGTH(aiTextureType_LIGHTMAP, 0), occlusionStrength) == AI_SUCCESS)
            {
                // 遮蔽の強度は[0,1]が仕様上の値域。範囲外は壊れたデータとみなし既定値へ戻す
                if (occlusionStrength >= 0.0f && occlusionStrength <= 1.0f)
                {
                    outMesh.OcclusionStrength = occlusionStrength;
                }
                else
                {
                    Kurenai::Core::Logger::Warning(
                        "ModelSource",
                        "遮蔽の強度が[0,1]の範囲外のため既定値1.0として扱います: " + std::to_string(occlusionStrength));
                }
            }

            // FBXなどPBRメタリック係数を持たない形式では既定値(非金属)のままになる
            material->Get(AI_MATKEY_METALLIC_FACTOR, outMesh.MetallicFactor);

            // ラフネスは「ソースデータが持っていなければ無効値を書き出す」方針を取る。
            // もっともらしい既定値(例: 0.7)をパッカーが勝手に埋めると、データに無い値が
            // 事実として下流へ流れてしまい、消費側は「指定された0.7」と「データに無かった」を
            // 区別できなくなる。何を既定値とするかはフォーマットやアセットによって異なり、
            // 変換ツールが決めてよい値ではない。無効値の解釈は消費側の責任とする
            // (シェーダーは係数1.0=テクスチャの値をそのまま使う、として扱う)
            outMesh.RoughnessFactor = Kurenai::Assets::kInvalidMaterialFactor;

            float roughnessFactor = 0.0f;
            if (material->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor) == AI_SUCCESS)
            {
                // 古いPhong系マテリアル(Shininessのみ持つ)をassimpがPBRラフネスへ変換する際、
                // 変換式が破綻して[0,1]範囲外の値を返すことがある(実測で負値を確認済み)。
                // そのまま流すとシェーダー側のclampで最小ラフネス(ほぼ鏡面)に張り付いてしまうため、
                // 範囲外は「取得できなかった」と同じ扱いにする
                if (roughnessFactor >= 0.0f && roughnessFactor <= 1.0f)
                {
                    outMesh.RoughnessFactor = roughnessFactor;
                }
                else
                {
                    Kurenai::Core::Logger::Warning(
                        "ModelSource",
                        "ラフネス係数が[0,1]の範囲外のため無効値として扱います: " + std::to_string(roughnessFactor));
                }
            }
            else
            {
                // WavefrontMTL(OBJ)はPBRのラフネスを持たない代わりに、Blinn-Phongの鏡面反射指数Ns
                // (assimpがAI_MATKEY_SHININESSへ格納する)を持っていることがある。変換式は
                // Blinn-Phong指数 → GGXラフネス の一般的な近似 roughness = sqrt(2 / (Ns + 2))
                // (Ns→∞で0、Ns=0で1に漸近する)。
                //
                // ただしNs = 100.000 ちょうどの値は採用しない。これはOBJエクスポータが書き出す
                // 定型の既定値で、マテリアルごとに調整された値ではないため。実測したMTLでは
                // 全マテリアルの8割前後がこの値ちょうどで、石畳や漆喰のような明らかに粗い材質まで
                // 含まれていた。これを採用するとそれらがラフネス0.14(ほぼ鏡面)になり、
                // 環境光の鏡面反射がシーン全体へ強くかかって白く霞んだ絵になることを実機で確認している。
                // 情報量の無い値として無視し、無効値のままにする
                constexpr float kMtlDefaultShininess = 100.0f;
                float shininess = 0.0f;
                if (material->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && shininess > 0.0f &&
                    std::abs(shininess - kMtlDefaultShininess) > 0.001f)
                {
                    outMesh.RoughnessFactor = std::clamp(std::sqrt(2.0f / (shininess + 2.0f)), 0.0f, 1.0f);
                }
            }

            aiColor3D emissiveColor(0.0f, 0.0f, 0.0f);
            material->Get(AI_MATKEY_COLOR_EMISSIVE, emissiveColor);
            outMesh.EmissiveFactor[0] = emissiveColor.r;
            outMesh.EmissiveFactor[1] = emissiveColor.g;
            outMesh.EmissiveFactor[2] = emissiveColor.b;

            // glTFのpbrMetallicRoughness.baseColorFactor(既定[1,1,1,1])。テクスチャを持たず
            // baseColorFactorのみで色/不透明度を表現するマテリアル(ガラス等)を正しく再現するために
            // 読み取る。取得できない場合(FBX/OBJ等、AI_MATKEY_BASE_COLORはglTF専用のため常に失敗する)は
            // 代わりにOBJ/FBXの古典的なPhongモデルのKd(拡散色)であるAI_MATKEY_COLOR_DIFFUSEを使う
            // (ガラス系マテリアルによくあるKdが黒[0,0,0]のケースを、白1x1プレースホルダーへ
            // 誤ってフォールバックさせないため)
            aiColor4D baseColorFactor(1.0f, 1.0f, 1.0f, 1.0f);
            if (material->Get(AI_MATKEY_BASE_COLOR, baseColorFactor) != AI_SUCCESS)
            {
                aiColor3D diffuseColor(1.0f, 1.0f, 1.0f);
                material->Get(AI_MATKEY_COLOR_DIFFUSE, diffuseColor);
                baseColorFactor = aiColor4D(diffuseColor.r, diffuseColor.g, diffuseColor.b, 1.0f);
            }

            // アルファモード(BLEND/MASK/OPAQUE)はglTFの拡張情報でのみ判定できる(FBX/OBJ等には
            // この3値の概念自体が無い)。MASKはAlphaCutoffを設定してGBuffer.hlsl側のclip()で
            // カットアウトさせ、BLENDはIsTransparent=trueにしてKurenaiEngine3DのTransparentパス
            // (フォワードシェーディング+アルファブレンド)へ回す
            aiString alphaMode;
            float alphaCutoff = 0.5f;
            material->Get(AI_MATKEY_GLTF_ALPHACUTOFF, alphaCutoff);
            const bool hasAlphaMode = material->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS;
            outMesh.AlphaCutoff = (hasAlphaMode && std::strcmp(alphaMode.C_Str(), "MASK") == 0) ? alphaCutoff : 0.0f;
            outMesh.IsTransparent = hasAlphaMode && std::strcmp(alphaMode.C_Str(), "BLEND") == 0;

            // OBJ等、alphaModeの概念を持たない形式ではAI_MATKEY_OPACITY(WavefrontMTLの
            // d/Trから変換された不透明度。assimpのObjFileImporter.cppが
            // AI_MATKEY_OPACITYへ格納する)を見る。d/Trがどちらも無ければassimpは既定の1.0
            // (不透明)を返すため、架空の値を補うことにはならない
            float opacity = 1.0f;
            if (!hasAlphaMode && material->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS && opacity < 0.999f)
            {
                outMesh.IsTransparent = true;
                baseColorFactor.a = opacity;
            }

            // WavefrontMTLには、d/Trとは別に「透過を伴う照明モデル(illum 4/6/7/9)+ Tf(透過フィルタ)」で
            // 透明度を表現する書き方がある。assimpのOBJインポータはTfをAI_MATKEY_COLOR_TRANSPARENTへ、
            // illumをAI_MATKEY_OBJ_ILLUMへ格納するだけで、不透明度(AI_MATKEY_OPACITY)には一切反映しない
            // (ObjFileImporter.cppのCreateMaterial。AI_MATKEY_OPACITYへ入るのはd/Tr由来の値のみ)。
            // そのためd/Trだけを見ていると、この書き方のマテリアルがすべて不透明として扱われる。
            // 実測した大規模なOBJ配布では、瓶・窓・街灯といったガラス系マテリアルの大半がこの書き方で、
            // d/Trのみの判定ではシーン全体で半透明メッシュが計36三角形しか出ていなかった。
            //
            // Tfの解釈: MTLの原仕様では「透過光にかけるフィルタ色」(Tf=1,1,1が無着色=全透過)だが、
            // Tf = 1 - Tr(= d)、つまり不透明度そのものとして書き出すエクスポータが実在する。
            // d/TrとTfを両方持つマテリアルの実測値(Tr 0.800/Tf 0.2、Tr 0.900/Tf 0.1、
            // Tr 0.000/Tf 1.0)がいずれもTf == 1 - Trで一貫していたため、Tfを不透明度として読む。
            // assimpのTf既定値も(1,1,1)(ObjFileData.hのMaterial::transparent)なので、Tfを書いていない
            // 通常のOBJがこの経路で誤って半透明化されることはない。
            // なお原仕様どおり「Tf=透過率」と解釈するとTf未記載の全マテリアルが全透過になってしまい、
            // 実用にならない点でも、この解釈以外に選択肢がない
            if (!hasAlphaMode && !outMesh.IsTransparent)
            {
                int illuminationModel = 1;
                aiColor3D transmissionFilter(1.0f, 1.0f, 1.0f);
                if (material->Get(AI_MATKEY_OBJ_ILLUM, illuminationModel) == AI_SUCCESS &&
                    (illuminationModel == 4 || illuminationModel == 6 || illuminationModel == 7 || illuminationModel == 9) &&
                    material->Get(AI_MATKEY_COLOR_TRANSPARENT, transmissionFilter) == AI_SUCCESS)
                {
                    const float filterOpacity =
                        (transmissionFilter.r + transmissionFilter.g + transmissionFilter.b) / 3.0f;
                    if (filterOpacity < 0.999f)
                    {
                        outMesh.IsTransparent = true;
                        baseColorFactor.a = std::clamp(filterOpacity, 0.0f, 1.0f);

                        aiString materialName;
                        material->Get(AI_MATKEY_NAME, materialName);
                        Kurenai::Core::Logger::Info(
                            "ModelSource",
                            std::string("マテリアル\"") + materialName.C_Str() + "\"をillum " +
                                std::to_string(illuminationModel) + " + Tfから半透明と判定しました(不透明度 " +
                                std::to_string(baseColorFactor.a) + ")");
                    }
                }
            }

            outMesh.BaseColorFactor[0] = baseColorFactor.r;
            outMesh.BaseColorFactor[1] = baseColorFactor.g;
            outMesh.BaseColorFactor[2] = baseColorFactor.b;
            outMesh.BaseColorFactor[3] = baseColorFactor.a;

            model.Meshes.push_back(std::move(outMesh));
        }

        if (scene->mNumLights > 0)
        {
            ImportLights(scene, filePath, model.Lights);
        }

        // マテリアル係数の上書き(--metallic/--roughness/--base-color)。
        // ソースが持っていた値を無条件で置き換えるため、解析の最後にまとめて適用する
        for (SourceMesh& mesh : model.Meshes)
        {
            if (materialOverride.MetallicFactor)
            {
                mesh.MetallicFactor = *materialOverride.MetallicFactor;
            }
            if (materialOverride.RoughnessFactor)
            {
                mesh.RoughnessFactor = *materialOverride.RoughnessFactor;
            }
            if (materialOverride.BaseColor)
            {
                mesh.BaseColorFactor[0] = (*materialOverride.BaseColor)[0];
                mesh.BaseColorFactor[1] = (*materialOverride.BaseColor)[1];
                mesh.BaseColorFactor[2] = (*materialOverride.BaseColor)[2];
                // アルファは上書きしない(不透明度はalphaMode/Tf由来の判定を尊重する)
            }
        }

        return model;
    }
}
