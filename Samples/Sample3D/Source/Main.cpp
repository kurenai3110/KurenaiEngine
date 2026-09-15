#include <Windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "Assets/SceneLoader.h"
#include "Core/Logger.h"
#include "Core/StringUtil.h"
#include "EngineDefaults.h"
#include "KurenaiEngine3D.h"
#include "KurenaiTypes.h"

namespace
{
    std::wstring Utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty())
        {
            return {};
        }

        int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        std::wstring wide(length, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), length);
        return wide;
    }

    // 「-dx12」引数が指定されていればDX12バックエンドを使う(再ビルド無しでDX11/DX12を比較するため)
    Kurenai::GraphicsAPI ParseGraphicsAPI()
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            return Kurenai::GraphicsAPI::DX11;
        }

        Kurenai::GraphicsAPI api = Kurenai::GraphicsAPI::DX11;
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"-dx12") == 0)
            {
                api = Kurenai::GraphicsAPI::DX12;
                break;
            }
        }

        LocalFree(argv);
        return api;
    }

    // コマンドラインの「-ddgilod <段数>」と「-ddgifollow」を読む。
    // クリップマップLODの段数を振って効果を測るためのもの。指定が無ければ0を返す
    uint32_t ParseDDGILODCount()
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            return 0u;
        }

        uint32_t lodCount = 0u;
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"-ddgilod") != 0)
            {
                continue;
            }
            if (i + 1 >= argc)
            {
                Kurenai::Core::Logger::Warning(
                    "Main", "-ddgilodの後に段数が指定されていないため、.ksceneの指定のままにします");
                break;
            }
            wchar_t* end = nullptr;
            const long parsed = wcstol(argv[i + 1], &end, 10);
            if (end == argv[i + 1] || (end != nullptr && *end != 0) || parsed < 1)
            {
                Kurenai::Core::Logger::Warning(
                    "Main",
                    "-ddgilodの引数が正の整数ではないため、.ksceneの指定のままにします: " +
                        Kurenai::Core::WideToUtf8(argv[i + 1]));
                break;
            }
            lodCount = static_cast<uint32_t>(parsed);
            break;
        }

        LocalFree(argv);
        return lodCount;
    }

    bool ParseDDGIFollowCamera()
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            return false;
        }

        bool follow = false;
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"-ddgifollow") == 0)
            {
                follow = true;
                break;
            }
        }

        LocalFree(argv);
        return follow;
    }

    // コマンドラインの「-ddgithreshold <値>」を読む。DDGIのプローブ分類のしきい値。
    // 0を渡すと分類そのものを無効にする。指定が無ければ負を返して既定のままにする
    float ParseDDGIBackfaceThreshold()
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            return -1.0f;
        }

        float threshold = -1.0f;
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"-ddgithreshold") != 0)
            {
                continue;
            }
            if (i + 1 >= argc)
            {
                Kurenai::Core::Logger::Warning(
                    "Main", "-ddgithresholdの後に値が指定されていないため、既定のままにします");
                break;
            }
            wchar_t* end = nullptr;
            const double parsed = wcstod(argv[i + 1], &end);
            // 末尾までが数値であること(終端がNUL以外なら余計な文字が付いている)
            if (end == argv[i + 1] || (end != nullptr && *end != 0))
            {
                Kurenai::Core::Logger::Warning(
                    "Main",
                    "-ddgithresholdの引数が数値ではないため、既定のままにします: " +
                        Kurenai::Core::WideToUtf8(argv[i + 1]));
                break;
            }
            threshold = static_cast<float>(parsed);
            break;
        }

        LocalFree(argv);
        return threshold;
    }

    // コマンドラインに「-ddgiraster」があるか。あればDDGIのレイ取得をラスタライズへ固定する。
    // ラスタ経路とレイトレース経路のA/B比較を、同じ起動手順のまま切り替えるために使う
    bool ParseForceDDGIRaster()
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            return false;
        }

        bool forceRaster = false;
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"-ddgiraster") == 0)
            {
                forceRaster = true;
                break;
            }
        }

        LocalFree(argv);
        return forceRaster;
    }

    // 値を取らない起動オプション(フラグ)があるか。ParseForceDDGIRasterと同じ処理だが、
    // フラグが増えるたびに同じ関数を書き足すのをやめて名前で引く形にしたもの
    bool HasFlagOption(const wchar_t* optionName)
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            return false;
        }

        bool found = false;
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], optionName) == 0)
            {
                found = true;
                break;
            }
        }

        LocalFree(argv);
        return found;
    }

    // コマンドラインの「-debugview <番号>」を読む。番号の並びはUIの「デバッグ表示」コンボと同じ。
    // 指定が無ければ-1(=既定のFinalのまま)を返す。
    //
    // 【何のためにあるのか】アトラスやバッファの生値を測る検証を、GUIのコンボを人手で
    // 操作せずに再現できるようにするため(KurenaiEngine3D::SetDebugViewIndexのコメント参照)
    int ParseDebugViewIndex()
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            return -1;
        }

        int index = -1;
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"-debugview") != 0)
            {
                continue;
            }
            if (i + 1 >= argc)
            {
                Kurenai::Core::Logger::Warning(
                    "Main", "-debugviewの後に番号が指定されていないため、デバッグ表示は既定のままにします");
                break;
            }
            // 数字以外が来たら弾く(wcstolは先頭が数字でなければ0を返すため、自前で見る)
            wchar_t* end = nullptr;
            const long parsed = wcstol(argv[i + 1], &end, 10);
            // 末尾までが数字であること(終端がNUL以外なら余計な文字が付いている)
            if (end == argv[i + 1] || (end != nullptr && *end != 0))
            {
                Kurenai::Core::Logger::Warning(
                    "Main",
                    "-debugviewの引数が数値ではないため、デバッグ表示は既定のままにします: " +
                        Kurenai::Core::WideToUtf8(argv[i + 1]));
                break;
            }
            index = static_cast<int>(parsed);
            break;
        }

        LocalFree(argv);
        return index;
    }

    // 「<オプション名> <文字列>」の形の起動オプションを1つ読む。指定が無ければ空文字列を返す
    std::wstring ParseStringOption(const wchar_t* optionName)
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            return std::wstring();
        }

        std::wstring value;
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], optionName) != 0)
            {
                continue;
            }
            if (i + 1 >= argc)
            {
                Kurenai::Core::Logger::Warning(
                    "Main",
                    Kurenai::Core::WideToUtf8(optionName) + "の後に値が指定されていないため、無視します");
                break;
            }
            value = argv[i + 1];
            break;
        }

        LocalFree(argv);
        return value;
    }

    // 「<オプション名> <整数>」の形の起動オプションを1つ読む。指定が無い場合と、
    // 値が整数でない場合は notFound をそのまま返す(呼び出し側が「指定なし」を判別できるように)。
    // 検査はParseDebugViewIndexと同じで、末尾までが数字であることまで見る
    int ParseIntOption(const wchar_t* optionName, int notFound)
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            return notFound;
        }

        int value = notFound;
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], optionName) != 0)
            {
                continue;
            }
            const std::string optionNameUtf8 = Kurenai::Core::WideToUtf8(optionName);
            if (i + 1 >= argc)
            {
                Kurenai::Core::Logger::Error(
                    "Main", optionNameUtf8 + "の後に値が指定されていないため、既定のままにします");
                break;
            }
            wchar_t* end = nullptr;
            const long parsed = wcstol(argv[i + 1], &end, 10);
            if (end == argv[i + 1] || (end != nullptr && *end != 0))
            {
                Kurenai::Core::Logger::Error(
                    "Main",
                    optionNameUtf8 + "の引数が数値ではないため、既定のままにします: " +
                        Kurenai::Core::WideToUtf8(argv[i + 1]));
                break;
            }
            value = static_cast<int>(parsed);
            break;
        }

        LocalFree(argv);
        return value;
    }

    // 「<オプション名> <実数>」の形の起動オプションを1つ読む。ParseIntOptionの実数版で、
    // 検査の仕方(末尾まで数値として読み切れること)も同じ
    float ParseFloatOption(const wchar_t* optionName, float notFound)
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            return notFound;
        }

        float value = notFound;
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], optionName) != 0)
            {
                continue;
            }
            const std::string optionNameUtf8 = Kurenai::Core::WideToUtf8(optionName);
            if (i + 1 >= argc)
            {
                Kurenai::Core::Logger::Error(
                    "Main", optionNameUtf8 + "の後に値が指定されていないため、既定のままにします");
                break;
            }
            wchar_t* end = nullptr;
            const double parsed = wcstod(argv[i + 1], &end);
            if (end == argv[i + 1] || (end != nullptr && *end != 0))
            {
                Kurenai::Core::Logger::Error(
                    "Main",
                    optionNameUtf8 + "の引数が数値ではないため、既定のままにします: " +
                        Kurenai::Core::WideToUtf8(argv[i + 1]));
                break;
            }
            value = static_cast<float>(parsed);
            break;
        }

        LocalFree(argv);
        return value;
    }

    // 「-dumptex <テクスチャ名> <出力パス>」の指定。**繰り返し指定できる**
    struct TextureDumpArg
    {
        std::wstring Name;
        std::wstring Path;
        int MipLevel = 0;
        int ArraySlice = 0;
        int Frames = 1;
        int Stride = 1;
    };

    // -dumptex を全部拾う。直後に続く -dumptexmip / -dumptexslice は「直前の -dumptex」に掛かる。
    //
    // 【1回の走査で拾う理由】ParseStringOptionは最初の1件で打ち切るため繰り返しに使えない。
    // また mip/slice を「どの -dumptex に掛かるか」で決めるには、引数の並び順を見る必要がある。
    // **1回の起動で必要なバッファを全部吸えること**が肝心で(GUIの起動は共有資源)、
    // そのために繰り返し指定を受け付ける
    std::vector<TextureDumpArg> ParseTextureDumps()
    {
        std::vector<TextureDumpArg> dumps;

        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            return dumps;
        }

        const auto parseIntArg = [](const wchar_t* text, const char* optionName, int& outValue)
        {
            wchar_t* end = nullptr;
            const long parsed = wcstol(text, &end, 10);
            if (end == text || (end != nullptr && *end != 0) || parsed < 0)
            {
                Kurenai::Core::Logger::Warning(
                    "Main",
                    std::string(optionName) + "の引数が0以上の整数ではないため、無視します: " +
                        Kurenai::Core::WideToUtf8(text));
                return false;
            }
            outValue = static_cast<int>(parsed);
            return true;
        };

        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"-dumptex") == 0)
            {
                if (i + 2 >= argc)
                {
                    Kurenai::Core::Logger::Warning(
                        "Main", "-dumptex は「-dumptex <テクスチャ名> <出力パス>」の形で指定します。無視します");
                    break;
                }
                TextureDumpArg dump;
                dump.Name = argv[i + 1];
                dump.Path = argv[i + 2];
                dumps.push_back(std::move(dump));
                i += 2;
                continue;
            }

            if (_wcsicmp(argv[i], L"-dumptexmip") == 0 || _wcsicmp(argv[i], L"-dumptexslice") == 0 ||
                _wcsicmp(argv[i], L"-dumptexframes") == 0 || _wcsicmp(argv[i], L"-dumptexstride") == 0)
            {
                const bool isMip = _wcsicmp(argv[i], L"-dumptexmip") == 0;
                const bool isSlice = _wcsicmp(argv[i], L"-dumptexslice") == 0;
                const bool isFrames = _wcsicmp(argv[i], L"-dumptexframes") == 0;
                const bool isStride = _wcsicmp(argv[i], L"-dumptexstride") == 0;
                const char* optionName = isMip      ? "-dumptexmip"
                                         : isSlice  ? "-dumptexslice"
                                         : isFrames ? "-dumptexframes"
                                                    : "-dumptexstride";
                if (dumps.empty())
                {
                    // 直前に -dumptex が無ければ掛ける相手がいない。黙って捨てると
                    // 「指定したのに効かない」形で気づけないので警告を出す
                    Kurenai::Core::Logger::Warning(
                        "Main", std::string(optionName) + " は -dumptex の後に指定します。無視します");
                    continue;
                }
                if (i + 1 >= argc)
                {
                    Kurenai::Core::Logger::Warning(
                        "Main", std::string(optionName) + "の後に値が指定されていないため、無視します");
                    break;
                }
                int value = 0;
                if (parseIntArg(argv[i + 1], optionName, value))
                {
                    // 枚数と間隔は0だと「撮らない」「無限に待つ」の意味になってしまうので1へ寄せる。
                    // ミップと配列スライスは0が正当な既定値なのでここには含めない
                    if ((isFrames || isStride) && value <= 0)
                    {
                        Kurenai::Core::Logger::Warning(
                            "Main", std::string(optionName) + " は1以上で指定してください。1へ補正します: " +
                                Kurenai::Core::WideToUtf8(argv[i + 1]));
                        value = 1;
                    }
                    if (isMip) dumps.back().MipLevel = value;
                    else if (isSlice) dumps.back().ArraySlice = value;
                    else if (isFrames) dumps.back().Frames = value;
                    else dumps.back().Stride = value;
                }
                ++i;
                continue;
            }
        }

        LocalFree(argv);
        return dumps;
    }

    struct BufferDumpArg
    {
        std::wstring Name;
        std::wstring Path;
    };

    // -dumpbuf は -dumptex と同じく名前と出力パスを対で受け取り、複数回指定できる。
    std::vector<BufferDumpArg> ParseBufferDumps()
    {
        std::vector<BufferDumpArg> dumps;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            Kurenai::Core::Logger::Error("Main", "-dumpbuf のコマンドラインを取得できませんでした");
            return dumps;
        }
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"-dumpbuf") != 0) continue;
            if (i + 2 >= argc)
            {
                Kurenai::Core::Logger::Warning(
                    "Main", "-dumpbuf は「-dumpbuf <バッファ名> <出力パス>」の形で指定します。無視します");
                break;
            }
            dumps.push_back(BufferDumpArg{ argv[i + 1], argv[i + 2] });
            i += 2;
        }
        LocalFree(argv);
        return dumps;
    }

    // -recreate <フレーム> <指示> を全部拾う。ベースライン採取だけでは通らない解像度・精度・
    // シーン切り替え時のGPUリソース作り直し経路を、無人の採取スクリプトから検証するために使う。
    std::vector<Kurenai::ScheduledRecreation> ParseScheduledRecreations()
    {
        std::vector<Kurenai::ScheduledRecreation> recreations;

        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            Kurenai::Core::Logger::Error("Main", "-recreateのコマンドライン取得に失敗しました");
            return recreations;
        }

        const auto logInvalid = [](const wchar_t* frame, const wchar_t* instruction)
        {
            Kurenai::Core::Logger::Error(
                "Main", "-recreateの指定が不正なため無視します: frame=" + Kurenai::Core::WideToUtf8(frame) +
                    ", instruction=" + Kurenai::Core::WideToUtf8(instruction));
        };
        const auto parseDimension = [](const std::wstring& text, uint32_t& value)
        {
            if (text.empty() || text[0] == L'-')
            {
                return false;
            }
            errno = 0;
            wchar_t* end = nullptr;
            const unsigned long parsed = wcstoul(text.c_str(), &end, 10);
            if (errno == ERANGE || end == text.c_str() || (end != nullptr && *end != L'\0') || parsed == 0 ||
                parsed > (std::numeric_limits<uint32_t>::max)())
            {
                return false;
            }
            value = static_cast<uint32_t>(parsed);
            return true;
        };

        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"-recreate") != 0)
            {
                continue;
            }
            if (i + 2 >= argc)
            {
                Kurenai::Core::Logger::Error("Main", "-recreate は「-recreate <フレーム> <指示>」の形で指定します。無視します");
                break;
            }

            errno = 0;
            wchar_t* frameEnd = nullptr;
            const long frame = wcstol(argv[i + 1], &frameEnd, 10);
            if (errno == ERANGE || frameEnd == argv[i + 1] || (frameEnd != nullptr && *frameEnd != L'\0') || frame < 0)
            {
                logInvalid(argv[i + 1], argv[i + 2]);
                i += 2;
                continue;
            }

            Kurenai::ScheduledRecreation request;
            request.Frame = static_cast<uint32_t>(frame);
            const std::wstring instruction = argv[i + 2];
            const auto parseResolution = [&](const wchar_t* prefix)
            {
                const size_t prefixLength = wcslen(prefix);
                if (instruction.size() <= prefixLength || _wcsnicmp(instruction.c_str(), prefix, prefixLength) != 0)
                {
                    return false;
                }
                const std::wstring size = instruction.substr(prefixLength);
                const size_t x = size.find(L'x');
                if (x == std::wstring::npos || size.find(L'x', x + 1) != std::wstring::npos ||
                    !parseDimension(size.substr(0, x), request.Width) || !parseDimension(size.substr(x + 1), request.Height))
                {
                    return false;
                }
                return true;
            };

            bool valid = true;
            if (_wcsnicmp(instruction.c_str(), L"renderres=", 10) == 0)
            {
                request.Kind = Kurenai::ScheduledRecreationKind::RenderResolution;
                valid = parseResolution(L"renderres=");
            }
            else if (_wcsnicmp(instruction.c_str(), L"upscale=", 8) == 0)
            {
                request.Kind = Kurenai::ScheduledRecreationKind::UpscaleOutput;
                valid = parseResolution(L"upscale=");
            }
            else if (_wcsicmp(instruction.c_str(), L"precision=hdr") == 0)
            {
                request.Kind = Kurenai::ScheduledRecreationKind::BufferPrecision;
                request.Precision = Kurenai::BufferPrecision::HDR;
            }
            else if (_wcsicmp(instruction.c_str(), L"precision=legacy8bit") == 0)
            {
                request.Kind = Kurenai::ScheduledRecreationKind::BufferPrecision;
                request.Precision = Kurenai::BufferPrecision::Legacy8bit;
            }
            else if (_wcsnicmp(instruction.c_str(), L"scene=", 6) == 0 && instruction.size() > 6)
            {
                request.Kind = Kurenai::ScheduledRecreationKind::SceneLoad;
                request.SceneName = instruction.substr(6);
            }
            else
            {
                valid = false;
            }

            if (!valid)
            {
                logInvalid(argv[i + 1], argv[i + 2]);
            }
            else
            {
                recreations.push_back(std::move(request));
            }
            i += 2;
        }

        LocalFree(argv);
        return recreations;
    }

    // -sceneを指定したのに解決できなかったときに投げる。
    //
    // 【既定のシーンへ落とさない】以前は警告を1行出して0番(一覧の先頭)で起動していたが、
    // 終了コードも0のままなので、**無人実行では指定ミスが静かに別のシーンの計測になる**。
    // 実際に -scene LODSwitchTest が _PlateauDebug.kscene を読み、
    // 新旧ビルドのバイト比較が丸ごと別シーンのものになった
    // (docs/ImplementationHistory.md 85章)。
    //
    // 【専用の型にしてMessageBoxを出さない理由】wWinMainのstd::exceptionハンドラは
    // ダイアログを出す。無人実行ではそこで止まってしまうため、この失敗だけは
    // ログと終了コードだけで返す
    struct SceneNotResolvedError : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    // コマンドラインの「-scene <名前>」(拡張子を除いたファイル名。例: MontSaintMichel)を、
    // KurenaiEngine3Dが構築するシーン一覧上の番号へ解決する。
    // 一覧の作り方(列挙→_wcsicmpで昇順ソート→Assets::ReadSceneNameが成功したものだけ採用)は
    // KurenaiEngine3D::DiscoverScenes()(KurenaiEngine3D.cpp)と厳密に一致させる必要がある
    // (手順がずれると番号が一覧側とずれ、意図と別のシーンが開いてしまう)。
    // 【-sceneの指定が無いときだけ0を返す】指定があって解決できないときは
    // SceneNotResolvedErrorを投げる(上のコメント参照)
    size_t ParseInitialSceneIndex()
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
        {
            return 0;
        }

        std::wstring requestedName;
        bool sceneOptionSeen = false;
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"-scene") == 0)
            {
                sceneOptionSeen = true;
                if (i + 1 < argc)
                {
                    requestedName = argv[i + 1];
                }
                break;
            }
        }
        LocalFree(argv);

        if (!sceneOptionSeen)
        {
            // -scene指定なし。従来どおり0番(一覧の先頭)で起動する
            return 0;
        }

        if (requestedName.empty())
        {
            Kurenai::Core::Logger::Error(
                "Main", "-sceneの後にシーン名が指定されていません");
            throw SceneNotResolvedError("-sceneの後にシーン名が指定されていません");
        }

        // 実行ファイル(Sample3D.exe)自身のあるディレクトリを求める。
        // KurenaiEngine3D::DiscoverScenes()が使うCore::GetModuleDirectory()はヘッダオンリーの
        // inline関数で、呼び出し元のTU(=このexe)でコンパイルされるとこのexeモジュール基準の
        // アドレスで解決されてしまい、KurenaiEngine.dll側のフォルダを返す保証が無い
        // (通常はexeとdllが同じ出力フォルダに置かれるため実害は無いはずだが、
        // 前提を混同しないためここではexe自身の配置フォルダを求める処理を素朴に書く)
        wchar_t exePathBuffer[MAX_PATH];
        DWORD exePathLength = GetModuleFileNameW(nullptr, exePathBuffer, MAX_PATH);
        if (exePathLength == 0 || exePathLength == MAX_PATH)
        {
            Kurenai::Core::Logger::Error(
                "Main", "実行ファイルのパス取得に失敗したため、-sceneの指定を解決できません");
            throw SceneNotResolvedError("実行ファイルのパス取得に失敗しました");
        }

        const std::wstring exePath(exePathBuffer, exePathLength);
        const size_t slashPos = exePath.find_last_of(L"\\/");
        const std::wstring exeDirectory = slashPos == std::wstring::npos ? L"" : exePath.substr(0, slashPos + 1);
        const std::wstring sceneDirectory = exeDirectory + L"Assets\\Scenes\\";

        // 以降、KurenaiEngine3D::DiscoverScenes()と同じ手順で一覧を組み立てる
        std::vector<std::wstring> fileNames;
        WIN32_FIND_DATAW findData{};
        HANDLE findHandle = FindFirstFileW((sceneDirectory + L"*.kscene").c_str(), &findData);
        if (findHandle != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    fileNames.push_back(findData.cFileName);
                }
            } while (FindNextFileW(findHandle, &findData));
            FindClose(findHandle);
        }
        else
        {
            const std::string message =
                "シーンフォルダを開けませんでした (" + Kurenai::Core::WideToUtf8(sceneDirectory) + ")";
            Kurenai::Core::Logger::Error("Main", message);
            throw SceneNotResolvedError(message);
        }

        std::sort(fileNames.begin(), fileNames.end(), [](const std::wstring& a, const std::wstring& b)
        {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });

        size_t resolvedIndex = 0;
        for (const std::wstring& fileName : fileNames)
        {
            const std::wstring fullPath = sceneDirectory + fileName;
            try
            {
                // 戻り値([Scene]Name)は使わない。DiscoverScenes()と同様、成功したものだけを
                // 番号に数えるための呼び出し(失敗するファイルは一覧から除外される)
                Kurenai::Assets::ReadSceneName(fullPath);
            }
            catch (const std::exception&)
            {
                // KurenaiEngine3D::DiscoverScenes()側で個別のエラーログが出るため、ここでは
                // 番号を進めずスキップするだけに留める
                continue;
            }

            // -sceneに渡すのは拡張子を除いたファイル名(例: MontSaintMichel)
            const size_t dotPos = fileName.find_last_of(L'.');
            const std::wstring stem = dotPos == std::wstring::npos ? fileName : fileName.substr(0, dotPos);

            if (_wcsicmp(stem.c_str(), requestedName.c_str()) == 0)
            {
                Kurenai::Core::Logger::Info(
                    "Main",
                    "コマンドライン引数で指定されたシーンを解決しました: " +
                        Kurenai::Core::WideToUtf8(requestedName) + " -> " + std::to_string(resolvedIndex) + "番");
                return resolvedIndex;
            }

            ++resolvedIndex;
        }

        // 【候補を並べて出す】名前の綴りではなく「そのシーンが配布先に無い」ことが
        // 原因のことがある。エンジンが読むのはビルド出力の Assets\Scenes\ で、
        // Git管理下の Scenes\ からの自動コピーは無い
        std::string candidates;
        for (const std::wstring& fileName : fileNames)
        {
            const size_t dotPos = fileName.find_last_of(L'.');
            const std::wstring stem = dotPos == std::wstring::npos ? fileName : fileName.substr(0, dotPos);
            if (!candidates.empty()) { candidates += ", "; }
            candidates += Kurenai::Core::WideToUtf8(stem);
        }
        const std::string message =
            "指定されたシーンが見つかりません: " + Kurenai::Core::WideToUtf8(requestedName) +
            " (探した場所: " + Kurenai::Core::WideToUtf8(sceneDirectory) + " / 候補: " + candidates + ")";
        Kurenai::Core::Logger::Error("Main", message);
        throw SceneNotResolvedError(message);
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    // DirectXTexのWICテクスチャ読み込みがCOMを使用するため初期化しておく
    HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int exitCode = 0;
    try
    {
        // ImGuiの「システム」パネルからグラフィックスAPIを切り替えると、Run()はウィンドウを
        // 閉じずに戻ってくる。その場合はエンジンを破棄して新しいAPIで作り直す。
        //
        // デバイスだけを差し替えるのではなくオブジェクトごと作り直しているのは、破棄の順序
        // (全リソース → スワップチェーン → デバイス → ウィンドウ)をC++のメンバ破棄順に
        // 任せられるため(KurenaiEngine3D.hのHasPendingGraphicsAPIChange付近のコメント参照)。
        // ウィンドウは作り直されるが、位置・サイズはwindow.iniを介して復元される
        Kurenai::GraphicsAPI api = ParseGraphicsAPI();
        uint32_t renderWidth = Kurenai::Defaults::RenderWidth;
        uint32_t renderHeight = Kurenai::Defaults::RenderHeight;
        {
            const std::wstring renderRes = ParseStringOption(L"-renderres");
            if (!renderRes.empty())
            {
                unsigned int w = 0u;
                unsigned int h = 0u;
                if (swscanf_s(renderRes.c_str(), L"%ux%u", &w, &h) == 2 && w > 0u && h > 0u)
                {
                    renderWidth = w;
                    renderHeight = h;
                    Kurenai::Core::Logger::Info(
                        "Main",
                        "内部レンダー解像度を起動オプションで設定しました: " + std::to_string(w) + "x" +
                            std::to_string(h));
                }
                else
                {
                    Kurenai::Core::Logger::Warning(
                        "Main",
                        "-renderresの指定が <幅>x<高さ> の形ではないため、既定のままにします: " +
                            Kurenai::Core::WideToUtf8(renderRes));
                }
            }
        }
        size_t sceneIndex = ParseInitialSceneIndex();
        const int debugViewIndex = ParseDebugViewIndex();
        const bool forceDDGIRaster = ParseForceDDGIRaster();
        const float ddgiBackfaceThreshold = ParseDDGIBackfaceThreshold();
        const uint32_t ddgiLODCount = ParseDDGILODCount();
        const bool ddgiFollowCamera = ParseDDGIFollowCamera();
        // -megalights <0=なし|1=参照実装> / -megalightsrays <本数。0で恒等テスト>。
        // どちらも指定が無ければ-1で、その項目は既定のままになる
        const int megaLightsMode = ParseIntOption(L"-megalights", -1);
        const int megaLightsShadowRays = ParseIntOption(L"-megalightsrays", -1);
        // -megalightssamples <M>。確率的サンプリングが1ピクセルあたりに候補プールから引く数
        const int megaLightsSamples = ParseIntOption(L"-megalightssamples", -1);
        // -megalightsaccum <枚数>。線形空間で足し込む枚数(0で蓄積しない)。
        // 指定した枚数で止まるので「ちょうどNサンプルの平均」を決定的に撮れる
        const int megaLightsAccumFrames = ParseIntOption(L"-megalightsaccum", -1);
        // -megalightsdump <パス>。蓄積し終えた平均を線形のまま生データで書き出す
        const std::wstring megaLightsDumpPath = ParseStringOption(L"-megalightsdump");
        // 空間再利用。-megalightsspatial <0|1> / -megalightsspatialneighbors <k> /
        // -megalightsspatialradius <ピクセル>
        const int megaLightsSpatial = ParseIntOption(L"-megalightsspatial", -1);
        const int megaLightsSpatialNeighbors = ParseIntOption(L"-megalightsspatialneighbors", -1);
        const int megaLightsSpatialRadius = ParseIntOption(L"-megalightsspatialradius", -1);
        // -megalightsspatialiters <回数>。空間再利用を何回繰り返すか
        const int megaLightsSpatialIterations = ParseIntOption(L"-megalightsspatialiters", -1);
        // -megalightsspatialmis <0=confidence重み|1=生成化バランスヒューリスティック>
        const int megaLightsSpatialMIS = ParseIntOption(L"-megalightsspatialmis", -1);
        // -megalightsinitialvis <0|1>。初期サンプルの可視レイ(遮蔽されたサンプルを殺す)の有無
        const int megaLightsInitialVis = ParseIntOption(L"-megalightsinitialvis", -1);
        // クアッド共有(-megalights 3)の設定。
        // -megalightsquadshare <0|1> は2x2の仲間が撃ったレイの結果を借りるか。
        // **0が陽性対照** ―― 手法2から時間・空間再利用を外した構成と画素単位で一致するはず。
        // -megalightsquadstratify <0|1> はクアッドの4画素へ候補スロットを分けて引かせるか。
        // -megalightsblockedcache <0|1> は遮蔽が確定した灯のキャッシュを使うか(陽性対照では0)
        const int megaLightsQuadShare = ParseIntOption(L"-megalightsquadshare", -1);
        const int megaLightsQuadStratify = ParseIntOption(L"-megalightsquadstratify", -1);
        const int megaLightsBlockedCache = ParseIntOption(L"-megalightsblockedcache", -1);
        // -megalightsquadsamples <1〜16>。クアッド共有が1画素あたりに引く標本の数。
        // 影レイの本数がそのままこの数になるので、コストはほぼ比例して増える
        const int megaLightsQuadSamples = ParseIntOption(L"-megalightsquadsamples", -1);
        // ブースト標本数Bと対象モード(1=予測棄却、2=短い履歴も対象、3=全画素・検算専用)
        const int megaLightsQuadBoost = ParseIntOption(L"-megalightsquadboost", -1);
        const int megaLightsQuadBoostMode = ParseIntOption(L"-megalightsquadboostmode", -1);
        // -megalightspool <8〜512>。候補プールが1タイルあたりに抽出する灯の数(K)。
        // 1画素あたりの標本数では減らない「タイル間」のノイズがここで決まる
        const int megaLightsPoolCapacity = ParseIntOption(L"-megalightspool", -1);
        // -megalightstilejitter <0|1|2>。1=Halton(2,3)で格子をずらす、2=有効だがオフセット0固定
        const int megaLightsTileJitter = ParseIntOption(L"-megalightstilejitter", -1);
        // -megalightspoolbilinear <0|1|2>。候補プールの確率的バイリニア参照。
        // 0=自分のタイル固定(従来)、1=2x2クアッドごとに1タイル、2=画素ごとに1タイル
        const int megaLightsPoolBilinear = ParseIntOption(L"-megalightspoolbilinear", -1);
        // -megalightstemporal <0|1> / -megalightstemporalmclamp <上限>。時間再利用
        const int megaLightsTemporal = ParseIntOption(L"-megalightstemporal", -1);
        const int megaLightsTemporalMClamp = ParseIntOption(L"-megalightstemporalmclamp", -1);
        // -megalightsperturb <0|1|2>。【検証専用】蓄積開始時の摂動
        // (1=全ライトを消す / 2=露出を+2段跳ばす)。時間再利用の追従を測るためのもの
        const int megaLightsPerturb = ParseIntOption(L"-megalightsperturb", -1);
        // -megalightsdenoise <0|1> / -megalightsdenoiseatrous <段数> /
        // -megalightsdenoiseframes <上限>。デノイザ(時間累積 + a-trous)
        const int megaLightsDenoise = ParseIntOption(L"-megalightsdenoise", -1);
        const int megaLightsDenoiseAtrous = ParseIntOption(L"-megalightsdenoiseatrous", -1);
        const int megaLightsDenoiseFrames = ParseIntOption(L"-megalightsdenoiseframes", -1);
        // -emissivelights <0|1>。自発光メッシュを光源として扱うか(既定は無効)。
        // -emissivelightscutoff <τ> は打ち切り照度、-emissivelightsmax <N> は採用数の上限。
        // -emissivelightsddgi <0|1> はDDGIにも自発光を加算するか(=二重に数えるか。既定は0で抑止)
        const int emissiveLights = ParseIntOption(L"-emissivelights", -1);
        const float emissiveLightsCutoff = ParseFloatOption(L"-emissivelightscutoff", -1.0f);
        const int emissiveLightsMax = ParseIntOption(L"-emissivelightsmax", -1);
        const int emissiveLightsDDGI = ParseIntOption(L"-emissivelightsddgi", -1);
        // -meshlights <0|1>。段階2。発光面を三角形のまま面積分する(既定は無効)。
        // MegaLights 経路でのみ効き、有効なフレームは参照実装が段階1のプロキシ(型3)を
        // 読み飛ばして三角形を積む。**いまは全三角形総当たりの参照実装しか無い**ので
        // 実シーンでは回らない(小さな専用シーン用)
        const int meshLights = ParseIntOption(L"-meshlights", -1);
        // -emissiveintensity <倍率>。シーン全体の自発光の強度(ImGuiの同名スライダと同じ値)。
        // glTFのemissiveFactorは[0,1]に収まるため、既定の1.0では小さな器具が1階調に届かない
        const float emissiveIntensity = ParseFloatOption(L"-emissiveintensity", -1.0f);        // -megalightsdenoisesigma <値>。輝度のエッジ停止の強さ(SVGFのσ_l)
        const float megaLightsDenoiseSigma = ParseFloatOption(L"-megalightsdenoisesigma", -1.0f);
        // -megalightsfirefly <k>。ファイアフライの近傍クランプの強さ(0で無効)
        const float megaLightsFireflyClamp = ParseFloatOption(L"-megalightsfirefly", -1.0f);
        // -megalightsdenoisecatmull <0|1>。デノイザの時間累積が履歴の色を引くときの
        // 再サンプリング。0=バイリニア(従来) / 1=Catmull-Rom。
        // バイリニアだと毎フレーム補間が重なって移動中の鮮鋭さが累積的に失われる
        const int megaLightsDenoiseCatmull = ParseIntOption(L"-megalightsdenoisecatmull", -1);
        // -megalightsdenoise4tap <0|1>。履歴の妥当性を2x2の4タップで判定するか。
        // 従来は最近傍1点だけで見ており、1点がシルエットの向こう側だと履歴全体を棄却していた
        const int megaLightsDenoise4Tap = ParseIntOption(L"-megalightsdenoise4tap", -1);
        // -megalightsdenoiseantilag <0|1> と、そのしきい値 t0 / t1(相対変化の両端)/
        // fast(短い EMA の長さ[フレーム])。変化した画素だけ時間累積の上限を短く落とし、
        // 灯を消したあとの残光を縮める。値は 0 以下なら既定のまま
        // (根拠は EngineDefaults.h の MegaLightsDenoiseAntiLag)
        const int megaLightsAntiLag = ParseIntOption(L"-megalightsdenoiseantilag", -1);
        const float megaLightsAntiLagT0 = ParseFloatOption(L"-megalightsdenoiseantilagt0", -1.0f);
        const float megaLightsAntiLagT1 = ParseFloatOption(L"-megalightsdenoiseantilagt1", -1.0f);
        const int megaLightsAntiLagFast = ParseIntOption(L"-megalightsdenoiseantilagfast", -1);
        // -perfdump <パス> / -perfdumpframes <枚数>。GPUの区間計測を平均してCSVへ書き出す。
        // Perfログは0.05ms未満を落とし1フレームの代表値しか出さないので、性能測定には使えない
        const std::wstring perfDumpPath = ParseStringOption(L"-perfdump");
        const int perfDumpFrames = ParseIntOption(L"-perfdumpframes", 120);
        // -passmanifest <パス>。RenderGraph の登録順と実行順を比較用テキストへ書き出す。
        const std::wstring passManifestPath = ParseStringOption(L"-passmanifest");
        int passManifestFrames = ParseIntOption(L"-passmanifestframes", 1);
        if (passManifestFrames < 1)
        {
            Kurenai::Core::Logger::Warning("Main", "-passmanifestframes は1以上で指定します。1に丸めます");
            passManifestFrames = 1;
        }
        // -dumptex <名前> <パス> (繰り返し可) / -dumptexmip <N> / -dumptexslice <N> /
        // -dumpframe <N> / -exitafterdump。中間レンダーターゲットを線形の生値で書き出す。
        // 「コンパイルは通るが絵が違う」を、8bitのスクリーンショットではなく数値で切り分けるための経路
        const std::vector<TextureDumpArg> textureDumps = ParseTextureDumps();
        const std::vector<BufferDumpArg> bufferDumps = ParseBufferDumps();
        // -recreate <フレーム> <renderres=<幅>x<高さ>|upscale=<幅>x<高さ>|precision=hdr|precision=legacy8bit|scene=<名前>>
        // (繰り返し可)。GPUリソースの作り直し経路を指定フレームで無人検証する。
        const std::vector<Kurenai::ScheduledRecreation> scheduledRecreations = ParseScheduledRecreations();
        const int textureDumpFrame = ParseIntOption(L"-dumpframe", -1);
        const bool exitAfterDump = HasFlagOption(L"-exitafterdump");
        // -taa の読み取りは下の `taa` で行う(ダンプの比較でも同じ指定を使う)

        // -autoexposure <0|1>。自動露出の有効/無効。指定が無ければ既定のまま。
        // 画面で見ていた設定と計測の設定を揃えるために要る(UIからしか切り替えられないと、
        // 手法の差と設定の差を分けられない)
        const int autoExposure = ParseIntOption(L"-autoexposure", -1);
        // -occlusioncull 0|1。Hi-Zオクルージョンカリングの有無。カリングは保守的で
        // なければならないので、有無で絵が1画素も変わらないことが正しさの定義になる。
        // その突き合わせをUIのチェックボックスでやると撮影ごとに操作を再現できない
        const int occlusionCull = ParseIntOption(L"-occlusioncull", -1);
        constexpr int kMissingValidationOption = (std::numeric_limits<int>::min)();
        // -aotechnique: 0=SSAO、1=SSIL(Visibility Bitmask)、2=Raytraced AO。
        const int aoTechnique = ParseIntOption(L"-aotechnique", kMissingValidationOption);
        const int softwareRaster = ParseIntOption(L"-swraster", kMissingValidationOption);
        const int ddgiHalfResolution = ParseIntOption(L"-ddgihalfres", kMissingValidationOption);
        // -probeupdate: 0=Baked、1=OnDemand、2=Realtime。
        const int probeUpdate = ParseIntOption(L"-probeupdate", kMissingValidationOption);
        const int upscale = ParseIntOption(L"-upscale", kMissingValidationOption);
        constexpr float kMissingFixedTimeStep = (std::numeric_limits<float>::lowest)();
        const float fixedTimeStep = ParseFloatOption(L"-fixedstep", kMissingFixedTimeStep);
        // -taa 0|1。TAAは時間方向に蓄積するため、画素単位の一致を測るときは切る
        const int taa = ParseIntOption(L"-taa", -1);
        // -camerapath <名前>。.ksceneの[CameraPath]を1本選んで再生する。
        // 【計測専用】カメラを動かしたときのノイズと遅れを測るには同じ軌跡を再現する必要があるが、
        // 通常の操作は移動量がΔtに比例し、視点回転はPostMessageから駆動できない。
        // 再生中は視点の入力操作を受け付けず、フレーム番号だけから姿勢が決まる。
        // -fixedstep の指定が無ければ 1/60 が自動で入る(警告を出したうえで)
        const std::wstring cameraPathName = ParseStringOption(L"-camerapath");
        // -camerapathstart <N>。経路の再生を始めるフレーム。既定は整定待ちの180
        // (-dumpframe の既定と同じ定数を共有する)
        const int cameraPathStart = ParseIntOption(L"-camerapathstart", -1);
        // -camerapathvalidate。経路が本当に画面を動かすかの検算ログだけを出す
        const bool cameraPathValidate = HasFlagOption(L"-camerapathvalidate");
        // -meshlet 0|1。メッシュレット描画の有無。切ると従来の頂点シェーダー経路へ落ち、
        // メッシュレット単位のカリングが一切かからない。**両経路の絵は一致するのが正しい**
        // ので、これが「増幅シェーダーが何か落としていないか」を見るときの基準になる
        const int meshlet = ParseIntOption(L"-meshlet", -1);
        // -renderres <幅>x<高さ>。内部レンダー解像度。タイルライトカリングと
        // MegaLightsの候補プールは16レンダー画素のタイルなので、解像度が違うと
        // タイルと形状の噛み合いが変わる。比較する2回は必ず揃えること

        for (;;)
        {
            Kurenai::KurenaiEngine3D engine(api, renderWidth, renderHeight, sceneIndex);
            // グラフィックスAPIを切り替えて作り直したときも同じ表示で見たいので毎回適用する
            if (debugViewIndex >= 0)
            {
                engine.SetDebugViewIndex(debugViewIndex);
            }
            if (autoExposure >= 0)
            {
                engine.SetAutoExposureEnabled(autoExposure != 0);
            }
            if (occlusionCull >= 0)
            {
                engine.SetOcclusionCullingEnabled(occlusionCull != 0);
            }
            if (aoTechnique != kMissingValidationOption)
            {
                if (aoTechnique < 0 || aoTechnique > 2)
                {
                    Kurenai::Core::Logger::Error("Main", "-aotechnique の値が範囲外です: " + std::to_string(aoTechnique));
                }
                else
                {
                    engine.SetAOTechnique(aoTechnique);
                }
            }
            if (softwareRaster != kMissingValidationOption)
            {
                if (softwareRaster != 0 && softwareRaster != 1)
                {
                    Kurenai::Core::Logger::Error("Main", "-swraster の値が不正です: " + std::to_string(softwareRaster));
                }
                else
                {
                    engine.SetSoftwareRasterEnabled(softwareRaster != 0);
                }
            }
            if (ddgiHalfResolution != kMissingValidationOption)
            {
                if (ddgiHalfResolution != 0 && ddgiHalfResolution != 1)
                {
                    Kurenai::Core::Logger::Error("Main", "-ddgihalfres の値が不正です: " + std::to_string(ddgiHalfResolution));
                }
                else
                {
                    engine.SetDDGIHalfResolutionEnabled(ddgiHalfResolution != 0);
                }
            }
            if (probeUpdate != kMissingValidationOption)
            {
                if (probeUpdate < 0 || probeUpdate > 2)
                {
                    Kurenai::Core::Logger::Error("Main", "-probeupdate の値が範囲外です: " + std::to_string(probeUpdate));
                }
                else
                {
                    engine.SetProbeUpdateMode(probeUpdate);
                }
            }
            if (upscale != kMissingValidationOption)
            {
                if (upscale != 0 && upscale != 1)
                {
                    Kurenai::Core::Logger::Error("Main", "-upscale の値が不正です: " + std::to_string(upscale));
                }
                else
                {
                    engine.SetUpscaleEnabled(upscale != 0);
                }
            }
            if (fixedTimeStep != kMissingFixedTimeStep)
            {
                if (!std::isfinite(fixedTimeStep) || fixedTimeStep <= 0.0f)
                {
                    Kurenai::Core::Logger::Error("Main", "-fixedstep の値が不正です: " + std::to_string(fixedTimeStep));
                }
                else
                {
                    engine.SetFixedTimeStep(fixedTimeStep);
                }
            }
            if (taa >= 0)
            {
                engine.SetTAAEnabled(taa != 0);
            }
            // 【開始フレームを先に設定すること】SelectCameraPath が「開始フレーム」を
            // ログへ出すので、後に回すと出る値と実際に効く値が食い違う
            if (cameraPathStart >= 0)
            {
                engine.SetCameraPathStartFrame(cameraPathStart);
            }
            if (cameraPathValidate)
            {
                engine.SetCameraPathValidate(true);
            }
            if (!cameraPathName.empty())
            {
                engine.SelectCameraPath(cameraPathName.c_str());
            }
            if (meshlet >= 0)
            {
                engine.SetMeshletRenderingEnabled(meshlet != 0);
            }
            if (forceDDGIRaster)
            {
                engine.ForceDDGIRayModeRaster();
            }
            if (ddgiBackfaceThreshold >= 0.0f)
            {
                engine.SetDDGIBackfaceThreshold(ddgiBackfaceThreshold);
            }
            if (ddgiLODCount > 0u || ddgiFollowCamera)
            {
                // 段数に0を渡すと「.ksceneの指定のまま」で、追従だけを切り替える
                engine.OverrideDDGILOD(ddgiLODCount, ddgiFollowCamera);
            }
            if (megaLightsMode >= 0 || megaLightsShadowRays >= 0 || megaLightsSamples > 0)
            {
                // 負の値は「既定のまま」。手法だけ・本数だけ・M だけの指定もできる
                engine.OverrideMegaLights(megaLightsMode, megaLightsShadowRays, megaLightsSamples);
            }
            if (megaLightsAccumFrames >= 0)
            {
                engine.SetMegaLightsAccumFrames(megaLightsAccumFrames);
            }
            if (!megaLightsDumpPath.empty())
            {
                engine.SetMegaLightsDumpPath(megaLightsDumpPath.c_str());
            }
            if (megaLightsSpatial >= 0 || megaLightsSpatialNeighbors >= 0 || megaLightsSpatialRadius > 0 ||
                megaLightsSpatialMIS >= 0)
            {
                engine.SetMegaLightsSpatial(
                    megaLightsSpatial, megaLightsSpatialNeighbors, megaLightsSpatialRadius,
                    megaLightsSpatialMIS);
            }
            if (megaLightsInitialVis >= 0)
            {
                engine.SetMegaLightsInitialVisibility(megaLightsInitialVis);
            }
            if (megaLightsQuadShare >= 0 || megaLightsQuadStratify >= 0 || megaLightsBlockedCache >= 0)
            {
                engine.SetMegaLightsQuadShare(
                    megaLightsQuadShare, megaLightsQuadStratify, megaLightsBlockedCache);
            }
            if (megaLightsQuadSamples >= 0)
            {
                engine.SetMegaLightsQuadSamples(megaLightsQuadSamples);
            }
            if (megaLightsQuadBoost >= 0 || megaLightsQuadBoostMode >= 0)
            {
                engine.SetMegaLightsQuadBoost(megaLightsQuadBoost, megaLightsQuadBoostMode);
            }
            if (megaLightsPoolCapacity >= 0)
            {
                engine.SetMegaLightsTilePoolCapacity(megaLightsPoolCapacity);
            }
            // 未指定時も呼び、既定の無効状態を起動ログへ1行残す
            engine.SetMegaLightsTileJitter(megaLightsTileJitter);
            engine.SetMegaLightsTilePoolBilinear(megaLightsPoolBilinear);
            if (megaLightsTemporal >= 0 || megaLightsTemporalMClamp > 0)
            {
                engine.SetMegaLightsTemporal(megaLightsTemporal, megaLightsTemporalMClamp);
            }
            if (megaLightsPerturb >= 0)
            {
                engine.SetMegaLightsPerturb(megaLightsPerturb);
            }
            if (megaLightsDenoise >= 0 || megaLightsDenoiseAtrous >= 0 || megaLightsDenoiseFrames > 0)
            {
                engine.SetMegaLightsDenoise(
                    megaLightsDenoise, megaLightsDenoiseAtrous, megaLightsDenoiseFrames);
            }
            if (emissiveLights >= 0 || emissiveLightsCutoff > 0.0f || emissiveLightsMax > 0
                || emissiveLightsDDGI >= 0)
            {
                // 有効/無効を指定していない(負)なら、しきい値だけ差し替えて状態は既定のまま
                engine.SetEmissiveLights(
                    emissiveLights, emissiveLightsCutoff, emissiveLightsMax, emissiveLightsDDGI);
            }
            if (emissiveIntensity > 0.0f)
            {
                engine.SetEmissiveIntensity(emissiveIntensity);
            }
            if (meshLights >= 0)
            {
                engine.SetMeshLights(meshLights);
            }            if (megaLightsDenoiseSigma > 0.0f)
            {
                engine.SetMegaLightsDenoiseSigmaLuminance(megaLightsDenoiseSigma);
            }
            if (megaLightsFireflyClamp >= 0.0f)
            {
                engine.SetMegaLightsDenoiseFireflyClamp(megaLightsFireflyClamp);
            }
            if (megaLightsDenoiseCatmull >= 0)
            {
                engine.SetMegaLightsDenoiseHistoryCatmullRom(megaLightsDenoiseCatmull != 0);
            }
            if (megaLightsDenoise4Tap >= 0)
            {
                engine.SetMegaLightsDenoiseHistory4Tap(megaLightsDenoise4Tap != 0);
            }
            if (megaLightsAntiLag >= 0 || megaLightsAntiLagT0 > 0.0f || megaLightsAntiLagT1 > 0.0f ||
                megaLightsAntiLagFast > 0)
            {
                engine.SetMegaLightsDenoiseAntiLag(
                    megaLightsAntiLag, megaLightsAntiLagT0, megaLightsAntiLagT1, megaLightsAntiLagFast);
            }
            if (megaLightsSpatialIterations > 0)
            {
                engine.SetMegaLightsSpatialIterations(megaLightsSpatialIterations);
            }
            if (!perfDumpPath.empty())
            {
                engine.SetPerfDump(perfDumpPath.c_str(), perfDumpFrames);
            }
            if (!passManifestPath.empty())
            {
                engine.SetPassManifest(passManifestPath.c_str(), passManifestFrames);
            }
            // 【ループの中で適用する】APIを切り替えて作り直したときも同じ指定が効くようにする
            // (debugViewIndexを毎回適用しているのと同じ理由)
            for (const TextureDumpArg& dump : textureDumps)
            {
                engine.AddTextureDump(
                    dump.Name.c_str(), dump.Path.c_str(), dump.MipLevel, dump.ArraySlice, dump.Frames, dump.Stride);
            }
            for (const BufferDumpArg& dump : bufferDumps)
            {
                engine.AddBufferDump(dump.Name.c_str(), dump.Path.c_str());
            }
            for (const Kurenai::ScheduledRecreation& recreation : scheduledRecreations)
            {
                engine.AddScheduledRecreation(recreation);
            }
            if (!textureDumps.empty() || !bufferDumps.empty() || (!passManifestPath.empty() && passManifestFrames == 1))
            {
                engine.SetTextureDumpFrame(textureDumpFrame);
            }
            if (!textureDumps.empty() || !bufferDumps.empty())
            {
                engine.SetExitAfterDump(exitAfterDump);
            }
            engine.Run();

            if (!engine.HasPendingGraphicsAPIChange())
            {
                break;
            }

            // 切り替え先のAPIと、作り直しても保ちたい状態(内部レンダー解像度・シーン)を引き継ぐ。
            // それ以外の設定は新しいインスタンスで既定値に戻る
            api = engine.GetPendingGraphicsAPI();
            renderWidth = engine.GetRenderWidth();
            renderHeight = engine.GetRenderHeight();
            sceneIndex = engine.GetCurrentSceneIndex();
        }
    }
    catch (const SceneNotResolvedError&)
    {
        // 【MessageBoxを出さない】ログには既に出してある。無人実行が
        // ダイアログで止まらないよう、終了コードだけで失敗を伝える
        exitCode = 1;
    }
    catch (const std::exception& e)
    {
        std::ofstream log("error.log", std::ios::app);
        log << e.what() << std::endl;

        MessageBoxW(nullptr, Utf8ToWide(e.what()).c_str(), L"Kurenai Engine - 初期化エラー", MB_OK | MB_ICONERROR);
        exitCode = 1;
    }

    if (SUCCEEDED(comResult))
    {
        CoUninitialize();
    }

    return exitCode;
}
