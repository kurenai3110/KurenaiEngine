#include <Windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <cstdint>
#include <exception>
#include <fstream>
#include <string>

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
        size_t sceneIndex = 0;

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
