#include <Windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fstream>
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
                Kurenai::Core::Logger::Warning(
                    "Main", optionNameUtf8 + "の後に値が指定されていないため、既定のままにします");
                break;
            }
            wchar_t* end = nullptr;
            const long parsed = wcstol(argv[i + 1], &end, 10);
            if (end == argv[i + 1] || (end != nullptr && *end != 0))
            {
                Kurenai::Core::Logger::Warning(
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

    // コマンドラインの「-scene <名前>」(拡張子を除いたファイル名。例: MontSaintMichel)を、
    // KurenaiEngine3Dが構築するシーン一覧上の番号へ解決する。
    // 一覧の作り方(列挙→_wcsicmpで昇順ソート→Assets::ReadSceneNameが成功したものだけ採用)は
    // KurenaiEngine3D::DiscoverScenes()(KurenaiEngine3D.cpp)と厳密に一致させる必要がある
    // (手順がずれると番号が一覧側とずれ、意図と別のシーンが開いてしまう)。
    // 指定が無い/見つからない場合は0を返す(従来どおり一覧の先頭シーンで起動する)
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
            Kurenai::Core::Logger::Warning(
                "Main", "-sceneの後にシーン名が指定されていないため、既定のシーンで起動します");
            return 0;
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
            Kurenai::Core::Logger::Warning(
                "Main", "実行ファイルのパス取得に失敗したため、既定のシーンで起動します");
            return 0;
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
            Kurenai::Core::Logger::Warning(
                "Main",
                "シーンフォルダを開けなかったため、既定のシーンで起動します (" +
                    Kurenai::Core::WideToUtf8(sceneDirectory) + ")");
            return 0;
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

        Kurenai::Core::Logger::Warning(
            "Main",
            "指定されたシーンが見つからなかったため、既定のシーンで起動します: " +
                Kurenai::Core::WideToUtf8(requestedName));
        return 0;
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

        for (;;)
        {
            Kurenai::KurenaiEngine3D engine(api, renderWidth, renderHeight, sceneIndex);
            // グラフィックスAPIを切り替えて作り直したときも同じ表示で見たいので毎回適用する
            if (debugViewIndex >= 0)
            {
                engine.SetDebugViewIndex(debugViewIndex);
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
            if (megaLightsMode >= 0 || megaLightsShadowRays >= 0)
            {
                // どちらも負の値は「既定のまま」。手法だけ・本数だけの指定もできる
                engine.OverrideMegaLights(megaLightsMode, megaLightsShadowRays);
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
