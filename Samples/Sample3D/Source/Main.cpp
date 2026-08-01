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

        for (;;)
        {
            Kurenai::KurenaiEngine3D engine(api, renderWidth, renderHeight, sceneIndex);
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
