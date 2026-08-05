#include "SceneLoader.h"

#include <Windows.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "Core/Logger.h"
#include "Core/StringUtil.h"
#include "ModelLoader.h"
#include "ModelPackage.h"
#include "ShowLoader.h"

namespace Kurenai::Assets
{
    namespace
    {
        using Core::Utf8ToWide;
        using Core::WideToUtf8;

        bool IsHalfWidthSpace(wchar_t c)
        {
            return c == L' ' || c == L'\t';
        }

        std::wstring TrimHalfWidth(const std::wstring& s)
        {
            size_t begin = 0;
            while (begin < s.size() && IsHalfWidthSpace(s[begin])) ++begin;
            size_t end = s.size();
            while (end > begin && IsHalfWidthSpace(s[end - 1])) --end;
            return s.substr(begin, end - begin);
        }

        // '#'は行頭、または直前が半角空白/タブの場合のみコメント開始とみなす。
        // 通常の文字に直接続く'#'(例: "Textures/#1/foo.kmodel")は文字通り扱う
        std::wstring StripComment(const std::wstring& rawLine)
        {
            for (size_t i = 0; i < rawLine.size(); ++i)
            {
                if (rawLine[i] == L'#' && (i == 0 || IsHalfWidthSpace(rawLine[i - 1])))
                {
                    return rawLine.substr(0, i);
                }
            }
            return rawLine;
        }

        bool CaseInsensitiveEquals(const std::wstring& a, const std::wstring& b)
        {
            return _wcsicmp(a.c_str(), b.c_str()) == 0;
        }

        // 値全体が"で囲まれている場合は引用符を剥がし、中身は一切トリムしない。
        // それ以外は半角空白/タブのみトリムした結果をそのまま返す
        std::wstring ExtractValue(const std::wstring& rawValue)
        {
            const std::wstring trimmed = TrimHalfWidth(rawValue);
            if (trimmed.size() >= 2 && trimmed.front() == L'"' && trimmed.back() == L'"')
            {
                return trimmed.substr(1, trimmed.size() - 2);
            }
            return trimmed;
        }

        bool ParseFloatToken(const std::wstring& token, float& out)
        {
            if (token.empty())
            {
                return false;
            }
            wchar_t* endPtr = nullptr;
            const float value = std::wcstof(token.c_str(), &endPtr);
            if (endPtr != token.c_str() + token.size())
            {
                return false;
            }
            out = value;
            return true;
        }

        bool ParseFloat3(const std::wstring& value, float outXYZ[3])
        {
            std::vector<std::wstring> tokens;
            size_t start = 0;
            while (start <= value.size())
            {
                const size_t comma = value.find(L',', start);
                const size_t end = comma == std::wstring::npos ? value.size() : comma;
                tokens.push_back(TrimHalfWidth(value.substr(start, end - start)));
                if (comma == std::wstring::npos)
                {
                    break;
                }
                start = comma + 1;
            }
            if (tokens.size() != 3)
            {
                return false;
            }
            for (int i = 0; i < 3; ++i)
            {
                if (!ParseFloatToken(tokens[i], outXYZ[i]))
                {
                    return false;
                }
            }
            return true;
        }

        // "13, 5, 7"のような3要素の正整数([GIVolume]のProbeCounts用)。
        // 分割処理を二重に持たないようParseFloat3で数値として読んでから整数性を検証する。
        // 0・負数・小数はここで弾くので、呼び出し側は「1以上の整数である」ことを前提にしてよい
        bool ParseUint3(const std::wstring& value, uint32_t outXYZ[3])
        {
            float parsed[3] = {};
            if (!ParseFloat3(value, parsed))
            {
                return false;
            }
            for (int i = 0; i < 3; ++i)
            {
                if (!(parsed[i] >= 1.0f) || std::floor(parsed[i]) != parsed[i])
                {
                    return false;
                }
                outXYZ[i] = static_cast<uint32_t>(parsed[i]);
            }
            return true;
        }

        std::optional<bool> ParseBoolToken(const std::wstring& value)
        {
            if (CaseInsensitiveEquals(value, L"true")) return true;
            if (CaseInsensitiveEquals(value, L"false")) return false;
            return std::nullopt;
        }

        std::wstring NormalizePathSeparators(const std::wstring& path)
        {
            std::wstring result = path;
            std::replace(result.begin(), result.end(), L'\\', L'/');
            return result;
        }

        // '..'セグメントや絶対パス(ドライブレター/'/'始まり)を拒否する。Assetsルート外への
        // 脱出を防ぐため。normalizedPathは事前にNormalizePathSeparators済みであること
        bool IsPathEscaping(const std::wstring& normalizedPath)
        {
            if (normalizedPath.empty() || normalizedPath.front() == L'/')
            {
                return true;
            }
            if (normalizedPath.size() >= 2 && normalizedPath[1] == L':')
            {
                return true;
            }
            size_t start = 0;
            while (start <= normalizedPath.size())
            {
                const size_t slash = normalizedPath.find(L'/', start);
                const size_t end = slash == std::wstring::npos ? normalizedPath.size() : slash;
                if (normalizedPath.substr(start, end - start) == L"..")
                {
                    return true;
                }
                if (slash == std::wstring::npos)
                {
                    break;
                }
                start = slash + 1;
            }
            return false;
        }

        // [Scene]Skybox・[Water]NormalMapなど、Assetsルートからの相対パスを取るキーに共通の解決処理。
        // パス区切りの正規化→IsPathEscapingによるルート外脱出チェック→assetRootDirectoryとの結合を
        // まとめて行う。検証に失敗した場合はstd::runtime_error(フィールド名・元のパス・シーンファイル名
        // つき)を投げる。fieldLabelはエラーメッセージにそのまま出す表示名("[Scene]Skybox"等)
        std::wstring ResolveAssetRelativePath(
            const std::wstring& relativePath, const std::wstring& assetRootDirectory,
            const std::wstring& fieldLabel, const std::wstring& sceneFilePath)
        {
            const std::wstring normalizedPath = NormalizePathSeparators(relativePath);
            if (IsPathEscaping(normalizedPath))
            {
                throw std::runtime_error(
                    WideToUtf8(fieldLabel) + "がルート外を指しています(絶対パスまたは'..'は使用できません): " +
                    WideToUtf8(relativePath) + " (" + WideToUtf8(sceneFilePath) + ")");
            }
            return assetRootDirectory + normalizedPath;
        }

        // ==== パース結果の中間表現(モデルの実読み込み前) ====

        struct ParsedModelEntry
        {
            std::wstring Path;
            bool HasPath = false;
            float Translation[3] = { 0.0f, 0.0f, 0.0f };
            float RotationEulerDegrees[3] = { 0.0f, 0.0f, 0.0f };
            float Scale[3] = { 1.0f, 1.0f, 1.0f };
            // .kscene [Model]Water(水面マテリアル基盤)。trueならScene構築時に
            // ModelInstance::IsWaterへそのまま反映する
            bool Water = false;
        };

        struct ParsedLightEntry
        {
            bool HasType = false;
            LightType Type = LightType::Point;
            bool HasPosition = false;
            float Position[3] = { 0.0f, 0.0f, 0.0f };
            bool HasDirection = false;
            float Direction[3] = { 0.0f, -1.0f, 0.0f };
            float Color[3] = { 1.0f, 1.0f, 1.0f };
            float Intensity = 1.0f;
            float Range = 10.0f;
            float ConeAngleDegrees = 45.0f;
            // スクリーンスペースシャドウを落とすか。Assets::Light::CastShadowの既定値と揃える
            bool CastShadow = true;
        };

        struct ParsedReflectionProbeEntry
        {
            bool HasPosition = false;
            float Position[3] = { 0.0f, 0.0f, 0.0f };
            float Radius = 10.0f;
            ReflectionProbeShape Shape = ReflectionProbeShape::Sphere;
            float BoxExtents[3] = { 10.0f, 10.0f, 10.0f };
            float YawDegrees = 0.0f;
            float BlendDistance = 2.0f;
            std::wstring Name;
        };

        struct ParsedGIVolumeEntry
        {
            bool HasOrigin = false;
            bool HasProbeCounts = false;
            float Origin[3] = { 0.0f, 0.0f, 0.0f };
            float ProbeSpacing[3] = { 2.0f, 2.0f, 2.0f };
            uint32_t ProbeCounts[3] = { 8u, 4u, 8u };
            float NormalBias = 0.25f;
            float ViewBias = 0.10f;
            float Hysteresis = 0.97f;
            float MaxRayDistance = 8.0f;
            std::wstring Name;
        };

        struct ParsedScene
        {
            std::wstring Name;
            bool HasName = false;

            std::vector<ParsedModelEntry> Models;

            bool HasCamera = false;
            bool CameraPositionSet = false;
            float CameraPosition[3] = { 0.0f, 0.0f, 0.0f };
            float CameraYaw = 0.0f;
            float CameraPitch = 0.0f;

            bool HasSun = false;
            float SunTimeOfDay = 12.0f;
            float SunAzimuthDegrees = 126.87f;
            bool SunShadow = true;
            bool SunEnabled = true;

            std::wstring SkyboxPath;
            bool HasIBLIntensity = false;
            float IBLIntensity = 1.0f;
            bool AOEnabled = true;
            bool HasSSREnabled = false;
            bool SSREnabled = true;
            Scene::TonemapCurveSetting Tonemap = Scene::TonemapCurveSetting::AgX;
            float SkySaturation = 1.0f;
            bool HasExposure = false;
            float ExposureEV100 = 15.0f;

            // [Water]セクション(水面マテリアル基盤)。NormalMapは[Scene]Skyboxと同じく
            // Assetsルートからの相対パスで、LoadScene側でルート外チェックのうえ絶対パスへ解決する。
            // 空文字列のままなら「法線マップ無しのフラット水面」を意味する
            std::wstring WaterNormalMapPath;
            float WaterWaveScale = 12.0f;
            float WaterWaveSpeed = 0.03f;
            float WaterWaveStrength = 0.25f;

            // [Cloud]セクション。指定されたキーだけエンジンの設定を上書きする
            bool HasCloudCoverage = false;   float CloudCoverage = 0.40f;
            bool HasCloudAltitude = false;   float CloudAltitude = 1500.0f;
            bool HasCloudThickness = false;  float CloudThickness = 400.0f;
            bool HasCloudDensity = false;    float CloudDensity = 8.0f;
            bool HasCloudCellSize = false;   float CloudCellSize = 1000.0f;
            bool HasCirrusCoverage = false;  float CirrusCoverage = 0.5f;

            // [Fog]セクション。指定されたキーだけエンジンの設定を上書きする
            bool HasFogEnabled = false;      bool  FogEnabled = true;
            bool HasFogDensity = false;      float FogDensity = 0.0004f;
            bool HasFogScaleHeight = false;  float FogScaleHeight = 1000.0f;
            bool HasFogRefHeight = false;    float FogRefHeight = 0.0f;

            // [Bloom]セクション。指定されたキーだけエンジンの設定を上書きする
            bool HasBloomEnabled = false;    bool  BloomEnabled = false;
            bool HasBloomStrength = false;   float BloomStrength = 0.06f;
            bool HasBloomThreshold = false;  float BloomThreshold = 1.0f;

            // [Stars]セクション。指定されたキーだけエンジンの設定を上書きする
            bool HasStarsEnabled = false;    bool  StarsEnabled = true;
            bool HasStarsDensity = false;    float StarsDensity = 48.0f;
            bool HasStarsBrightness = false; float StarsBrightness = 1.0f;
            bool HasStarsTwinkle = false;    float StarsTwinkle = 0.0f;

            // [DroneShow]セクション。指定されたキーだけエンジンの設定を上書きする。
            // ショーの中身(機体数・秒数・明るさ等)はここではなく.kshowが持つ
            bool HasDroneShowEnabled = false;        bool  DroneShowEnabled = false;
            // .kshowへのパス(Assetsルートからの相対)。解決はLoadScene側
            std::wstring DroneShowPath;
            bool HasDroneShowCenter = false;         float DroneShowCenter[3] = { 0.0f, 220.0f, 260.0f };
            bool HasDroneShowScale = false;          float DroneShowScale = 130.0f;

            std::vector<ParsedLightEntry> Lights;
            std::vector<ParsedReflectionProbeEntry> ReflectionProbes;
            std::vector<ParsedGIVolumeEntry> GIVolumes;
        };

        enum class Section
        {
            None,
            Scene,
            Model,
            Camera,
            Sun,
            Light,
            ReflectionProbe,
            GIVolume,
            Water,
            Cloud,
            Fog,
            Bloom,
            Stars,
            DroneShow,
            Unknown,
        };

        Section SectionFromName(const std::wstring& name)
        {
            if (CaseInsensitiveEquals(name, L"Scene")) return Section::Scene;
            if (CaseInsensitiveEquals(name, L"Model")) return Section::Model;
            if (CaseInsensitiveEquals(name, L"Camera")) return Section::Camera;
            if (CaseInsensitiveEquals(name, L"Sun")) return Section::Sun;
            if (CaseInsensitiveEquals(name, L"Light")) return Section::Light;
            if (CaseInsensitiveEquals(name, L"ReflectionProbe")) return Section::ReflectionProbe;
            if (CaseInsensitiveEquals(name, L"GIVolume")) return Section::GIVolume;
            if (CaseInsensitiveEquals(name, L"Water")) return Section::Water;
            if (CaseInsensitiveEquals(name, L"Cloud")) return Section::Cloud;
            if (CaseInsensitiveEquals(name, L"Fog")) return Section::Fog;
            if (CaseInsensitiveEquals(name, L"Bloom")) return Section::Bloom;
            if (CaseInsensitiveEquals(name, L"Stars")) return Section::Stars;
            if (CaseInsensitiveEquals(name, L"DroneShow")) return Section::DroneShow;
            return Section::Unknown;
        }

        // .ksceneのテキストをパースする(モデルの実読み込みは行わない、純粋なテキスト解析)。
        // 失敗時はstd::runtime_error(ファイル名・行番号・該当行つき)を投げる
        ParsedScene ParseSceneFile(const std::wstring& filePath)
        {
            std::ifstream in(filePath, std::ios::binary);
            if (!in.is_open())
            {
                throw std::runtime_error("シーンファイルを開けませんでした: " + WideToUtf8(filePath));
            }
            std::ostringstream buffer;
            buffer << in.rdbuf();
            std::string content = buffer.str();

            // UTF-8 BOM(EF BB BF)を許容する
            if (content.size() >= 3 &&
                static_cast<unsigned char>(content[0]) == 0xEF &&
                static_cast<unsigned char>(content[1]) == 0xBB &&
                static_cast<unsigned char>(content[2]) == 0xBF)
            {
                content.erase(0, 3);
            }

            const std::wstring wideContent = Utf8ToWide(content);

            ParsedScene result;
            Section currentSection = Section::None;

            auto errorAt = [&](size_t lineNumber, const std::wstring& rawLine, const std::string& message) -> void
            {
                const std::string fullMessage =
                    message + " (" + WideToUtf8(filePath) + ":" + std::to_string(lineNumber) + ": " + WideToUtf8(rawLine) + ")";
                Core::Logger::Error("SceneLoader", fullMessage);
                throw std::runtime_error(fullMessage);
            };

            size_t lineNumber = 0;
            size_t pos = 0;
            while (pos <= wideContent.size())
            {
                const size_t newlinePos = wideContent.find(L'\n', pos);
                const size_t lineEnd = newlinePos == std::wstring::npos ? wideContent.size() : newlinePos;
                std::wstring rawLine = wideContent.substr(pos, lineEnd - pos);
                if (!rawLine.empty() && rawLine.back() == L'\r')
                {
                    rawLine.pop_back();
                }
                ++lineNumber;

                const std::wstring stripped = StripComment(rawLine);
                const std::wstring trimmedForStructure = TrimHalfWidth(stripped);

                if (trimmedForStructure.empty())
                {
                    if (newlinePos == std::wstring::npos) break;
                    pos = newlinePos + 1;
                    continue;
                }

                if (trimmedForStructure.front() == L'[')
                {
                    const size_t closeBracket = trimmedForStructure.find(L']');
                    if (closeBracket == std::wstring::npos)
                    {
                        errorAt(lineNumber, rawLine, "セクション見出しの']'が見つかりません");
                    }
                    const std::wstring sectionName = TrimHalfWidth(trimmedForStructure.substr(1, closeBracket - 1));
                    currentSection = SectionFromName(sectionName);
                    if (currentSection == Section::Unknown)
                    {
                        Core::Logger::Warning("SceneLoader", "未知のセクションを無視します: [" + WideToUtf8(sectionName) + "] (" + WideToUtf8(filePath) + ":" + std::to_string(lineNumber) + ")");
                    }
                    else if (currentSection == Section::Model)
                    {
                        result.Models.emplace_back();
                    }
                    else if (currentSection == Section::Light)
                    {
                        result.Lights.emplace_back();
                    }
                    else if (currentSection == Section::ReflectionProbe)
                    {
                        result.ReflectionProbes.emplace_back();
                    }
                    else if (currentSection == Section::GIVolume)
                    {
                        result.GIVolumes.emplace_back();
                    }
                    else if (currentSection == Section::Camera)
                    {
                        result.HasCamera = true;
                    }
                    else if (currentSection == Section::Sun)
                    {
                        result.HasSun = true;
                    }

                    if (newlinePos == std::wstring::npos) break;
                    pos = newlinePos + 1;
                    continue;
                }

                const size_t equalsPos = stripped.find(L'=');
                if (equalsPos == std::wstring::npos)
                {
                    errorAt(lineNumber, rawLine, "'key = value'の形式でも'[Section]'の形式でもありません");
                }
                const std::wstring key = TrimHalfWidth(stripped.substr(0, equalsPos));
                const std::wstring value = ExtractValue(stripped.substr(equalsPos + 1));

                if (currentSection == Section::Unknown || currentSection == Section::None)
                {
                    if (currentSection == Section::None)
                    {
                        Core::Logger::Warning("SceneLoader", "セクション外のキーを無視します: " + WideToUtf8(key) + " (" + WideToUtf8(filePath) + ":" + std::to_string(lineNumber) + ")");
                    }
                    // Unknownセクション内は既にセクション単位で警告済みなのでキーごとの警告は出さない
                    if (newlinePos == std::wstring::npos) break;
                    pos = newlinePos + 1;
                    continue;
                }

                auto warnUnknownKey = [&]()
                {
                    Core::Logger::Warning("SceneLoader", "未知のキーを無視します: " + WideToUtf8(key) + " (" + WideToUtf8(filePath) + ":" + std::to_string(lineNumber) + ")");
                };

                switch (currentSection)
                {
                case Section::Scene:
                    if (CaseInsensitiveEquals(key, L"Name"))
                    {
                        result.Name = value;
                        result.HasName = true;
                    }
                    else if (CaseInsensitiveEquals(key, L"Skybox"))
                    {
                        // スカイボックス(キューブマップDDS)のAssetsルートからの相対パス。
                        // [Model]Pathと同じ基準・同じルート外チェックを適用する
                        result.SkyboxPath = value;
                    }
                    else if (CaseInsensitiveEquals(key, L"IBLIntensity"))
                    {
                        if (!ParseFloatToken(value, result.IBLIntensity)) errorAt(lineNumber, rawLine, "IBLIntensityの値が不正です");
                        if (result.IBLIntensity < 0.0f) errorAt(lineNumber, rawLine, "IBLIntensityは0以上で指定してください");
                        result.HasIBLIntensity = true;
                    }
                    else if (CaseInsensitiveEquals(key, L"AmbientOcclusion"))
                    {
                        const std::optional<bool> parsedValue = ParseBoolToken(value);
                        if (!parsedValue) errorAt(lineNumber, rawLine, "AmbientOcclusionの値はtrue/falseで指定してください");
                        result.AOEnabled = *parsedValue;
                    }
                    else if (CaseInsensitiveEquals(key, L"Tonemap"))
                    {
                        // トーンマップのカーブをシーン単位で選ぶ。屋外の風景はACESのほうが空の青が残る
                        // (既定のAgXはハイライトを色相保持のまま白へ脱色するため彩度が落ちる)
                        if (CaseInsensitiveEquals(value, L"Reinhard"))
                        {
                            result.Tonemap = Scene::TonemapCurveSetting::Reinhard;
                        }
                        else if (CaseInsensitiveEquals(value, L"ACES"))
                        {
                            result.Tonemap = Scene::TonemapCurveSetting::ACES;
                        }
                        else if (CaseInsensitiveEquals(value, L"AgX"))
                        {
                            result.Tonemap = Scene::TonemapCurveSetting::AgX;
                        }
                        else
                        {
                            errorAt(lineNumber, rawLine, "Tonemapの値はReinhard/ACES/AgXのいずれかで指定してください");
                        }
                    }
                    else if (CaseInsensitiveEquals(key, L"SkySaturation"))
                    {
                        if (!ParseFloatToken(value, result.SkySaturation)) errorAt(lineNumber, rawLine, "SkySaturationの値が不正です");
                        if (result.SkySaturation < 0.0f) errorAt(lineNumber, rawLine, "SkySaturationは0以上で指定してください");
                    }
                    else if (CaseInsensitiveEquals(key, L"Exposure"))
                    {
                        if (!ParseFloatToken(value, result.ExposureEV100)) errorAt(lineNumber, rawLine, "Exposureの値が不正です");
                        // EV100の実用域(暗い室内-6 〜 直射日光下17程度)を大きく外れた値は
                        // 打ち間違いとみなす。自動露出の範囲(EngineDefaults.hのAutoExposure
                        // Min/MaxEV100)と同じ-6〜18に合わせてある
                        if (result.ExposureEV100 < -6.0f || result.ExposureEV100 > 18.0f)
                        {
                            errorAt(lineNumber, rawLine, "Exposureは-6〜18(EV100)の範囲で指定してください");
                        }
                        result.HasExposure = true;
                    }
                    else if (CaseInsensitiveEquals(key, L"ScreenSpaceReflection"))
                    {
                        const std::optional<bool> parsedValue = ParseBoolToken(value);
                        if (!parsedValue) errorAt(lineNumber, rawLine, "ScreenSpaceReflectionの値はtrue/falseで指定してください");
                        result.SSREnabled = *parsedValue;
                        // 「書いた」ことそのものに意味がある(Scene::HasSSREnabledOverride参照)
                        result.HasSSREnabled = true;
                    }
                    else
                    {
                        warnUnknownKey();
                    }
                    break;

                case Section::Model:
                {
                    ParsedModelEntry& entry = result.Models.back();
                    if (CaseInsensitiveEquals(key, L"Path"))
                    {
                        entry.Path = value;
                        entry.HasPath = true;
                    }
                    else if (CaseInsensitiveEquals(key, L"Translation"))
                    {
                        if (!ParseFloat3(value, entry.Translation)) errorAt(lineNumber, rawLine, "Translationの値が不正です(x, y, zの3要素が必要)");
                    }
                    else if (CaseInsensitiveEquals(key, L"RotationEuler"))
                    {
                        if (!ParseFloat3(value, entry.RotationEulerDegrees)) errorAt(lineNumber, rawLine, "RotationEulerの値が不正です(x, y, zの3要素が必要)");
                    }
                    else if (CaseInsensitiveEquals(key, L"Scale"))
                    {
                        if (!ParseFloat3(value, entry.Scale)) errorAt(lineNumber, rawLine, "Scaleの値が不正です(x, y, zの3要素が必要)");
                    }
                    else if (CaseInsensitiveEquals(key, L"Water"))
                    {
                        // 水面マテリアル基盤。trueにするとこのインスタンスがWater.hlslで
                        // 描画され、G-BufferのMaterial.aへ水面のマテリアルIDが書かれるようになる
                        const std::optional<bool> parsed = ParseBoolToken(value);
                        if (!parsed) errorAt(lineNumber, rawLine, "Waterの値はtrue/falseで指定してください");
                        entry.Water = *parsed;
                    }
                    else
                    {
                        warnUnknownKey();
                    }
                    break;
                }

                case Section::Camera:
                    if (CaseInsensitiveEquals(key, L"Position"))
                    {
                        if (!ParseFloat3(value, result.CameraPosition)) errorAt(lineNumber, rawLine, "Positionの値が不正です(x, y, zの3要素が必要)");
                        result.CameraPositionSet = true;
                    }
                    else if (CaseInsensitiveEquals(key, L"Yaw"))
                    {
                        if (!ParseFloatToken(value, result.CameraYaw)) errorAt(lineNumber, rawLine, "Yawの値が不正です");
                    }
                    else if (CaseInsensitiveEquals(key, L"Pitch"))
                    {
                        if (!ParseFloatToken(value, result.CameraPitch)) errorAt(lineNumber, rawLine, "Pitchの値が不正です");
                    }
                    else
                    {
                        warnUnknownKey();
                    }
                    break;

                case Section::Sun:
                    if (CaseInsensitiveEquals(key, L"TimeOfDay"))
                    {
                        if (!ParseFloatToken(value, result.SunTimeOfDay)) errorAt(lineNumber, rawLine, "TimeOfDayの値が不正です");
                        if (result.SunTimeOfDay < 0.0f || result.SunTimeOfDay > 24.0f) errorAt(lineNumber, rawLine, "TimeOfDayは0〜24の範囲で指定してください");
                    }
                    else if (CaseInsensitiveEquals(key, L"AzimuthDegrees"))
                    {
                        if (!ParseFloatToken(value, result.SunAzimuthDegrees)) errorAt(lineNumber, rawLine, "AzimuthDegreesの値が不正です");
                    }
                    else if (CaseInsensitiveEquals(key, L"Shadow"))
                    {
                        const std::optional<bool> parsed = ParseBoolToken(value);
                        if (!parsed) errorAt(lineNumber, rawLine, "Shadowの値はtrue/falseで指定してください");
                        result.SunShadow = *parsed;
                    }
                    else if (CaseInsensitiveEquals(key, L"Enabled"))
                    {
                        // 太陽(平行光)そのものの有効/無効。TimeOfDayを夜にすると昼度も一緒に
                        // 落ちて環境光まで消えるため、「昼のまま太陽だけ消す」にはこちらを使う
                        const std::optional<bool> parsed = ParseBoolToken(value);
                        if (!parsed) errorAt(lineNumber, rawLine, "Enabledの値はtrue/falseで指定してください");
                        result.SunEnabled = *parsed;
                    }
                    else
                    {
                        warnUnknownKey();
                    }
                    break;

                case Section::Light:
                {
                    ParsedLightEntry& entry = result.Lights.back();
                    if (CaseInsensitiveEquals(key, L"Type"))
                    {
                        if (CaseInsensitiveEquals(value, L"Point")) entry.Type = LightType::Point;
                        else if (CaseInsensitiveEquals(value, L"Spot")) entry.Type = LightType::Spot;
                        else errorAt(lineNumber, rawLine, "Typeの値はPointかSpotで指定してください");
                        entry.HasType = true;
                    }
                    else if (CaseInsensitiveEquals(key, L"Position"))
                    {
                        if (!ParseFloat3(value, entry.Position)) errorAt(lineNumber, rawLine, "Positionの値が不正です(x, y, zの3要素が必要)");
                        entry.HasPosition = true;
                    }
                    else if (CaseInsensitiveEquals(key, L"Direction"))
                    {
                        if (!ParseFloat3(value, entry.Direction)) errorAt(lineNumber, rawLine, "Directionの値が不正です(x, y, zの3要素が必要)");
                        entry.HasDirection = true;
                    }
                    else if (CaseInsensitiveEquals(key, L"Color"))
                    {
                        if (!ParseFloat3(value, entry.Color)) errorAt(lineNumber, rawLine, "Colorの値が不正です(r, g, bの3要素が必要)");
                    }
                    else if (CaseInsensitiveEquals(key, L"Intensity"))
                    {
                        if (!ParseFloatToken(value, entry.Intensity)) errorAt(lineNumber, rawLine, "Intensityの値が不正です");
                    }
                    else if (CaseInsensitiveEquals(key, L"Range"))
                    {
                        if (!ParseFloatToken(value, entry.Range)) errorAt(lineNumber, rawLine, "Rangeの値が不正です");
                    }
                    else if (CaseInsensitiveEquals(key, L"ConeAngleDegrees"))
                    {
                        if (!ParseFloatToken(value, entry.ConeAngleDegrees)) errorAt(lineNumber, rawLine, "ConeAngleDegreesの値が不正です");
                    }
                    else if (CaseInsensitiveEquals(key, L"CastShadow"))
                    {
                        const std::optional<bool> parsed = ParseBoolToken(value);
                        if (!parsed) errorAt(lineNumber, rawLine, "CastShadowの値はtrue/falseで指定してください");
                        entry.CastShadow = *parsed;
                    }
                    else
                    {
                        warnUnknownKey();
                    }
                    break;
                }

                case Section::ReflectionProbe:
                {
                    ParsedReflectionProbeEntry& entry = result.ReflectionProbes.back();
                    if (CaseInsensitiveEquals(key, L"Position"))
                    {
                        if (!ParseFloat3(value, entry.Position)) errorAt(lineNumber, rawLine, "Positionの値が不正です(x, y, zの3要素が必要)");
                        entry.HasPosition = true;
                    }
                    else if (CaseInsensitiveEquals(key, L"Radius"))
                    {
                        if (!ParseFloatToken(value, entry.Radius)) errorAt(lineNumber, rawLine, "Radiusの値が不正です");
                        if (entry.Radius <= 0.0f) errorAt(lineNumber, rawLine, "Radiusは0より大きい値で指定してください");
                    }
                    else if (CaseInsensitiveEquals(key, L"Shape"))
                    {
                        if (CaseInsensitiveEquals(value, L"Sphere")) entry.Shape = ReflectionProbeShape::Sphere;
                        else if (CaseInsensitiveEquals(value, L"Box")) entry.Shape = ReflectionProbeShape::Box;
                        else errorAt(lineNumber, rawLine, "Shapeの値が不正です(SphereまたはBox)");
                    }
                    else if (CaseInsensitiveEquals(key, L"BoxExtents"))
                    {
                        if (!ParseFloat3(value, entry.BoxExtents)) errorAt(lineNumber, rawLine, "BoxExtentsの値が不正です(x, y, zの3要素が必要)");
                        if (entry.BoxExtents[0] <= 0.0f || entry.BoxExtents[1] <= 0.0f || entry.BoxExtents[2] <= 0.0f)
                        {
                            errorAt(lineNumber, rawLine, "BoxExtentsは全ての軸を0より大きい値で指定してください");
                        }
                    }
                    else if (CaseInsensitiveEquals(key, L"Yaw"))
                    {
                        if (!ParseFloatToken(value, entry.YawDegrees)) errorAt(lineNumber, rawLine, "Yawの値が不正です");
                    }
                    else if (CaseInsensitiveEquals(key, L"BlendDistance"))
                    {
                        if (!ParseFloatToken(value, entry.BlendDistance)) errorAt(lineNumber, rawLine, "BlendDistanceの値が不正です");
                        if (entry.BlendDistance < 0.0f) errorAt(lineNumber, rawLine, "BlendDistanceは0以上の値で指定してください");
                    }
                    else if (CaseInsensitiveEquals(key, L"Name"))
                    {
                        entry.Name = value;
                    }
                    else
                    {
                        warnUnknownKey();
                    }
                    break;
                }

                case Section::GIVolume:
                {
                    ParsedGIVolumeEntry& entry = result.GIVolumes.back();
                    if (CaseInsensitiveEquals(key, L"Origin"))
                    {
                        if (!ParseFloat3(value, entry.Origin)) errorAt(lineNumber, rawLine, "Originの値が不正です(x, y, zの3要素が必要)");
                        entry.HasOrigin = true;
                    }
                    else if (CaseInsensitiveEquals(key, L"ProbeSpacing"))
                    {
                        if (!ParseFloat3(value, entry.ProbeSpacing)) errorAt(lineNumber, rawLine, "ProbeSpacingの値が不正です(x, y, zの3要素が必要)");
                        if (entry.ProbeSpacing[0] <= 0.0f || entry.ProbeSpacing[1] <= 0.0f || entry.ProbeSpacing[2] <= 0.0f)
                        {
                            errorAt(lineNumber, rawLine, "ProbeSpacingは全ての軸を0より大きい値で指定してください");
                        }
                    }
                    else if (CaseInsensitiveEquals(key, L"ProbeCounts"))
                    {
                        if (!ParseUint3(value, entry.ProbeCounts))
                        {
                            errorAt(lineNumber, rawLine, "ProbeCountsの値が不正です(x, y, zの3要素、それぞれ1以上の整数)");
                        }
                        // トライリニア補間は周囲8個のプローブを使うため、各軸2個以上ないと成立しない
                        if (entry.ProbeCounts[0] < 2u || entry.ProbeCounts[1] < 2u || entry.ProbeCounts[2] < 2u)
                        {
                            errorAt(lineNumber, rawLine, "ProbeCountsは全ての軸を2以上で指定してください(トライリニア補間に周囲8個が必要なため)");
                        }
                        entry.HasProbeCounts = true;
                    }
                    else if (CaseInsensitiveEquals(key, L"NormalBias"))
                    {
                        if (!ParseFloatToken(value, entry.NormalBias)) errorAt(lineNumber, rawLine, "NormalBiasの値が不正です");
                        if (entry.NormalBias < 0.0f) errorAt(lineNumber, rawLine, "NormalBiasは0以上の値で指定してください");
                    }
                    else if (CaseInsensitiveEquals(key, L"ViewBias"))
                    {
                        if (!ParseFloatToken(value, entry.ViewBias)) errorAt(lineNumber, rawLine, "ViewBiasの値が不正です");
                        if (entry.ViewBias < 0.0f) errorAt(lineNumber, rawLine, "ViewBiasは0以上の値で指定してください");
                    }
                    else if (CaseInsensitiveEquals(key, L"Hysteresis"))
                    {
                        if (!ParseFloatToken(value, entry.Hysteresis)) errorAt(lineNumber, rawLine, "Hysteresisの値が不正です");
                        if (entry.Hysteresis < 0.0f || entry.Hysteresis >= 1.0f)
                        {
                            errorAt(lineNumber, rawLine, "Hysteresisは0以上1未満で指定してください(1では新しい値が一切入らない)");
                        }
                    }
                    else if (CaseInsensitiveEquals(key, L"MaxRayDistance"))
                    {
                        if (!ParseFloatToken(value, entry.MaxRayDistance)) errorAt(lineNumber, rawLine, "MaxRayDistanceの値が不正です");
                        // 距離アトラスは平均距離と平均二乗距離を持ち、その差から分散を求める。
                        // 距離が大きいほどこの引き算の桁落ちが効くため上限を設ける
                        // (r=200なら r²=40000 で、fp32の有効桁に対し分散を0.01程度の分解能で
                        //  残せる。詳細はScene.hのGIVolume::MaxRayDistance参照)
                        if (entry.MaxRayDistance <= 0.0f || entry.MaxRayDistance > 200.0f)
                        {
                            errorAt(lineNumber, rawLine, "MaxRayDistanceは0より大きく200以下で指定してください(分散の計算が桁落ちで潰れるため)");
                        }
                    }
                    else if (CaseInsensitiveEquals(key, L"Name"))
                    {
                        entry.Name = value;
                    }
                    else
                    {
                        warnUnknownKey();
                    }
                    break;
                }

                case Section::Water:
                    // 水面マテリアル基盤。NormalMapのパス解決(ルート外チェック・絶対パス化)は
                    // ここでは行わず、[Scene]Skyboxと同じくLoadScene側でまとめて行う
                    // (ParseSceneFileは純粋なテキスト解析でファイルシステムに触れない方針のため)
                    if (CaseInsensitiveEquals(key, L"NormalMap"))
                    {
                        result.WaterNormalMapPath = value;
                    }
                    else if (CaseInsensitiveEquals(key, L"WaveScale"))
                    {
                        if (!ParseFloatToken(value, result.WaterWaveScale)) errorAt(lineNumber, rawLine, "WaveScaleの値が不正です");
                    }
                    else if (CaseInsensitiveEquals(key, L"WaveSpeed"))
                    {
                        if (!ParseFloatToken(value, result.WaterWaveSpeed)) errorAt(lineNumber, rawLine, "WaveSpeedの値が不正です");
                    }
                    else if (CaseInsensitiveEquals(key, L"WaveStrength"))
                    {
                        if (!ParseFloatToken(value, result.WaterWaveStrength)) errorAt(lineNumber, rawLine, "WaveStrengthの値が不正です");
                    }
                    else
                    {
                        warnUnknownKey();
                    }
                    break;

                case Section::Cloud:
                {
                    // 数値1つを読んで範囲を確かめ、「指定された」印を立てるだけの処理が続くので
                    // ラムダにまとめる(範囲外は打ち間違いとみなしてエラーにする)
                    const auto readFloat = [&](float& out, bool& has, float minValue, float maxValue, const wchar_t* name)
                    {
                        if (!ParseFloatToken(value, out))
                        {
                            errorAt(lineNumber, rawLine, WideToUtf8(name) + "の値が不正です");
                        }
                        if (out < minValue || out > maxValue)
                        {
                            errorAt(lineNumber, rawLine, WideToUtf8(name) + "の値が範囲外です");
                        }
                        has = true;
                    };

                    if (CaseInsensitiveEquals(key, L"Coverage"))
                    {
                        readFloat(result.CloudCoverage, result.HasCloudCoverage, 0.0f, 1.0f, L"Coverage");
                    }
                    else if (CaseInsensitiveEquals(key, L"Altitude"))
                    {
                        readFloat(result.CloudAltitude, result.HasCloudAltitude, 100.0f, 20000.0f, L"Altitude");
                    }
                    else if (CaseInsensitiveEquals(key, L"Thickness"))
                    {
                        readFloat(result.CloudThickness, result.HasCloudThickness, 0.0f, 5000.0f, L"Thickness");
                    }
                    else if (CaseInsensitiveEquals(key, L"Density"))
                    {
                        readFloat(result.CloudDensity, result.HasCloudDensity, 0.0f, 100.0f, L"Density");
                    }
                    else if (CaseInsensitiveEquals(key, L"CellSize"))
                    {
                        readFloat(result.CloudCellSize, result.HasCloudCellSize, 10.0f, 100000.0f, L"CellSize");
                    }
                    else if (CaseInsensitiveEquals(key, L"CirrusCoverage"))
                    {
                        readFloat(result.CirrusCoverage, result.HasCirrusCoverage, 0.0f, 1.0f, L"CirrusCoverage");
                    }
                    else
                    {
                        warnUnknownKey();
                    }
                    break;
                }

                case Section::Fog:
                {
                    // [Cloud]と同じ作法。範囲外は打ち間違いとみなしてエラーにする
                    const auto readFloat = [&](float& out, bool& has, float minValue, float maxValue, const wchar_t* name)
                    {
                        if (!ParseFloatToken(value, out))
                        {
                            errorAt(lineNumber, rawLine, WideToUtf8(name) + "の値が不正です");
                        }
                        if (out < minValue || out > maxValue)
                        {
                            errorAt(lineNumber, rawLine, WideToUtf8(name) + "の値が範囲外です");
                        }
                        has = true;
                    };

                    if (CaseInsensitiveEquals(key, L"Enabled"))
                    {
                        const std::optional<bool> parsedValue = ParseBoolToken(value);
                        if (!parsedValue) errorAt(lineNumber, rawLine, "Enabledの値はtrue/falseで指定してください");
                        result.FogEnabled = *parsedValue;
                        result.HasFogEnabled = true;
                    }
                    else if (CaseInsensitiveEquals(key, L"Density"))
                    {
                        // 上限0.002は視程約2km(もや)に相当する。これより濃いと600m先の地物すら
                        // 見えなくなり屋外の風景として成立しないため、UIのスライダーと同じ上限にしてある
                        readFloat(result.FogDensity, result.HasFogDensity, 0.0f, 0.002f, L"Density");
                    }
                    else if (CaseInsensitiveEquals(key, L"ScaleHeight"))
                    {
                        readFloat(result.FogScaleHeight, result.HasFogScaleHeight, 10.0f, 5000.0f, L"ScaleHeight");
                    }
                    else if (CaseInsensitiveEquals(key, L"RefHeight"))
                    {
                        readFloat(result.FogRefHeight, result.HasFogRefHeight, -500.0f, 500.0f, L"RefHeight");
                    }
                    else
                    {
                        warnUnknownKey();
                    }
                    break;
                }

                case Section::Bloom:
                {
                    const auto readFloat = [&](float& out, bool& has, float minValue, float maxValue, const wchar_t* name)
                    {
                        if (!ParseFloatToken(value, out))
                        {
                            errorAt(lineNumber, rawLine, WideToUtf8(name) + "の値が不正です");
                        }
                        if (out < minValue || out > maxValue)
                        {
                            errorAt(lineNumber, rawLine, WideToUtf8(name) + "の値が範囲外です");
                        }
                        has = true;
                    };

                    if (CaseInsensitiveEquals(key, L"Enabled"))
                    {
                        const std::optional<bool> parsedValue = ParseBoolToken(value);
                        if (!parsedValue) errorAt(lineNumber, rawLine, "Enabledの値はtrue/falseで指定してください");
                        result.BloomEnabled = *parsedValue;
                        result.HasBloomEnabled = true;
                    }
                    else if (CaseInsensitiveEquals(key, L"Strength"))
                    {
                        // Tonemapは元の色とブルームをこの比率でlerpするため1.0で完全に置き換わる
                        readFloat(result.BloomStrength, result.HasBloomStrength, 0.0f, 1.0f, L"Strength");
                    }
                    else if (CaseInsensitiveEquals(key, L"Threshold"))
                    {
                        readFloat(result.BloomThreshold, result.HasBloomThreshold, 0.0f, 100.0f, L"Threshold");
                    }
                    else
                    {
                        warnUnknownKey();
                    }
                    break;
                }

                case Section::Stars:
                {
                    // [Cloud]/[Fog]と同じ作法。範囲外は打ち間違いとみなしてエラーにする
                    const auto readFloat = [&](float& out, bool& has, float minValue, float maxValue, const wchar_t* name)
                    {
                        if (!ParseFloatToken(value, out))
                        {
                            errorAt(lineNumber, rawLine, WideToUtf8(name) + "の値が不正です");
                        }
                        if (out < minValue || out > maxValue)
                        {
                            errorAt(lineNumber, rawLine, WideToUtf8(name) + "の値が範囲外です");
                        }
                        has = true;
                    };

                    if (CaseInsensitiveEquals(key, L"Enabled"))
                    {
                        const std::optional<bool> parsedValue = ParseBoolToken(value);
                        if (!parsedValue) errorAt(lineNumber, rawLine, "Enabledの値はtrue/falseで指定してください");
                        result.StarsEnabled = *parsedValue;
                        result.HasStarsEnabled = true;
                    }
                    else if (CaseInsensitiveEquals(key, L"Density"))
                    {
                        // 上限256は「1セルに1個」の規則から全天で数十万個に相当し、
                        // これ以上は星というより砂嵐になる
                        readFloat(result.StarsDensity, result.HasStarsDensity, 1.0f, 256.0f, L"Density");
                    }
                    else if (CaseInsensitiveEquals(key, L"Brightness"))
                    {
                        readFloat(result.StarsBrightness, result.HasStarsBrightness, 0.0f, 20.0f, L"Brightness");
                    }
                    else if (CaseInsensitiveEquals(key, L"Twinkle"))
                    {
                        readFloat(result.StarsTwinkle, result.HasStarsTwinkle, 0.0f, 1.0f, L"Twinkle");
                    }
                    else
                    {
                        warnUnknownKey();
                    }
                    break;
                }

                case Section::DroneShow:
                {
                    const auto readFloat = [&](float& out, bool& has, float minValue, float maxValue, const wchar_t* name)
                    {
                        if (!ParseFloatToken(value, out))
                        {
                            errorAt(lineNumber, rawLine, WideToUtf8(name) + "の値が不正です");
                        }
                        if (out < minValue || out > maxValue)
                        {
                            errorAt(lineNumber, rawLine, WideToUtf8(name) + "の値が範囲外です");
                        }
                        has = true;
                    };

                    if (CaseInsensitiveEquals(key, L"Enabled"))
                    {
                        const std::optional<bool> parsedValue = ParseBoolToken(value);
                        if (!parsedValue) errorAt(lineNumber, rawLine, "Enabledの値はtrue/falseで指定してください");
                        result.DroneShowEnabled = *parsedValue;
                        result.HasDroneShowEnabled = true;
                    }
                    else if (CaseInsensitiveEquals(key, L"Path"))
                    {
                        // .kshowのパス。パス解決(ルート外チェック・絶対パス化)はここでは行わず、
                        // [Scene]Skybox・[Water]NormalMapと同じくLoadScene側でまとめて行う
                        result.DroneShowPath = value;
                    }
                    else if (CaseInsensitiveEquals(key, L"Center"))
                    {
                        if (!ParseFloat3(value, result.DroneShowCenter))
                        {
                            errorAt(lineNumber, rawLine, "Centerの値が不正です(x, y, zの3要素が必要)");
                        }
                        result.HasDroneShowCenter = true;
                    }
                    else if (CaseInsensitiveEquals(key, L"Scale"))
                    {
                        readFloat(result.DroneShowScale, result.HasDroneShowScale, 1.0f, 5000.0f, L"Scale");
                    }
                    else
                    {
                        warnUnknownKey();
                    }
                    break;
                }

                default:
                    break;
                }

                if (newlinePos == std::wstring::npos) break;
                pos = newlinePos + 1;
            }

            if (result.Models.empty())
            {
                throw std::runtime_error("シーンファイルに[Model]が1つもありません: " + WideToUtf8(filePath));
            }
            for (size_t i = 0; i < result.Models.size(); ++i)
            {
                if (!result.Models[i].HasPath)
                {
                    throw std::runtime_error("[Model]の" + std::to_string(i + 1) + "番目にPathが指定されていません: " + WideToUtf8(filePath));
                }
            }
            for (size_t i = 0; i < result.Lights.size(); ++i)
            {
                const ParsedLightEntry& light = result.Lights[i];
                if (!light.HasType)
                {
                    throw std::runtime_error("[Light]の" + std::to_string(i + 1) + "番目にTypeが指定されていません: " + WideToUtf8(filePath));
                }
                if (!light.HasPosition)
                {
                    throw std::runtime_error("[Light]の" + std::to_string(i + 1) + "番目にPositionが指定されていません: " + WideToUtf8(filePath));
                }
                if (light.Type == LightType::Spot && !light.HasDirection)
                {
                    throw std::runtime_error("[Light]の" + std::to_string(i + 1) + "番目(Spot)にDirectionが指定されていません: " + WideToUtf8(filePath));
                }
            }
            for (size_t i = 0; i < result.ReflectionProbes.size(); ++i)
            {
                if (!result.ReflectionProbes[i].HasPosition)
                {
                    throw std::runtime_error("[ReflectionProbe]の" + std::to_string(i + 1) + "番目にPositionが指定されていません: " + WideToUtf8(filePath));
                }
            }
            for (size_t i = 0; i < result.GIVolumes.size(); ++i)
            {
                const ParsedGIVolumeEntry& volume = result.GIVolumes[i];
                // OriginとProbeCountsは既定値で代用できない。前者は格子の位置そのもので、
                // 後者はボリュームの大きさを決める(ProbeSpacingとの積が範囲になる)ため、
                // 書き忘れると意図と無関係な場所へ静かに格子が張られる
                if (!volume.HasOrigin)
                {
                    throw std::runtime_error("[GIVolume]の" + std::to_string(i + 1) + "番目にOriginが指定されていません: " + WideToUtf8(filePath));
                }
                if (!volume.HasProbeCounts)
                {
                    throw std::runtime_error("[GIVolume]の" + std::to_string(i + 1) + "番目にProbeCountsが指定されていません: " + WideToUtf8(filePath));
                }
            }
            if (result.HasCamera && !result.CameraPositionSet)
            {
                throw std::runtime_error("[Camera]セクションにPositionが指定されていません: " + WideToUtf8(filePath));
            }

            return result;
        }

        std::wstring GetFileStem(const std::wstring& filePath)
        {
            const size_t slash = filePath.find_last_of(L"/\\");
            const std::wstring fileName = slash == std::wstring::npos ? filePath : filePath.substr(slash + 1);
            const size_t dot = fileName.find_last_of(L'.');
            return dot == std::wstring::npos ? fileName : fileName.substr(0, dot);
        }
    }

    Scene LoadScene(RHI::IRHIDevice& device, const std::wstring& sceneFilePath, const std::wstring& assetRootDirectory)
    {
        const ParsedScene parsed = ParseSceneFile(sceneFilePath);

        Scene scene;
        scene.Name = parsed.HasName ? parsed.Name : GetFileStem(sceneFilePath);
        scene.HasCameraOverride = parsed.HasCamera;
        scene.CameraPosition[0] = parsed.CameraPosition[0];
        scene.CameraPosition[1] = parsed.CameraPosition[1];
        scene.CameraPosition[2] = parsed.CameraPosition[2];
        // .kscene上のYaw/Pitchは度(ドキュメント4.7節)。Camera::SetYawPitchはラジアンを受け取るため、
        // [Light]のConeAngleDegreesや[Model]のRotationEulerと同様にここで変換する
        // (これまで変換が抜けていたが、[Camera]を持つ既存シーンがYaw = 0.0しか使っておらず
        //  度とラジアンで同じ値になるため表面化していなかった)
        scene.CameraYaw = DirectX::XMConvertToRadians(parsed.CameraYaw);
        scene.CameraPitch = DirectX::XMConvertToRadians(parsed.CameraPitch);
        scene.SunTimeOfDay = parsed.SunTimeOfDay;
        scene.SunAzimuthDegrees = parsed.SunAzimuthDegrees;
        scene.ShadowEnabled = parsed.SunShadow;
        scene.SunEnabled = parsed.SunEnabled;
        scene.AOEnabled = parsed.AOEnabled;
        scene.HasSSREnabledOverride = parsed.HasSSREnabled;
        scene.SSREnabled = parsed.SSREnabled;
        scene.Tonemap = parsed.Tonemap;
        scene.SkySaturation = parsed.SkySaturation;
        scene.HasExposureOverride = parsed.HasExposure;
        scene.HasCloudCoverage = parsed.HasCloudCoverage;   scene.CloudCoverage = parsed.CloudCoverage;
        scene.HasCloudAltitude = parsed.HasCloudAltitude;   scene.CloudAltitude = parsed.CloudAltitude;
        scene.HasCloudThickness = parsed.HasCloudThickness; scene.CloudThickness = parsed.CloudThickness;
        scene.HasCloudDensity = parsed.HasCloudDensity;     scene.CloudDensity = parsed.CloudDensity;
        scene.HasCloudCellSize = parsed.HasCloudCellSize;   scene.CloudCellSize = parsed.CloudCellSize;
        scene.HasCirrusCoverage = parsed.HasCirrusCoverage; scene.CirrusCoverage = parsed.CirrusCoverage;
        scene.HasFogEnabled = parsed.HasFogEnabled;         scene.FogEnabled = parsed.FogEnabled;
        scene.HasFogDensity = parsed.HasFogDensity;         scene.FogDensity = parsed.FogDensity;
        scene.HasFogScaleHeight = parsed.HasFogScaleHeight; scene.FogScaleHeight = parsed.FogScaleHeight;
        scene.HasFogRefHeight = parsed.HasFogRefHeight;     scene.FogRefHeight = parsed.FogRefHeight;
        scene.HasBloomEnabled = parsed.HasBloomEnabled;       scene.BloomEnabled = parsed.BloomEnabled;
        scene.HasBloomStrength = parsed.HasBloomStrength;     scene.BloomStrength = parsed.BloomStrength;
        scene.HasBloomThreshold = parsed.HasBloomThreshold;   scene.BloomThreshold = parsed.BloomThreshold;
        scene.HasStarsEnabled = parsed.HasStarsEnabled;       scene.StarsEnabled = parsed.StarsEnabled;
        scene.HasStarsDensity = parsed.HasStarsDensity;       scene.StarsDensity = parsed.StarsDensity;
        scene.HasStarsBrightness = parsed.HasStarsBrightness; scene.StarsBrightness = parsed.StarsBrightness;
        scene.HasStarsTwinkle = parsed.HasStarsTwinkle;       scene.StarsTwinkle = parsed.StarsTwinkle;
        scene.HasDroneShowEnabled = parsed.HasDroneShowEnabled;   scene.DroneShowEnabled = parsed.DroneShowEnabled;
        scene.HasDroneShowCenter = parsed.HasDroneShowCenter;
        scene.DroneShowCenter[0] = parsed.DroneShowCenter[0];
        scene.DroneShowCenter[1] = parsed.DroneShowCenter[1];
        scene.DroneShowCenter[2] = parsed.DroneShowCenter[2];
        scene.HasDroneShowScale = parsed.HasDroneShowScale;             scene.DroneShowScale = parsed.DroneShowScale;
        scene.ExposureEV100 = parsed.ExposureEV100;
        scene.HasIBLIntensityOverride = parsed.HasIBLIntensity;
        scene.IBLIntensity = parsed.IBLIntensity;

        // [Scene]Skyboxは[Model]Pathと同じくAssetsルートからの相対パスとして扱い、
        // 同じルート外チェックを適用したうえで絶対パスへ解決してから返す
        if (!parsed.SkyboxPath.empty())
        {
            scene.SkyboxPath = ResolveAssetRelativePath(parsed.SkyboxPath, assetRootDirectory, L"[Scene]Skybox", sceneFilePath);
        }

        // [Water]NormalMapも同じ規則(Assetsルートからの相対パス、ルート外チェックあり)で解決する。
        // 空文字列のままなら「法線マップ無しのフラット水面」を意味し、C++側(KurenaiEngine3D)が
        // 1x1のフラット法線テクスチャへフォールバックするためエラーにはしない
        if (!parsed.WaterNormalMapPath.empty())
        {
            scene.WaterNormalMapPath =
                ResolveAssetRelativePath(parsed.WaterNormalMapPath, assetRootDirectory, L"[Water]NormalMap", sceneFilePath);
        }
        scene.WaterWaveScale = parsed.WaterWaveScale;
        scene.WaterWaveSpeed = parsed.WaterWaveSpeed;
        scene.WaterWaveStrength = parsed.WaterWaveStrength;

        // [DroneShow]Pathの.kshowも同じ規則で解決し、ここ(=Loaderスレッド)で読んでしまう。
        // Renderスレッドでファイルを開かないための配置で、[Model]Pathの.kmodelと同じ扱い。
        //
        // 【読み込み失敗でシーンごと落とさない】モデルはシーンそのものだが、ドローンショーは
        // 夜空の装飾で、これが無くてもシーンは成立する。エラーをログに残して編隊なしで進む
        // (パス解決の失敗——Assetsルートの外を指しているなど——は書式の誤りなので従来どおり投げる)
        if (!parsed.DroneShowPath.empty())
        {
            const std::wstring showPath =
                ResolveAssetRelativePath(parsed.DroneShowPath, assetRootDirectory, L"[DroneShow]Path", sceneFilePath);
            try
            {
                scene.DroneShowData = LoadShow(showPath);
            }
            catch (const std::exception& e)
            {
                Core::Logger::Error(
                    "SceneLoader",
                    std::string("[DroneShow]Pathのショーを読み込めませんでした(編隊なしで続行します): ") + e.what());
            }
        }

        // .kscene自身の[Light]で直接指定されたライトは、既にワールド空間の値として書かれているため
        // 変換不要でそのままScene::Lightsへ入れる(モデル埋め込みライトは下のモデルループ内で
        // Instance::Worldによるワールド空間への変換を行ってから追加する)
        for (const ParsedLightEntry& parsedLight : parsed.Lights)
        {
            Light light;
            light.Type = parsedLight.Type;
            std::memcpy(light.Position, parsedLight.Position, sizeof(light.Position));
            std::memcpy(light.Direction, parsedLight.Direction, sizeof(light.Direction));
            std::memcpy(light.Color, parsedLight.Color, sizeof(light.Color));
            light.Intensity = parsedLight.Intensity;
            light.Range = parsedLight.Range;
            // .ksceneはコーン角を1つ(外側)しか持たないため、内側も同じ値にしてソフトエッジ無しの
            // 単純な円錐として扱う
            const float outerRadians = DirectX::XMConvertToRadians(parsedLight.ConeAngleDegrees);
            light.SpotOuterConeAngle = outerRadians;
            light.SpotInnerConeAngle = outerRadians;
            light.Enabled = true;
            light.CastShadow = parsedLight.CastShadow;
            scene.Lights.push_back(light);
        }

        // [ReflectionProbe]も[Light]と同様、.kscene上の値が既にワールド空間のため変換不要
        for (const ParsedReflectionProbeEntry& parsedProbe : parsed.ReflectionProbes)
        {
            ReflectionProbe probe;
            std::memcpy(probe.Position, parsedProbe.Position, sizeof(probe.Position));
            probe.Radius = parsedProbe.Radius;
            probe.Shape = parsedProbe.Shape;
            std::memcpy(probe.BoxExtents, parsedProbe.BoxExtents, sizeof(probe.BoxExtents));
            probe.YawDegrees = parsedProbe.YawDegrees;
            probe.BlendDistance = parsedProbe.BlendDistance;
            probe.Name = parsedProbe.Name.empty()
                ? ("Probe " + std::to_string(scene.ReflectionProbes.size()))
                : WideToUtf8(parsedProbe.Name);
            scene.ReflectionProbes.push_back(probe);
        }

        // [GIVolume]も同様にワールド空間のまま渡す
        for (const ParsedGIVolumeEntry& parsedVolume : parsed.GIVolumes)
        {
            GIVolume volume;
            std::memcpy(volume.Origin, parsedVolume.Origin, sizeof(volume.Origin));
            std::memcpy(volume.ProbeSpacing, parsedVolume.ProbeSpacing, sizeof(volume.ProbeSpacing));
            std::memcpy(volume.ProbeCounts, parsedVolume.ProbeCounts, sizeof(volume.ProbeCounts));
            volume.NormalBias = parsedVolume.NormalBias;
            volume.ViewBias = parsedVolume.ViewBias;
            volume.Hysteresis = parsedVolume.Hysteresis;
            volume.MaxRayDistance = parsedVolume.MaxRayDistance;
            volume.Name = parsedVolume.Name.empty()
                ? ("GI Volume " + std::to_string(scene.GIVolumes.size()))
                : WideToUtf8(parsedVolume.Name);
            scene.GIVolumes.push_back(volume);
        }

        bool boundsInitialized = false;

        for (const ParsedModelEntry& parsedModel : parsed.Models)
        {
            const std::wstring normalizedPath = NormalizePathSeparators(parsedModel.Path);
            if (IsPathEscaping(normalizedPath))
            {
                throw std::runtime_error(
                    "[Model]Pathがルート外を指しています(絶対パスまたは'..'は使用できません): " +
                    WideToUtf8(parsedModel.Path) + " (" + WideToUtf8(sceneFilePath) + ")");
            }

            const std::wstring fullModelPath = assetRootDirectory + normalizedPath;

            ModelInstance instance;
            instance.Model = LoadModel(device, fullModelPath);
            instance.IsWater = parsedModel.Water;

            using namespace DirectX;
            const XMMATRIX scaleMatrix = XMMatrixScaling(parsedModel.Scale[0], parsedModel.Scale[1], parsedModel.Scale[2]);
            const XMMATRIX rotationMatrix = XMMatrixRotationRollPitchYaw(
                XMConvertToRadians(parsedModel.RotationEulerDegrees[0]),
                XMConvertToRadians(parsedModel.RotationEulerDegrees[1]),
                XMConvertToRadians(parsedModel.RotationEulerDegrees[2]));
            const XMMATRIX translationMatrix = XMMatrixTranslation(parsedModel.Translation[0], parsedModel.Translation[1], parsedModel.Translation[2]);
            // 合成順はS(スケール)→R(回転)→T(平行移動)。行ベクトル規約(p' = p * World)のため
            // この掛け算順でスケール→回転→平行移動の順に適用される
            const XMMATRIX worldMathSpace = scaleMatrix * rotationMatrix * translationMatrix;

            const float determinant = XMVectorGetX(XMMatrixDeterminant(worldMathSpace));
            instance.TangentSignFlip = determinant < 0.0f ? -1.0f : 1.0f;
            // ミラーリングは三角形のワインディングも反転させるため、描画時に表裏判定を
            // 入れ替えたパイプラインを選ぶ必要がある(KurenaiEngine3D::Renderの各ジオメトリパス)
            instance.IsMirrored = determinant < 0.0f;

            // 法線用行列はWorldの3x3部分の逆転置(inverse-transpose)。回転+非一様スケールが
            // 組み合わさった場合に法線が歪むのを防ぐ(ModelSource.cppの同種の処理と同じ理由)。
            // 特異行列(スケール0など)で逆行列が求まらない場合は3x3部分をそのまま使う簡易
            // フォールバックとする
            XMMATRIX normalMathSpace = worldMathSpace;
            if (determinant != 0.0f)
            {
                normalMathSpace = XMMatrixTranspose(XMMatrixInverse(nullptr, worldMathSpace));
            }

            // FrameConstants(ViewProj等)と同じく、HLSL側のmul(vec, matrix)(行ベクトル)規約に
            // 合わせて転置して格納する
            XMStoreFloat4x4(&instance.World, XMMatrixTranspose(worldMathSpace));
            XMStoreFloat4x4(&instance.NormalMatrix, XMMatrixTranspose(normalMathSpace));

            // モデルのローカル空間AABB(8頂点)をWorldで変換し、シーン全体のAABBへ合成する。
            // 軸並行のまま変換前のmin/maxだけを使うと回転時に不正確になるため、必ず8頂点全てを変換する
            const Model& loadedModel = instance.Model;
            for (int cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
            {
                const XMVECTOR corner = XMVectorSet(
                    (cornerIndex & 1) ? loadedModel.BoundsMax[0] : loadedModel.BoundsMin[0],
                    (cornerIndex & 2) ? loadedModel.BoundsMax[1] : loadedModel.BoundsMin[1],
                    (cornerIndex & 4) ? loadedModel.BoundsMax[2] : loadedModel.BoundsMin[2],
                    1.0f);
                const XMVECTOR transformed = XMVector3TransformCoord(corner, worldMathSpace);
                XMFLOAT3 transformedFloat3;
                XMStoreFloat3(&transformedFloat3, transformed);

                if (!boundsInitialized)
                {
                    scene.BoundsMin[0] = scene.BoundsMax[0] = transformedFloat3.x;
                    scene.BoundsMin[1] = scene.BoundsMax[1] = transformedFloat3.y;
                    scene.BoundsMin[2] = scene.BoundsMax[2] = transformedFloat3.z;
                    boundsInitialized = true;
                }
                else
                {
                    scene.BoundsMin[0] = std::min(scene.BoundsMin[0], transformedFloat3.x);
                    scene.BoundsMin[1] = std::min(scene.BoundsMin[1], transformedFloat3.y);
                    scene.BoundsMin[2] = std::min(scene.BoundsMin[2], transformedFloat3.z);
                    scene.BoundsMax[0] = std::max(scene.BoundsMax[0], transformedFloat3.x);
                    scene.BoundsMax[1] = std::max(scene.BoundsMax[1], transformedFloat3.y);
                    scene.BoundsMax[2] = std::max(scene.BoundsMax[2], transformedFloat3.z);
                }
            }

            // モデルファイル埋め込みのライト(glTFのKHR_lights_punctual・FBXのライトノード由来、
            // ModelLoader.cppがModel::Lightsへ読み込み済み)をInstance::Worldでワールド空間へ変換して
            // シーン全体のライト一覧へ追加する。Positionは平行移動を含む点として、Directionは
            // 平行移動を含まない方向ベクトルとして変換する必要があるため、それぞれ
            // XMVector3TransformCoord/TransformNormalを使い分ける(法線のような逆転置は不要。
            // 接線ベクトルの変換(GBuffer.hlsl)と同じ理由)
            for (const Light& localLight : instance.Model.Lights)
            {
                Light worldLight = localLight;

                const XMVECTOR localPosition = XMVectorSet(localLight.Position[0], localLight.Position[1], localLight.Position[2], 0.0f);
                const XMVECTOR worldPosition = XMVector3TransformCoord(localPosition, worldMathSpace);
                XMFLOAT3 worldPositionFloat3;
                XMStoreFloat3(&worldPositionFloat3, worldPosition);
                worldLight.Position[0] = worldPositionFloat3.x;
                worldLight.Position[1] = worldPositionFloat3.y;
                worldLight.Position[2] = worldPositionFloat3.z;

                const XMVECTOR localDirection = XMVectorSet(localLight.Direction[0], localLight.Direction[1], localLight.Direction[2], 0.0f);
                const XMVECTOR worldDirection = XMVector3Normalize(XMVector3TransformNormal(localDirection, worldMathSpace));
                XMFLOAT3 worldDirectionFloat3;
                XMStoreFloat3(&worldDirectionFloat3, worldDirection);
                worldLight.Direction[0] = worldDirectionFloat3.x;
                worldLight.Direction[1] = worldDirectionFloat3.y;
                worldLight.Direction[2] = worldDirectionFloat3.z;

                scene.Lights.push_back(worldLight);
            }

            scene.Instances.push_back(std::move(instance));
        }

        return scene;
    }

    std::wstring ReadSceneName(const std::wstring& sceneFilePath)
    {
        const ParsedScene parsed = ParseSceneFile(sceneFilePath);
        return parsed.HasName ? parsed.Name : GetFileStem(sceneFilePath);
    }

    void ValidateScene(const std::wstring& sceneFilePath, const std::wstring& assetRootDirectory)
    {
        // 書式(セクション/キー/必須フィールド/数値範囲)の検証はParseSceneFile自体が行う
        const ParsedScene parsed = ParseSceneFile(sceneFilePath);

        for (const ParsedModelEntry& parsedModel : parsed.Models)
        {
            const std::wstring normalizedPath = NormalizePathSeparators(parsedModel.Path);
            if (IsPathEscaping(normalizedPath))
            {
                throw std::runtime_error(
                    "[Model]Pathがルート外を指しています(絶対パスまたは'..'は使用できません): " +
                    WideToUtf8(parsedModel.Path) + " (" + WideToUtf8(sceneFilePath) + ")");
            }

            const std::wstring fullModelPath = assetRootDirectory + normalizedPath;

            std::ifstream modelIn(fullModelPath, std::ios::binary);
            if (!modelIn.is_open())
            {
                throw std::runtime_error("[Model]Pathが指す.kmodelが見つかりません: " + WideToUtf8(fullModelPath));
            }

            PackageHeader header{};
            modelIn.read(reinterpret_cast<char*>(&header), sizeof(header));
            if (!modelIn || std::memcmp(header.Magic, kPackageMagic, sizeof(kPackageMagic)) != 0)
            {
                throw std::runtime_error(".kmodelのマジックナンバーが不正です: " + WideToUtf8(fullModelPath));
            }
            if (header.Version != kPackageVersion)
            {
                throw std::runtime_error(
                    ".kmodelのバージョンが対応していません(ファイル: " + std::to_string(header.Version) +
                    ", ランタイム: " + std::to_string(kPackageVersion) + "): " + WideToUtf8(fullModelPath));
            }
        }
    }
}
