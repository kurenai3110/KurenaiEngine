#include "ModelSource.h"

#include <Windows.h>

#include <psapi.h>

#include <assimp/GltfMaterial.h>
#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/ObjMaterial.h>
#include <assimp/light.h>
#include <assimp/metadata.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <DirectXTex.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
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
        // 法線を持たないアセットに対して assimp が法線を生成するときの、稜線を平均化する
        // 上限角度。これより開いた稜線は頂点を分けて面法線にする。
        // 【30度にしてある根拠】PLATEAU の建物は壁と屋根が直角に交わる押し出し形状で、
        // assimp の既定(175度)ではその直角まで平均化されて丸まる。一方で地形(dem)には
        // なだらかな起伏があり、面法線だけにすると三角形の切れ目が段差として出る。
        // 30度なら建物の角は分かれ、地形の緩い勾配は繋がる
        constexpr float kMaxSmoothingAngleDegrees = 30.0f;

        // === 計測 ==============================================================
        //
        // どのフェーズで時間が溶けているかを推測しないための計装(OcclusionBaker.cppの
        // BakeTimingsと同じ考え方)。解析は数十ms〜数秒で、now()の呼び出しは
        // メッシュごと・フェーズごとにしか入れないため、計測自体の影響は無視できる
        using PhaseClock = std::chrono::steady_clock;

        double PhaseSecondsSince(const PhaseClock::time_point& start)
        {
            return std::chrono::duration<double>(PhaseClock::now() - start).count();
        }

        // スコープを抜けるときに累計へ足す。早期continueのあるブロックでも取りこぼさない
        struct ScopedPhase
        {
            double& Target;
            PhaseClock::time_point Start;

            explicit ScopedPhase(double& target) : Target(target), Start(PhaseClock::now()) {}
            ~ScopedPhase() { Target += PhaseSecondsSince(Start); }

            ScopedPhase(const ScopedPhase&) = delete;
            ScopedPhase& operator=(const ScopedPhase&) = delete;
        };

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

        // aiTexture::achFormatHintを拡張子として使える形へ均す。assimpのバージョンや形式によって
        // "jpg" / "*.jpg" / "JPG" / 空文字 のいずれもありうるため、英数字だけを小文字で拾う
        std::string SanitizeFormatHint(const char* hint)
        {
            std::string result;
            if (hint == nullptr)
            {
                return result;
            }
            for (const char* p = hint; *p != '\0' && result.size() < 8; ++p)
            {
                const unsigned char c = static_cast<unsigned char>(*p);
                if (std::isalnum(c))
                {
                    result.push_back(static_cast<char>(std::tolower(c)));
                }
            }
            return result;
        }

        // achFormatHintが当てにならなかったときに、先頭バイトから形式を当てる。
        // 【推測で拡張子を付けない】拡張子はTextureImage::LoadFromFileの分岐そのものなので、
        // 間違えるとDDS/TGA扱いになってミップもBC7も行われない。当てられなければ空を返し、
        // 呼び出し側に警告を出させる
        std::string GuessExtensionFromMagic(const unsigned char* data, size_t size)
        {
            if (data == nullptr || size < 4)
            {
                return std::string();
            }
            if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
            {
                return "jpg";
            }
            if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
            {
                return "png";
            }
            if ((data[0] == 'I' && data[1] == 'I' && data[2] == 0x2A && data[3] == 0x00) ||
                (data[0] == 'M' && data[1] == 'M' && data[2] == 0x00 && data[3] == 0x2A))
            {
                return "tif";
            }
            if (data[0] == 'D' && data[1] == 'D' && data[2] == 'S' && data[3] == ' ')
            {
                return "dds";
            }
            if (data[0] == 'B' && data[1] == 'M')
            {
                return "bmp";
            }
            return std::string();
        }

        // 一時ファイル名に使えない文字を落とす。埋め込みテクスチャのmFilenameは
        // "..\..\FME_TEMP\...\skjp5921.jpg" のようなパスを名乗ることがあるため、
        // ディレクトリ部を捨ててから使う
        std::wstring SanitizeFileNameStem(const std::string& name)
        {
            const std::wstring wide = Utf8ToWide(name);
            const size_t slashPos = wide.find_last_of(L"/\\");
            std::wstring base = slashPos == std::wstring::npos ? wide : wide.substr(slashPos + 1);
            const size_t dotPos = base.find_last_of(L'.');
            if (dotPos != std::wstring::npos)
            {
                base = base.substr(0, dotPos);
            }

            std::wstring result;
            for (wchar_t c : base)
            {
                if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
                    c == L'"' || c == L'<' || c == L'>' || c == L'|' || c < 32)
                {
                    continue;
                }
                result.push_back(c);
                if (result.size() >= 48)
                {
                    break;
                }
            }
            return result;
        }

        // sceneのテクスチャ配列から、そのポインタの位置(番号)を求める。
        // GetEmbeddedTextureはポインタしか返さず、番号は決定的なファイル名を作るのに要る
        bool FindEmbeddedTextureIndex(const aiScene* scene, const aiTexture* texture, unsigned int& outIndex)
        {
            for (unsigned int i = 0; i < scene->mNumTextures; ++i)
            {
                if (scene->mTextures[i] == texture)
                {
                    outIndex = i;
                    return true;
                }
            }
            return false;
        }

        // 埋め込みテクスチャを、assimpの検索(GetEmbeddedTexture)で見つからなかった場合に
        // ベース名だけで線形探索する。
        //
        // 【なぜ要るのか】Project PLATEAUのLOD2はマテリアルが
        // "..\..\FME_TEMP\wbrun_1648613083830_64256\...\skjp5921.jpg" という実在しない一時パスを
        // 指しており、aiTexture側のmFilenameがどう入っているかは配布物によって違う。
        // assimpの突き合わせが外れても、ファイル名が一致すれば同じ画像とみなしてよい
        const aiTexture* FindEmbeddedTextureByFileName(const aiScene* scene, const std::string& rawPath)
        {
            const size_t slashPos = rawPath.find_last_of("/\\");
            std::string fileName = slashPos == std::string::npos ? rawPath : rawPath.substr(slashPos + 1);
            if (fileName.empty())
            {
                return nullptr;
            }
            std::transform(fileName.begin(), fileName.end(), fileName.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            for (unsigned int i = 0; i < scene->mNumTextures; ++i)
            {
                const aiTexture* texture = scene->mTextures[i];
                std::string candidate = texture->mFilename.C_Str();
                const size_t candidateSlash = candidate.find_last_of("/\\");
                if (candidateSlash != std::string::npos)
                {
                    candidate = candidate.substr(candidateSlash + 1);
                }
                std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!candidate.empty() && candidate == fileName)
                {
                    return texture;
                }
            }
            return nullptr;
        }

        // テクスチャ参照(assimpが返した生のパス)を、実際に読めるファイルパスへ解決する。
        // 実ファイルが見つからなければ埋め込みテクスチャを探し、見つかれば一時ファイルへ取り出す。
        //
        // 【従来の挙動を変えないこと】実ファイルが見つかった場合も、埋め込みも無い場合も、
        // 戻り値はResolveTexturePathと完全に同じ。埋め込みが実在するときにだけ経路が増える
        std::wstring ResolveTextureReference(
            const aiScene* scene,
            const std::shared_ptr<EmbeddedTextureStore>& store,
            const std::wstring& directory,
            const aiString& rawTexPath)
        {
            const std::string rawUtf8 = UriDecode(rawTexPath.C_Str());
            const std::wstring rawWide = Utf8ToWide(rawUtf8);
            const std::wstring resolved = ResolveTexturePath(directory, rawWide);
            if (FileExists(resolved))
            {
                return resolved;
            }

            if (scene == nullptr || scene->mNumTextures == 0 || !store)
            {
                return resolved;
            }

            // assimpの規約("*N"表記とファイル名照合)で引く。外したらベース名で線形に探す
            const aiTexture* texture = scene->GetEmbeddedTexture(rawTexPath.C_Str());
            const char* foundBy = "GetEmbeddedTexture";
            if (texture == nullptr)
            {
                texture = FindEmbeddedTextureByFileName(scene, rawUtf8);
                foundBy = "ファイル名の線形探索";
            }
            if (texture == nullptr)
            {
                return resolved;
            }

            unsigned int textureIndex = 0;
            if (!FindEmbeddedTextureIndex(scene, texture, textureIndex))
            {
                Kurenai::Core::Logger::Warning("KurenaiPacker",
                    "埋め込みテクスチャの配列番号を特定できませんでした: " + rawUtf8);
                return resolved;
            }

            std::wstring extracted;
            if (texture->mHeight == 0)
            {
                // 圧縮ブロブ(JPEG/PNG/TIFF等)。mWidthがバイト数
                extracted = store->StoreCompressed(
                    textureIndex,
                    SanitizeFormatHint(texture->achFormatHint),
                    texture->mFilename.C_Str(),
                    texture->pcData,
                    static_cast<size_t>(texture->mWidth));
            }
            else
            {
                // 非圧縮のARGB8888。aiTexelはb,g,r,aの順に並んでおりBGRA8と同じ配置
                extracted = store->StoreUncompressed(
                    textureIndex,
                    texture->mFilename.C_Str(),
                    texture->pcData,
                    texture->mWidth,
                    texture->mHeight);
            }

            if (extracted.empty())
            {
                Kurenai::Core::Logger::Warning("KurenaiPacker",
                    "埋め込みテクスチャの取り出しに失敗しました(このスロットは指定なしとして扱われます): " + rawUtf8);
                return resolved;
            }

            // 【全件は出さない】LOD2は1タイルで1,714枚あり、1行ずつ出すとログが実用にならない。
            // 経路が正しいことを確かめるのに要るのは先頭の数件で、総数はパック完了の
            // サマリ(「埋め込みテクスチャ N枚を取り出し」)が受け持つ
            constexpr size_t kVerboseExtractionLogCount = 5;
            if (store->ExtractedCount() <= kVerboseExtractionLogCount)
            {
                Kurenai::Core::Logger::Info("KurenaiPacker",
                    std::string("埋め込みテクスチャを取り出しました(") + foundBy + "): "
                    + rawUtf8 + " -> " + WideToUtf8(extracted));
            }
            return extracted;
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

    // === EmbeddedTextureStore ===

    EmbeddedTextureStore::~EmbeddedTextureStore()
    {
        if (m_Directory.empty())
        {
            return;
        }
        // 【デストラクタから例外を出さない】後始末に失敗してもパック結果は既に書けているので、
        // 落とさずに警告だけ残す(一時ファイルは%TEMP%配下なのでOSの掃除でも消える)
        std::error_code ec;
        const uintmax_t removed = std::filesystem::remove_all(std::filesystem::path(m_Directory), ec);
        if (ec)
        {
            Kurenai::Core::Logger::Warning("KurenaiPacker",
                "埋め込みテクスチャの一時ディレクトリを削除できませんでした(" + ec.message() + "): "
                + WideToUtf8(m_Directory));
        }
        else
        {
            Kurenai::Core::Logger::Info("KurenaiPacker",
                "埋め込みテクスチャの一時ディレクトリを削除しました(" + std::to_string(removed) + "項目): "
                + WideToUtf8(m_Directory));
        }
    }

    bool EmbeddedTextureStore::EnsureDirectory()
    {
        if (!m_Directory.empty())
        {
            return true;
        }

        wchar_t tempRoot[MAX_PATH + 1] = {};
        const DWORD length = GetTempPathW(MAX_PATH, tempRoot);
        if (length == 0 || length > MAX_PATH)
        {
            Kurenai::Core::Logger::Error("KurenaiPacker",
                "一時ディレクトリのパスを取得できませんでした(GetTempPathW)");
            return false;
        }

        // 同じプロセスで複数のモデルを扱う場合や、前回の残骸がある場合に備えて連番で退避する
        for (unsigned int attempt = 0; attempt < 64; ++attempt)
        {
            std::wstring candidate = std::wstring(tempRoot) + L"KurenaiPacker_"
                + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(attempt);
            if (CreateDirectoryW(candidate.c_str(), nullptr))
            {
                m_Directory = candidate + L"\\";
                Kurenai::Core::Logger::Info("KurenaiPacker",
                    "埋め込みテクスチャの取り出し先: " + WideToUtf8(m_Directory));
                return true;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS)
            {
                Kurenai::Core::Logger::Error("KurenaiPacker",
                    "一時ディレクトリを作成できませんでした(GetLastError=" + std::to_string(GetLastError())
                    + "): " + WideToUtf8(candidate));
                return false;
            }
        }

        Kurenai::Core::Logger::Error("KurenaiPacker",
            "一時ディレクトリの候補が64件とも既に存在するため、埋め込みテクスチャを取り出せません");
        return false;
    }

    std::wstring EmbeddedTextureStore::StoreCompressed(
        unsigned int textureIndex,
        const std::string& formatHint,
        const std::string& originalName,
        const void* data,
        size_t sizeInBytes)
    {
        const auto it = m_Extracted.find(textureIndex);
        if (it != m_Extracted.end())
        {
            return it->second;
        }
        if (data == nullptr || sizeInBytes == 0)
        {
            Kurenai::Core::Logger::Warning("KurenaiPacker",
                "埋め込みテクスチャ" + std::to_string(textureIndex) + "の中身が空です");
            return std::wstring();
        }
        if (!EnsureDirectory())
        {
            return std::wstring();
        }

        std::string extension = formatHint;
        if (extension.empty() || extension == "bin")
        {
            extension = GuessExtensionFromMagic(static_cast<const unsigned char*>(data), sizeInBytes);
        }
        if (extension.empty())
        {
            Kurenai::Core::Logger::Warning("KurenaiPacker",
                "埋め込みテクスチャ" + std::to_string(textureIndex)
                + "の形式を判別できませんでした(achFormatHintも先頭バイトも一致せず)");
            return std::wstring();
        }

        const std::wstring stem = SanitizeFileNameStem(originalName);
        wchar_t indexText[16] = {};
        swprintf_s(indexText, L"emb%04u", textureIndex);
        std::wstring fileName = indexText;
        if (!stem.empty())
        {
            fileName += L"_" + stem;
        }
        fileName += L"." + Utf8ToWide(extension);

        const std::wstring fullPath = m_Directory + fileName;
        std::ofstream file(fullPath, std::ios::binary);
        if (!file)
        {
            Kurenai::Core::Logger::Error("KurenaiPacker",
                "埋め込みテクスチャを書き出せませんでした: " + WideToUtf8(fullPath));
            return std::wstring();
        }
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(sizeInBytes));
        if (!file)
        {
            Kurenai::Core::Logger::Error("KurenaiPacker",
                "埋め込みテクスチャの書き出しが途中で失敗しました: " + WideToUtf8(fullPath));
            return std::wstring();
        }
        file.close();

        m_Extracted.emplace(textureIndex, fullPath);
        return fullPath;
    }

    std::wstring EmbeddedTextureStore::StoreUncompressed(
        unsigned int textureIndex,
        const std::string& originalName,
        const void* bgraTexels,
        unsigned int width,
        unsigned int height)
    {
        const auto it = m_Extracted.find(textureIndex);
        if (it != m_Extracted.end())
        {
            return it->second;
        }
        if (bgraTexels == nullptr || width == 0 || height == 0)
        {
            Kurenai::Core::Logger::Warning("KurenaiPacker",
                "埋め込みテクスチャ" + std::to_string(textureIndex) + "の寸法が不正です");
            return std::wstring();
        }
        if (!EnsureDirectory())
        {
            return std::wstring();
        }

        DirectX::Image image{};
        image.width = width;
        image.height = height;
        image.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        image.rowPitch = static_cast<size_t>(width) * 4;
        image.slicePitch = image.rowPitch * height;
        image.pixels = const_cast<uint8_t*>(static_cast<const uint8_t*>(bgraTexels));

        const std::wstring stem = SanitizeFileNameStem(originalName);
        wchar_t indexText[16] = {};
        swprintf_s(indexText, L"emb%04u", textureIndex);
        std::wstring fileName = indexText;
        if (!stem.empty())
        {
            fileName += L"_" + stem;
        }
        fileName += L".png";
        const std::wstring fullPath = m_Directory + fileName;

        // WICはCOMを要求する。ここは(ワーカースレッドではなく)解析中の呼び出し元スレッドなので、
        // この場で初期化して使い終わったら戻す
        const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comInitialized = SUCCEEDED(coHr);
        const HRESULT hr = DirectX::SaveToWICFile(
            image, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), fullPath.c_str());
        if (comInitialized)
        {
            CoUninitialize();
        }
        if (FAILED(hr))
        {
            Kurenai::Core::Logger::Error("KurenaiPacker",
                "非圧縮の埋め込みテクスチャをPNGへ書き出せませんでした(hr=0x"
                + std::to_string(static_cast<unsigned long>(hr)) + "): " + WideToUtf8(fullPath));
            return std::wstring();
        }

        m_Extracted.emplace(textureIndex, fullPath);
        return fullPath;
    }

    SourceModel LoadSourceModel(
        const std::wstring& filePath,
        float scale,
        const MaterialOverride& materialOverride,
        const std::optional<std::array<float, 3>>& originOffset,
        ParseTimings* outTimings)
    {
        // outTimingsがnullptrでも分岐を増やさずに済むよう、常にローカルへ積んで最後に転記する
        ParseTimings timings;

        Assimp::Importer importer;
        // 【GenSmoothNormalsは法線を持たないアセットのためにある】assimpのこの処理は
        // 法線を既に持つメッシュには何もしないため、法線付きのアセット(Sponza/Bistro等)の
        // 出力は変わらない。一方 PLATEAU の FBX は CityGML 由来で法線を1つも持っておらず、
        // これが無いと後段のフォールバック(上向き固定法線)が全頂点に入る。その結果、
        // 垂直な壁も真上を向いていることになり、面の向きによる明暗(陰)が一切出なくなる。
        // 落ち影は法線と無関係に出るので絵は「それらしく」見え、気づきにくい
        //
        // 【スムージング角度を絞る】既定の175度ではほぼ全ての稜線が平均化され、建物の
        // 壁と屋根の直角まで丸まる。この角度より開いた稜線は頂点を分けて面法線にする
        importer.SetPropertyFloat(AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, kMaxSmoothingAngleDegrees);
        // 接線はassimpのaiProcess_CalcTangentSpaceに任せず自前で計算する(下記の頂点ループ内、
        // TangentAccumKey関連のコード参照)。CalcTangentSpaceはUV面積がほぼ0(縮退)の三角形で
        // 接線が数値的に不安定になり、位置・法線・UVが完全に同一の頂点間でさえ接線がほぼ正反対に
        // なることがある。JoinIdenticalVerticesは重複頂点を減らせるため付けておく
        // (自前の接線平均化は位置+法線をキーにしており重複頂点の有無に依存しないため、
        // 平均化の正しさ自体には影響しない)
        const auto readStart = PhaseClock::now();
        const aiScene* scene = importer.ReadFile(
            WideToUtf8(filePath),
            aiProcess_Triangulate | aiProcess_ConvertToLeftHanded | aiProcess_JoinIdenticalVertices |
                aiProcess_GenSmoothNormals);
        timings.ReadSeconds += PhaseSecondsSince(readStart);

        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
        {
            throw std::runtime_error(std::string("モデルの読み込みに失敗しました: ") + importer.GetErrorString());
        }

        const std::wstring directory = GetDirectory(filePath);

        SourceModel model;

        // 埋め込みテクスチャを持つモデルでだけ取り出し先を用意する。
        // 一時ディレクトリの実体は最初の1枚を取り出す時点で作られる(EnsureDirectory)
        if (scene->mNumTextures > 0)
        {
            model.EmbeddedTextures = std::make_shared<EmbeddedTextureStore>();
            Kurenai::Core::Logger::Info("KurenaiPacker",
                "埋め込みテクスチャを" + std::to_string(scene->mNumTextures) + "枚検出しました");
        }

        std::vector<std::pair<const aiMesh*, aiMatrix4x4>> meshNodes;
        const auto collectStart = PhaseClock::now();
        CollectMeshNodes(scene, scene->mRootNode, aiMatrix4x4(), meshNodes);
        timings.CollectSeconds += PhaseSecondsSince(collectStart);

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
                const ScopedPhase timeTangent(timings.TangentSeconds);
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

            const auto vertexLoopStart = PhaseClock::now();
            std::vector<Vertex> vertices;
            vertices.reserve(mesh->mNumVertices);
            for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
            {
                aiVector3D position = transform * mesh->mVertices[v];
                if (originOffset)
                {
                    // --origin。scaleを掛ける前に引くので、呼び出し側はソースの単位
                    // (PLATEAUなら平面直角座標のメートル値)のまま指定できる
                    position.x -= (*originOffset)[0];
                    position.y -= (*originOffset)[1];
                    position.z -= (*originOffset)[2];
                }
                position *= scale;
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
            timings.VertexSeconds += PhaseSecondsSince(vertexLoopStart);

            const auto mergeStart = PhaseClock::now();
            MergedMeshAccumulator& accum = meshesByMaterial[mesh->mMaterialIndex];
            // 頂点を足す前に基準を取る(足した後だと自分の頂点数まで含んでしまう)
            const uint32_t indexBase = static_cast<uint32_t>(accum.Vertices.size());
            accum.Vertices.insert(accum.Vertices.end(), vertices.begin(), vertices.end());

            // 【reserveで「ぴったりのサイズ」を要求してはいけない】ここは以前
            //     accum.Indices.reserve(accum.Indices.size() + indices.size());
            // としていたが、reserveは要求どおりの容量へ確保し直すためvectorの倍々成長が
            // 無効になり、**メッシュを1つ足すたびに全体をコピーし直す**(O(N^2))。
            // PLATEAUのタイルは入力メッシュが1382個あってマテリアルが1つしかないため、
            // 1つのアキュムレータへ1382回追記することになり、これだけで解析時間の51%を
            // 占めていた(12タイルの実測で786ms / 解析1530ms)。push_backに任せて
            // 償却O(N)へ戻す。
            //
            // あわせて、面インデックスを一度std::vectorへ集めてから移し替えるのをやめ、
            // 直接追記する(中間バッファの確保とコピーが1メッシュにつき1回消える)。
            // 追記の順序も値も従来とまったく同じで、出力は1バイトも変わらない
            for (unsigned int f = 0; f < mesh->mNumFaces; ++f)
            {
                const aiFace& face = mesh->mFaces[f];
                for (unsigned int idx = 0; idx < face.mNumIndices; ++idx)
                {
                    accum.Indices.push_back(indexBase + face.mIndices[idx]);
                }
            }
            timings.MergeSeconds += PhaseSecondsSince(mergeStart);
        }

        // マテリアルインデックスの昇順(assimpのマテリアル配列順)に処理することで、
        // 生成される.kmodelのメッシュ順が実行のたびに変わらないようにする
        const auto materialStart = PhaseClock::now();
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
                outMesh.BaseColorPath = ResolveTextureReference(scene, model.EmbeddedTextures, directory, texPath);
            }

            // OBJ形式は法線マップをaiTextureType_NORMALSではなくmap_bump(aiTextureType_HEIGHT)として
            // 格納する慣習があるため、NORMALSが無い場合はHEIGHTにもフォールバックする
            if (material->GetTexture(aiTextureType_NORMALS, 0, &texPath) == AI_SUCCESS ||
                material->GetTexture(aiTextureType_HEIGHT, 0, &texPath) == AI_SUCCESS)
            {
                outMesh.NormalPath = ResolveTextureReference(scene, model.EmbeddedTextures, directory, texPath);
            }

            // glTFのmetallicRoughnessテクスチャはG=ラフネス、B=メタリックを1枚に格納しており、
            // assimpはこれをROUGHNESS/METALNESSの両方のテクスチャタイプとして同じ画像を指す
            //
            // --specular-as-orm指定時は、どちらも無い場合にaiTextureType_SPECULARも見る。
            // FBXのSpecularColorスロットへORM(R=遮蔽/G=ラフネス/B=メタリック)を入れる規約の
            // アセット用(MaterialOverride::SpecularAsOrmのコメント参照)。チャンネルの割り当ては
            // glTFのmetallicRoughness(G=ラフネス/B=メタリック)と一致するため、
            // GBuffer.hlslのサンプリングはそのままでよい
            if (material->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &texPath) == AI_SUCCESS ||
                material->GetTexture(aiTextureType_METALNESS, 0, &texPath) == AI_SUCCESS ||
                (materialOverride.SpecularAsOrm &&
                 material->GetTexture(aiTextureType_SPECULAR, 0, &texPath) == AI_SUCCESS))
            {
                outMesh.MetallicRoughnessPath = ResolveTextureReference(scene, model.EmbeddedTextures, directory, texPath);
            }

            if (material->GetTexture(aiTextureType_EMISSIVE, 0, &texPath) == AI_SUCCESS)
            {
                outMesh.EmissivePath = ResolveTextureReference(scene, model.EmbeddedTextures, directory, texPath);
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
                outMesh.OcclusionPath = ResolveTextureReference(scene, model.EmbeddedTextures, directory, texPath);
            }
            else if (materialOverride.SpecularAsOrm && !outMesh.MetallicRoughnessPath.empty())
            {
                // ORMのRチャンネルは遮蔽なので、同じ画像を遮蔽スロットとしても使う。
                //
                // 【ktexは増えない】PackageWriterのテクスチャ要求は「解決済みパス|sRGBの要否」を
                // キーに重複排除するため、metallicRoughnessと同じパス・同じlinear指定のこの要求は
                // 同一エントリに畳まれる。
                //
                // 【UV1で引かれても正しい】遮蔽マップはシェーダー側で常にUV1(TEXCOORD1)から引くが、
                // --bake-occlusionを行わない場合UV1にはUV0が複製される(Assets/Vertex.hのコメント)。
                // ORMはUV0の空間で作られているため、これで意図どおりの位置が引ける
                outMesh.OcclusionPath = outMesh.MetallicRoughnessPath;
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

            // --alpha-cutout <マテリアル名>=<しきい値>。上のalphaMode判定はglTF専用で、
            // FBX/OBJには対応する情報が無い。SpeedTreeの葉のように「BaseColorのアルファで抜く」
            // 前提で作られたマテリアルは、指定しないと不透明な板として描かれる
            // (Bistro(OBJ)の葉で実際に起きている既知の破綻と同じもの)
            if (!materialOverride.AlphaCutoff.empty())
            {
                aiString materialName;
                if (material->Get(AI_MATKEY_NAME, materialName) == AI_SUCCESS)
                {
                    const auto found = materialOverride.AlphaCutoff.find(materialName.C_Str());
                    if (found != materialOverride.AlphaCutoff.end())
                    {
                        outMesh.AlphaCutoff = found->second;
                        // カットアウトと半透明は排他(glTFのalphaModeがOPAQUE/MASK/BLENDの
                        // いずれか1つであるのと同じ)。明示的にカットアウトを指定した以上、
                        // 後段のOPACITY/Tf由来の半透明判定に横取りされないようにする
                        outMesh.IsTransparent = false;
                    }
                }
            }

            // 透過率(葉・花弁のような薄いものが裏からの光を透かす量)。
            // glTFにこれを表す標準のプロパティが無いため、--translucent <マテリアル名>=<値> で
            // 外から与える。名前が一致したマテリアルにだけ設定する
            if (!materialOverride.Translucency.empty())
            {
                aiString materialName;
                if (material->Get(AI_MATKEY_NAME, materialName) == AI_SUCCESS)
                {
                    const auto found = materialOverride.Translucency.find(materialName.C_Str());
                    if (found != materialOverride.Translucency.end())
                    {
                        outMesh.Translucency = found->second;
                    }
                }
            }

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
        timings.MaterialSeconds += PhaseSecondsSince(materialStart);

        if (outTimings)
        {
            *outTimings = timings;
        }
        return model;
    }

    namespace
    {
        // aiTextureTypeの列挙と表示名。マテリアルが「どのスロットに」テクスチャを持っているかを
        // 網羅して出すためのもの。LoadSourceModelが実際に読むのはBASE_COLOR/DIFFUSE、
        // NORMALS/HEIGHT、DIFFUSE_ROUGHNESS/METALNESS、EMISSIVE、LIGHTMAP/AMBIENT_OCCLUSIONの
        // 5系統だけなので、それ以外(SPECULAR等)に入っているものは取りこぼされている
        struct TextureTypeName
        {
            aiTextureType Type;
            const char* Name;
        };

        const TextureTypeName kTextureTypeNames[] = {
            { aiTextureType_DIFFUSE,            "DIFFUSE" },
            { aiTextureType_SPECULAR,           "SPECULAR" },
            { aiTextureType_AMBIENT,            "AMBIENT" },
            { aiTextureType_EMISSIVE,           "EMISSIVE" },
            { aiTextureType_HEIGHT,             "HEIGHT" },
            { aiTextureType_NORMALS,            "NORMALS" },
            { aiTextureType_SHININESS,          "SHININESS" },
            { aiTextureType_OPACITY,            "OPACITY" },
            { aiTextureType_DISPLACEMENT,       "DISPLACEMENT" },
            { aiTextureType_LIGHTMAP,           "LIGHTMAP" },
            { aiTextureType_REFLECTION,         "REFLECTION" },
            { aiTextureType_BASE_COLOR,         "BASE_COLOR" },
            { aiTextureType_NORMAL_CAMERA,      "NORMAL_CAMERA" },
            { aiTextureType_EMISSION_COLOR,     "EMISSION_COLOR" },
            { aiTextureType_METALNESS,          "METALNESS" },
            { aiTextureType_DIFFUSE_ROUGHNESS,  "DIFFUSE_ROUGHNESS" },
            { aiTextureType_AMBIENT_OCCLUSION,  "AMBIENT_OCCLUSION" },
            { aiTextureType_UNKNOWN,            "UNKNOWN" },
        };

        // LoadSourceModelが実際に読むスロットかどうか。falseなら「入っているのに使われない」
        bool IsSlotConsumedByPacker(aiTextureType type)
        {
            switch (type)
            {
            case aiTextureType_BASE_COLOR:
            case aiTextureType_DIFFUSE:
            case aiTextureType_NORMALS:
            case aiTextureType_HEIGHT:
            case aiTextureType_DIFFUSE_ROUGHNESS:
            case aiTextureType_METALNESS:
            case aiTextureType_EMISSIVE:
            case aiTextureType_LIGHTMAP:
            case aiTextureType_AMBIENT_OCCLUSION:
                return true;
            default:
                return false;
            }
        }

        unsigned int CountNodes(const aiNode* node)
        {
            unsigned int count = 1;
            for (unsigned int i = 0; i < node->mNumChildren; ++i)
            {
                count += CountNodes(node->mChildren[i]);
            }
            return count;
        }

        // aiMetadataの1エントリを型に応じて文字列化する
        std::string FormatMetadataEntry(const aiMetadata* metadata, unsigned int index)
        {
            switch (metadata->mValues[index].mType)
            {
            case AI_BOOL:     return *static_cast<bool*>(metadata->mValues[index].mData) ? "true" : "false";
            case AI_INT32:    return std::to_string(*static_cast<int32_t*>(metadata->mValues[index].mData));
            case AI_UINT64:   return std::to_string(*static_cast<uint64_t*>(metadata->mValues[index].mData));
            case AI_FLOAT:    return std::to_string(*static_cast<float*>(metadata->mValues[index].mData));
            case AI_DOUBLE:   return std::to_string(*static_cast<double*>(metadata->mValues[index].mData));
            case AI_AISTRING: return static_cast<aiString*>(metadata->mValues[index].mData)->C_Str();
            default:          return "(未対応の型)";
            }
        }

    }

    // ピークワーキングセット(MB)。巨大なFBXが「読めるが遅い」のか「メモリを食い潰す」のかを
    // 切り分けるために出す。K32版を直接呼ぶことでpsapi.libへのリンクを増やさない
    double GetPeakWorkingSetMB()
    {
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        {
            Kurenai::Core::Logger::Warning("ModelSource", "プロセスのメモリ情報を取得できませんでした");
            return 0.0;
        }
        return static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
    }

    // プロセスが消費したCPU時間(カーネル+ユーザー、全スレッドの合計)。
    //
    // 【実時間で割った値を必ず見る】これがワーカー数を超えていたら、呼んでいるライブラリが
    // 内部で自前に並列化しているという意味で、外側にスレッドプールを足してはいけない。
    // 過去にDirectXTexのOpenMPと外側8ワーカーが掛かって8x28=224スレッドになり、
    // 機械が固まったことがある。数を上限で抑える前に、まずこの比を測ること
    double GetProcessCpuSeconds()
    {
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        {
            Kurenai::Core::Logger::Warning("ModelSource", "プロセスのCPU時間を取得できませんでした");
            return 0.0;
        }
        // FILETIMEは100ナノ秒単位。ULARGE_INTEGER経由で64bitへ組み直す
        ULARGE_INTEGER k{}, u{};
        k.LowPart = kernel.dwLowDateTime;   k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime;     u.HighPart = user.dwHighDateTime;
        return static_cast<double>(k.QuadPart + u.QuadPart) * 1e-7;
    }

    void InspectModel(const std::wstring& filePath, float scale)
    {
        Assimp::Importer importer;

        // LoadSourceModelと完全に同じポストプロセスで読む。ここで違う設定を使うと
        // 「inspectでは正しく見えたのにパックすると違う」という最悪の食い違いが起きる
        const auto readStart = std::chrono::steady_clock::now();
        const aiScene* scene = importer.ReadFile(
            WideToUtf8(filePath),
            aiProcess_Triangulate | aiProcess_ConvertToLeftHanded | aiProcess_JoinIdenticalVertices);
        const auto readEnd = std::chrono::steady_clock::now();

        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
        {
            throw std::runtime_error(std::string("モデルの読み込みに失敗しました: ") + importer.GetErrorString());
        }

        const double readMs = std::chrono::duration<double, std::milli>(readEnd - readStart).count();

        std::cout << std::fixed;
        std::cout << "[KurenaiPacker][inspect] " << WideToUtf8(filePath) << "\n";
        std::cout << "  読み込み: " << std::setprecision(0) << readMs << "ms"
                  << " / ピークワーキングセット " << std::setprecision(1) << GetPeakWorkingSetMB() << "MB\n";

        // --- シーン全体の規模 ---
        std::cout << "  規模: ノード " << CountNodes(scene->mRootNode)
                  << " / メッシュ " << scene->mNumMeshes
                  << " / マテリアル " << scene->mNumMaterials
                  << " / 埋め込みテクスチャ " << scene->mNumTextures
                  << " / ライト " << scene->mNumLights
                  << " / カメラ " << scene->mNumCameras
                  << " / アニメーション " << scene->mNumAnimations << "\n";

        // --- 頂点属性の有無 ---
        // 【法線を持たないメッシュがあると陰影が出ない】読み込み側は法線が無ければ
        // (0,1,0) で埋めるため、垂直な壁も上向きとして陰影計算され、面の向きによる
        // 明暗が一切出なくなる。落ち影は法線と無関係に出るので絵は「それらしく」見え、
        // 気づきにくい。ここで元データの時点での有無を確かめられるようにしておく
        unsigned int meshesWithNormals = 0;
        unsigned int meshesWithUV = 0;
        unsigned int meshesWithTangents = 0;
        for (unsigned int i = 0; i < scene->mNumMeshes; ++i)
        {
            const aiMesh* mesh = scene->mMeshes[i];
            if (mesh == nullptr)
            {
                continue;
            }
            if (mesh->HasNormals())
            {
                ++meshesWithNormals;
            }
            if (mesh->HasTextureCoords(0))
            {
                ++meshesWithUV;
            }
            if (mesh->HasTangentsAndBitangents())
            {
                ++meshesWithTangents;
            }
        }
        std::cout << "  頂点属性を持つメッシュ数: 法線 " << meshesWithNormals
                  << " / UV " << meshesWithUV
                  << " / 接線 " << meshesWithTangents
                  << "  (メッシュ総数 " << scene->mNumMeshes << ")\n";
        if (scene->mNumMeshes > 0 && meshesWithNormals == 0)
        {
            std::cout << "  【警告】法線を持つメッシュが1つも無い。このまま焼くと全頂点の法線が\n"
                         "          (0,1,0) になり、面の向きによる明暗(陰)が出なくなる\n";
        }

        // --- メタデータ(FBXのUnitScaleFactor/UpAxis等) ---
        // 単位系と上方向軸はここにしか出ない。--scaleの値を決める一次情報になる
        if (scene->mMetaData && scene->mMetaData->mNumProperties > 0)
        {
            std::cout << "  メタデータ:\n";
            for (unsigned int i = 0; i < scene->mMetaData->mNumProperties; ++i)
            {
                std::cout << "    " << scene->mMetaData->mKeys[i].C_Str()
                          << " = " << FormatMetadataEntry(scene->mMetaData, i) << "\n";
            }
        }
        else
        {
            std::cout << "  メタデータ: なし\n";
        }

        // --- ルートノードの変換行列 ---
        // assimpのFBXインポータは、FBXのUpAxis/FrontAxis/CoordAxisによる軸変換行列に
        // UnitScaleFactorを掛けてここへ後乗算する(FBXConverter.cpp correctRootTransform)。
        // つまり「対角に100が入っている」なら、頂点は読み込んだ時点で既に100倍されている
        const aiMatrix4x4& root = scene->mRootNode->mTransformation;
        std::cout << std::setprecision(6);
        std::cout << "  ルート変換行列(この対角に単位換算の倍率が入る):\n";
        std::cout << "    [" << root.a1 << ", " << root.a2 << ", " << root.a3 << ", " << root.a4 << "]\n";
        std::cout << "    [" << root.b1 << ", " << root.b2 << ", " << root.b3 << ", " << root.b4 << "]\n";
        std::cout << "    [" << root.c1 << ", " << root.c2 << ", " << root.c3 << ", " << root.c4 << "]\n";
        std::cout << "    [" << root.d1 << ", " << root.d2 << ", " << root.d3 << ", " << root.d4 << "]\n";

        // --- ノード変換を適用した全体バウンズ(LoadSourceModelと同じ経路) ---
        std::vector<std::pair<const aiMesh*, aiMatrix4x4>> meshNodes;
        CollectMeshNodes(scene, scene->mRootNode, aiMatrix4x4(), meshNodes);

        float boundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float boundsMax[3] = { 0.0f, 0.0f, 0.0f };
        bool boundsInitialized = false;
        // メッシュがノード階層から何回参照されているか(インスタンス化の倍率)
        std::unordered_map<const aiMesh*, unsigned int> referenceCount;
        uint64_t totalInstancedVertices = 0;
        uint64_t totalInstancedTriangles = 0;

        for (const auto& [mesh, transform] : meshNodes)
        {
            ++referenceCount[mesh];
            totalInstancedVertices += mesh->mNumVertices;
            totalInstancedTriangles += mesh->mNumFaces;

            if (!mesh->HasPositions())
            {
                continue;
            }
            for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
            {
                const aiVector3D position = (transform * mesh->mVertices[v]) * scale;
                const float p[3] = { position.x, position.y, position.z };
                for (int axis = 0; axis < 3; ++axis)
                {
                    if (!boundsInitialized)
                    {
                        boundsMin[axis] = p[axis];
                        boundsMax[axis] = p[axis];
                    }
                    else
                    {
                        boundsMin[axis] = (std::min)(boundsMin[axis], p[axis]);
                        boundsMax[axis] = (std::max)(boundsMax[axis], p[axis]);
                    }
                }
                boundsInitialized = true;
            }
        }

        uint64_t uniqueVertices = 0;
        uint64_t uniqueTriangles = 0;
        for (unsigned int i = 0; i < scene->mNumMeshes; ++i)
        {
            uniqueVertices += scene->mMeshes[i]->mNumVertices;
            uniqueTriangles += scene->mMeshes[i]->mNumFaces;
        }

        std::cout << std::setprecision(3);
        std::cout << "  頂点: ユニーク " << uniqueVertices
                  << " / インスタンス展開後 " << totalInstancedVertices << "\n";
        std::cout << "  三角形: ユニーク " << uniqueTriangles
                  << " / インスタンス展開後 " << totalInstancedTriangles << "\n";

        if (boundsInitialized)
        {
            const float sizeX = boundsMax[0] - boundsMin[0];
            const float sizeY = boundsMax[1] - boundsMin[1];
            const float sizeZ = boundsMax[2] - boundsMin[2];
            const float diagonal = std::sqrt(sizeX * sizeX + sizeY * sizeY + sizeZ * sizeZ);

            std::cout << "  バウンズ(--scale " << scale << " 適用後):\n";
            std::cout << "    min = (" << boundsMin[0] << ", " << boundsMin[1] << ", " << boundsMin[2] << ")\n";
            std::cout << "    max = (" << boundsMax[0] << ", " << boundsMax[1] << ", " << boundsMax[2] << ")\n";
            std::cout << "    大きさ = (" << sizeX << ", " << sizeY << ", " << sizeZ << ")"
                      << " / 対角 " << diagonal << "\n";
            // エンジンはシーンAABBの対角から遠クリップ面を自動決定する(farZ = max(100, 対角*4))。
            // スケールを間違えるとここが桁で狂い、カスケードシャドウが近景で無効化される
            std::cout << "    このモデル単体なら farZ = " << (std::max)(100.0f, diagonal * 4.0f) << "\n";
        }
        else
        {
            std::cout << "  バウンズ: 頂点が1つも無い\n";
        }

        // --- 頂点数の多いメッシュ上位10件 ---
        // 「1メッシュだけが極端に重い」形になっていないかを見る。カリングやLODの
        // 効き方が変わるため、総頂点数だけでは判断できない
        std::vector<unsigned int> meshOrder(scene->mNumMeshes);
        for (unsigned int i = 0; i < scene->mNumMeshes; ++i)
        {
            meshOrder[i] = i;
        }
        std::sort(meshOrder.begin(), meshOrder.end(), [scene](unsigned int a, unsigned int b) {
            return scene->mMeshes[a]->mNumVertices > scene->mMeshes[b]->mNumVertices;
        });

        const unsigned int topCount = (std::min)(10u, scene->mNumMeshes);
        if (topCount > 0)
        {
            std::cout << "  頂点数の多いメッシュ上位" << topCount << "件:\n";
            for (unsigned int i = 0; i < topCount; ++i)
            {
                const aiMesh* mesh = scene->mMeshes[meshOrder[i]];
                const auto refIt = referenceCount.find(mesh);
                const unsigned int refs = refIt == referenceCount.end() ? 0 : refIt->second;
                const double share = uniqueVertices > 0
                    ? 100.0 * static_cast<double>(mesh->mNumVertices) / static_cast<double>(uniqueVertices)
                    : 0.0;
                std::cout << "    [" << meshOrder[i] << "] 頂点 " << mesh->mNumVertices
                          << " (" << std::setprecision(1) << share << "%)"
                          << " / 三角形 " << mesh->mNumFaces
                          << " / 参照 " << refs << "回"
                          << " / マテリアル " << mesh->mMaterialIndex
                          << " / 名前 \"" << mesh->mName.C_Str() << "\"\n";
                std::cout << std::setprecision(3);
            }
        }

        // --- 埋め込みテクスチャ ---
        // mHeight==0 なら圧縮ブロブ(JPEG/PNG等がそのまま入っている)、非0なら生RGBA。
        // KurenaiPackerは現状これを読まないため、埋め込みのみのモデルはテクスチャが落ちる
        if (scene->mNumTextures > 0)
        {
            const unsigned int showCount = (std::min)(10u, scene->mNumTextures);
            std::cout << "  埋め込みテクスチャ(先頭" << showCount << "件 / 全" << scene->mNumTextures << "件):\n";
            for (unsigned int i = 0; i < showCount; ++i)
            {
                const aiTexture* texture = scene->mTextures[i];
                std::cout << "    [" << i << "] 形式ヒント \"" << texture->achFormatHint << "\""
                          << " / " << (texture->mHeight == 0
                                        ? ("圧縮ブロブ " + std::to_string(texture->mWidth) + "バイト")
                                        : ("生RGBA " + std::to_string(texture->mWidth) + "x" + std::to_string(texture->mHeight)))
                          << " / 名前 \"" << texture->mFilename.C_Str() << "\"\n";
            }
        }

        // --- マテリアルごとのテクスチャスロット ---
        // ここが「ORMがSPECULARに入っていてパッカーに読まれない」のような食い違いを
        // 見つける唯一の場所。パッカーが読まないスロットには [パッカー未使用] を付ける
        std::cout << "  マテリアル(" << scene->mNumMaterials << "件):\n";
        for (unsigned int m = 0; m < scene->mNumMaterials; ++m)
        {
            const aiMaterial* material = scene->mMaterials[m];
            aiString materialName;
            material->Get(AI_MATKEY_NAME, materialName);
            std::cout << "    [" << m << "] \"" << materialName.C_Str() << "\"";

            bool anySlot = false;
            for (const TextureTypeName& entry : kTextureTypeNames)
            {
                const unsigned int count = material->GetTextureCount(entry.Type);
                for (unsigned int t = 0; t < count; ++t)
                {
                    aiString texPath;
                    if (material->GetTexture(entry.Type, t, &texPath) != AI_SUCCESS)
                    {
                        continue;
                    }
                    if (!anySlot)
                    {
                        std::cout << "\n";
                        anySlot = true;
                    }
                    std::cout << "        " << entry.Name;
                    if (!IsSlotConsumedByPacker(entry.Type))
                    {
                        std::cout << " [パッカー未使用]";
                    }
                    std::cout << " -> " << texPath.C_Str() << "\n";
                }
            }
            if (!anySlot)
            {
                std::cout << " (テクスチャ無し)\n";
            }
        }

        std::cout << "  ピークワーキングセット(最終): " << std::setprecision(1) << GetPeakWorkingSetMB() << "MB\n";
    }
}
