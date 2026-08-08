#include <Windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "Assets/SceneLoader.h"
#include "Assets/ShowLoader.h"
#include "Core/Logger.h"
#include "Core/StringUtil.h"
#include "KurenaiEngine3D.h"
#include "KurenaiTypes.h"
#include "ShowEditor.h"

// KurenaiShowEditor: ドローンショー(.kshow)のオーサリングツール。
//
// 2つの使い方がある。
//   KurenaiShowEditor.exe [-dx12] [-scene <名前>] [-show <パス>]
//       エンジンを起動し、「ドローンショー編集」ウィンドウを足したGUIで編集する。
//       プレビューはエンジンの本番と同じ経路(トーンマップ・ブルーム・露出)を通る
//   KurenaiShowEditor.exe --generate-standard <出力パス>
//       ウィンドウを開かず、標準の6形状のショーを書き出す。往復(書いて読み直す)まで
//       検査して終了する。Assets/Packed/Shows/Standard.kshowはこれで作った
//
// 【コンソールサブシステムである理由】--generate-standardの結果を標準出力へ返すため。
// GUIで起動したときはコンソールが1枚余分に出るが、これはツールなので許容する

namespace
{
    using Kurenai::Core::Utf8ToWide;
    using Kurenai::Core::WideToUtf8;

    // 【printfの書式文字列に日本語を置かないこと】/utf-8 を渡していても書式文字列の検査は
    // ANSIコードページでバイト列を読むらしく、日本語を含めると C4819(現在のコードページで
    // 表示できない文字)が出る。そのうえ %s や %zu の切り出しまでずれて C4474/C4477
    // (引数の数・型の不一致)が続く。実行時の出力は正しいので気付きにくい。
    // 文字列はここで組み立ててから、書式解析を通らないfputsで出す
    void Print(const std::string& message)
    {
        std::fputs(message.c_str(), stdout);
        std::fputs("\n", stdout);
    }

    std::wstring GetExeDirectory()
    {
        wchar_t buffer[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (length == 0 || length == MAX_PATH)
        {
            return L"";
        }
        const std::wstring path(buffer, length);
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? L"" : path.substr(0, slash + 1);
    }

    // コマンドライン引数を素朴に取り出す。見つからなければ空文字列
    std::wstring FindOption(const std::vector<std::wstring>& args, const wchar_t* name)
    {
        for (size_t i = 1; i + 1 < args.size(); ++i)
        {
            if (_wcsicmp(args[i].c_str(), name) == 0)
            {
                return args[i + 1];
            }
        }
        return L"";
    }

    bool HasFlag(const std::vector<std::wstring>& args, const wchar_t* name)
    {
        for (size_t i = 1; i < args.size(); ++i)
        {
            if (_wcsicmp(args[i].c_str(), name) == 0)
            {
                return true;
            }
        }
        return false;
    }

    // -scene <名前>(拡張子を除いたファイル名)を、KurenaiEngine3Dが構築するシーン一覧上の
    // 番号へ解決する。一覧の作り方(列挙→_wcsicmpで昇順ソート→ReadSceneNameが成功したものだけ
    // 採用)はKurenaiEngine3D::DiscoverScenes()と厳密に一致させる必要がある
    // (手順がずれると番号が一覧側とずれ、意図と別のシーンが開いてしまう)
    size_t ResolveSceneIndex(const std::wstring& requestedName)
    {
        if (requestedName.empty())
        {
            return 0;
        }

        const std::wstring sceneDirectory = GetExeDirectory() + L"Assets\\Scenes\\";
        std::vector<std::wstring> fileNames;
        WIN32_FIND_DATAW findData{};
        HANDLE findHandle = FindFirstFileW((sceneDirectory + L"*.kscene").c_str(), &findData);
        if (findHandle == INVALID_HANDLE_VALUE)
        {
            Kurenai::Core::Logger::Warning(
                "ShowEditor", "シーンフォルダを開けませんでした: " + WideToUtf8(sceneDirectory));
            return 0;
        }
        do
        {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                fileNames.push_back(findData.cFileName);
            }
        } while (FindNextFileW(findHandle, &findData));
        FindClose(findHandle);

        std::sort(fileNames.begin(), fileNames.end(), [](const std::wstring& a, const std::wstring& b)
        {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });

        size_t resolvedIndex = 0;
        for (const std::wstring& fileName : fileNames)
        {
            try
            {
                // 戻り値は使わない。成功したものだけを番号に数えるための呼び出し
                Kurenai::Assets::ReadSceneName(sceneDirectory + fileName);
            }
            catch (const std::exception&)
            {
                continue;
            }

            const size_t dot = fileName.find_last_of(L'.');
            const std::wstring stem = dot == std::wstring::npos ? fileName : fileName.substr(0, dot);
            if (_wcsicmp(stem.c_str(), requestedName.c_str()) == 0)
            {
                return resolvedIndex;
            }
            ++resolvedIndex;
        }

        Kurenai::Core::Logger::Warning(
            "ShowEditor", "指定されたシーンが見つかりませんでした: " + WideToUtf8(requestedName));
        return 0;
    }

    // 標準の6形状を書き出し、読み直して同じ内容になることまで確かめる。
    // 【書いて終わりにしない】書き出しの誤りは、次に読んだときに初めて分かる種類の
    // 失敗である(エディタで作った形が本番で崩れる、など)。ここで往復まで見る
    int GenerateStandard(const std::wstring& outputPath)
    {
        constexpr uint32_t kStandardDroneCount = 1500u;
        try
        {
            const Kurenai::Assets::ShowData source = Kurenai::ShowEditor::BuildStandardShow(kStandardDroneCount);
            Kurenai::Assets::SaveShow(outputPath, source);

            const Kurenai::Assets::ShowData reloaded = Kurenai::Assets::LoadShow(outputPath);
            if (reloaded.DroneCount != source.DroneCount ||
                reloaded.Formations.size() != source.Formations.size())
            {
                Print("往復の検査に失敗しました: 機体数または編隊数が一致しません");
                return 1;
            }
            for (size_t f = 0; f < source.Formations.size(); ++f)
            {
                if (reloaded.Formations[f].Name != source.Formations[f].Name)
                {
                    Print("往復の検査に失敗しました: 編隊" + std::to_string(f) + "の名前が一致しません");
                    return 1;
                }
                for (uint32_t i = 0; i < source.DroneCount; ++i)
                {
                    const DirectX::XMFLOAT3& a = source.Formations[f].Positions[i];
                    const DirectX::XMFLOAT3& b = reloaded.Formations[f].Positions[i];
                    const DirectX::XMFLOAT3& ca = source.Formations[f].Colors[i];
                    const DirectX::XMFLOAT3& cb = reloaded.Formations[f].Colors[i];
                    // floatをそのまま書いてそのまま読むので、ビット一致するのが正しい
                    if (a.x != b.x || a.y != b.y || a.z != b.z || ca.x != cb.x || ca.y != cb.y || ca.z != cb.z)
                    {
                        Print(
                            "往復の検査に失敗しました: 編隊" + std::to_string(f) + "の点" +
                            std::to_string(i) + "が一致しません");
                        return 1;
                    }
                }
            }

            Print(
                "書き出しました: " + WideToUtf8(outputPath) + " (機体数 " +
                std::to_string(source.DroneCount) + ", 編隊 " +
                std::to_string(source.Formations.size()) + ", 往復一致)");
            return 0;
        }
        catch (const std::exception& e)
        {
            Print(std::string("失敗しました: ") + e.what());
            return 1;
        }
    }
}

int wmain(int argc, wchar_t** argv)
{
    const std::vector<std::wstring> args(argv, argv + argc);

    if (HasFlag(args, L"--generate-standard"))
    {
        std::wstring outputPath = FindOption(args, L"--generate-standard");
        if (outputPath.empty())
        {
            Print("--generate-standard の後に出力パスを指定してください");
            return 1;
        }
        return GenerateStandard(outputPath);
    }

    // DirectXTexのWICテクスチャ読み込みがCOMを使用するため初期化しておく
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int exitCode = 0;
    try
    {
        const Kurenai::GraphicsAPI api =
            HasFlag(args, L"-dx12") ? Kurenai::GraphicsAPI::DX12 : Kurenai::GraphicsAPI::DX11;

        // 既定はドローンショーのシーン。[DroneShow]Enabled=falseのシーンでは
        // プレビューが描かれないため、編集用に最初からそこを開く
        std::wstring sceneName = FindOption(args, L"-scene");
        if (sceneName.empty())
        {
            sceneName = L"DroneShow";
        }

        std::wstring showPath = FindOption(args, L"-show");
        if (showPath.empty())
        {
            showPath = GetExeDirectory() + L"Assets\\Shows\\Standard.kshow";
        }

        Kurenai::KurenaiEngine3D engine(api, Kurenai::Defaults::RenderWidth, Kurenai::Defaults::RenderHeight,
                                        ResolveSceneIndex(sceneName));
        // 【Run()の前に登録すること】Renderスレッドが走り出した後に差し替えると、
        // 描画中のstd::functionを書き換えることになる。
        // editorはRun()が戻るまで生きている必要があるのでスタックに置く
        Kurenai::ShowEditor::Editor editor(engine, showPath);
        engine.SetExtraImGuiCallback([&editor]() { editor.Draw(); });
        engine.Run();
    }
    catch (const std::exception& e)
    {
        Print(std::string("初期化エラー: ") + e.what());
        MessageBoxW(nullptr, Utf8ToWide(e.what()).c_str(), L"KurenaiShowEditor - 初期化エラー", MB_OK | MB_ICONERROR);
        exitCode = 1;
    }

    if (SUCCEEDED(comResult))
    {
        CoUninitialize();
    }
    return exitCode;
}
