#include "SceneLoader.h"

#include <Windows.h>

#include <DirectXMath.h>

#include <algorithm>
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

        // ==== パース結果の中間表現(モデルの実読み込み前) ====

        struct ParsedModelEntry
        {
            std::wstring Path;
            bool HasPath = false;
            float Translation[3] = { 0.0f, 0.0f, 0.0f };
            float RotationEulerDegrees[3] = { 0.0f, 0.0f, 0.0f };
            float Scale[3] = { 1.0f, 1.0f, 1.0f };
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
            bool SSREnabled = true;

            std::vector<ParsedLightEntry> Lights;
        };

        enum class Section
        {
            None,
            Scene,
            Model,
            Camera,
            Sun,
            Light,
            Unknown,
        };

        Section SectionFromName(const std::wstring& name)
        {
            if (CaseInsensitiveEquals(name, L"Scene")) return Section::Scene;
            if (CaseInsensitiveEquals(name, L"Model")) return Section::Model;
            if (CaseInsensitiveEquals(name, L"Camera")) return Section::Camera;
            if (CaseInsensitiveEquals(name, L"Sun")) return Section::Sun;
            if (CaseInsensitiveEquals(name, L"Light")) return Section::Light;
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
                    else if (CaseInsensitiveEquals(key, L"ScreenSpaceReflection"))
                    {
                        const std::optional<bool> parsedValue = ParseBoolToken(value);
                        if (!parsedValue) errorAt(lineNumber, rawLine, "ScreenSpaceReflectionの値はtrue/falseで指定してください");
                        result.SSREnabled = *parsedValue;
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
        scene.CameraYaw = parsed.CameraYaw;
        scene.CameraPitch = parsed.CameraPitch;
        scene.SunTimeOfDay = parsed.SunTimeOfDay;
        scene.SunAzimuthDegrees = parsed.SunAzimuthDegrees;
        scene.ShadowEnabled = parsed.SunShadow;
        scene.SunEnabled = parsed.SunEnabled;
        scene.AOEnabled = parsed.AOEnabled;
        scene.SSREnabled = parsed.SSREnabled;
        scene.HasIBLIntensityOverride = parsed.HasIBLIntensity;
        scene.IBLIntensity = parsed.IBLIntensity;

        // [Scene]Skyboxは[Model]Pathと同じくAssetsルートからの相対パスとして扱い、
        // 同じルート外チェックを適用したうえで絶対パスへ解決してから返す
        if (!parsed.SkyboxPath.empty())
        {
            const std::wstring normalizedSkyboxPath = NormalizePathSeparators(parsed.SkyboxPath);
            if (IsPathEscaping(normalizedSkyboxPath))
            {
                throw std::runtime_error(
                    "[Scene]Skyboxがルート外を指しています(絶対パスまたは'..'は使用できません): " +
                    WideToUtf8(parsed.SkyboxPath) + " (" + WideToUtf8(sceneFilePath) + ")");
            }
            scene.SkyboxPath = assetRootDirectory + normalizedSkyboxPath;
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
