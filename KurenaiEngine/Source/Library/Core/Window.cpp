#include "Window.h"

#include <backends/imgui_impl_win32.h>
#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>

#include "Core/Logger.h"
#include "Core/StringUtil.h"

// imgui_impl_win32.hは<windows.h>への依存を避けるためこの宣言を#if 0でコメントアウトしており、
// 呼び出し側でこの1行をコピーして前方宣言することが公式に案内されている
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Kurenai::Core
{
    namespace
    {
        const wchar_t* kWindowClassName = L"KurenaiEngineWindowClass";

        // ウィンドウ配置の保存先。imgui.iniと同じく、起動時の作業ディレクトリに依存させず
        // KurenaiEngine.dllと同じフォルダに固定する(サンプルごとに出力フォルダが分かれるため、
        // Sample2DとSample3Dで記録が混ざることもない)
        std::wstring GetPlacementFilePath()
        {
            return GetModuleDirectory() + L"window.ini";
        }

        // window.iniの内容。位置とサイズは物理ピクセル(復元後の通常状態の枠を含む矩形)で、
        // Dpiは保存した時点でウィンドウが載っていたモニタのDPI
        struct SavedPlacement
        {
            int Left = 0;
            int Top = 0;
            int Width = 0;
            int Height = 0;
            bool Maximized = false;
            unsigned int Dpi = 96;
        };

        // 「数値だけが書かれている」ことを確かめたうえで整数へ変換する。
        // 末尾の空白と改行(CRLFで書かれたファイルのCR)は許容する
        bool ParseInt(const std::string& text, int& outValue)
        {
            try
            {
                size_t consumed = 0;
                const int parsed = std::stoi(text, &consumed);
                for (size_t i = consumed; i < text.size(); ++i)
                {
                    if (!std::isspace(static_cast<unsigned char>(text[i])))
                    {
                        return false;
                    }
                }
                outValue = parsed;
                return true;
            }
            catch (const std::exception&)
            {
                // 数値として読めない(std::invalid_argument)・範囲外(std::out_of_range)
                return false;
            }
        }

        bool ReadSavedPlacement(SavedPlacement& outPlacement)
        {
            const std::wstring path = GetPlacementFilePath();

            // MSVCのfstreamはwchar_t*のパスを受け付ける(日本語や記号を含むパスでも開ける)
            std::ifstream file(path.c_str());
            if (!file)
            {
                // 初回起動時はファイルが無いのが正常なので、エラーではなく情報として記録する
                Logger::Info(
                    "Window",
                    "ウィンドウ配置の記録が無いため既定のサイズで起動します (" + WideToUtf8(path) + ")");
                return false;
            }

            SavedPlacement parsed{};
            bool hasLeft = false;
            bool hasTop = false;
            bool hasWidth = false;
            bool hasHeight = false;

            std::string line;
            while (std::getline(file, line))
            {
                if (line.empty() || line[0] == '#')
                {
                    continue;
                }

                const size_t separator = line.find('=');
                if (separator == std::string::npos)
                {
                    continue;
                }

                const std::string key = line.substr(0, separator);
                const std::string valueText = line.substr(separator + 1);

                int value = 0;
                if (!ParseInt(valueText, value))
                {
                    Logger::Warning(
                        "Window",
                        "ウィンドウ配置の記録に数値でない値があるため既定のサイズで起動します (" + key + ")");
                    return false;
                }

                if (key == "Left") { parsed.Left = value; hasLeft = true; }
                else if (key == "Top") { parsed.Top = value; hasTop = true; }
                else if (key == "Width") { parsed.Width = value; hasWidth = true; }
                else if (key == "Height") { parsed.Height = value; hasHeight = true; }
                else if (key == "Maximized") { parsed.Maximized = value != 0; }
                else if (key == "Dpi") { parsed.Dpi = static_cast<unsigned int>(value); }
            }

            if (!hasLeft || !hasTop || !hasWidth || !hasHeight)
            {
                Logger::Warning("Window", "ウィンドウ配置の記録に必要な項目が足りないため既定のサイズで起動します");
                return false;
            }

            // 手で編集された場合や書き込みが途中で切れた場合に備えて妥当性を確認する。
            // 200px未満だとタイトルバーすら掴めなくなるため壊れた値として扱う
            constexpr int kMinSize = 200;
            constexpr int kMaxSize = 32767;
            if (parsed.Width < kMinSize || parsed.Height < kMinSize ||
                parsed.Width > kMaxSize || parsed.Height > kMaxSize)
            {
                Logger::Warning(
                    "Window",
                    "ウィンドウ配置の記録のサイズが不正なため既定のサイズで起動します (" +
                        std::to_string(parsed.Width) + "x" + std::to_string(parsed.Height) + ")");
                return false;
            }

            // Windowsのディスプレイ拡大率は100%〜500%(96〜480 DPI)。多少の余裕を持たせて判定する
            constexpr unsigned int kMinDpi = 48;
            constexpr unsigned int kMaxDpi = 960;
            if (parsed.Dpi < kMinDpi || parsed.Dpi > kMaxDpi)
            {
                Logger::Warning(
                    "Window",
                    "ウィンドウ配置の記録のDPIが不正なため既定のサイズで起動します (" +
                        std::to_string(parsed.Dpi) + ")");
                return false;
            }

            outPlacement = parsed;
            return true;
        }
    }

    Window::Window(const std::wstring& title, uint32_t width, uint32_t height)
        : m_Width(width)
        , m_Height(height)
    {
        HINSTANCE instance = GetModuleHandleW(nullptr);

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(WNDCLASSEXW);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &Window::WndProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kWindowClassName;
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            throw std::runtime_error("ウィンドウクラスの登録に失敗しました (GetLastError: " + std::to_string(GetLastError()) + ")");
        }

        RECT windowRect{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

        m_Handle = CreateWindowExW(
            0,
            kWindowClassName,
            title.c_str(),
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            windowRect.right - windowRect.left,
            windowRect.bottom - windowRect.top,
            nullptr,
            nullptr,
            instance,
            this);

        if (!m_Handle)
        {
            throw std::runtime_error("ウィンドウの作成に失敗しました (GetLastError: " + std::to_string(GetLastError()) + ")");
        }

        // 自動操作テストのハーネス(外部プロセスからPostMessageで入力を注入する側)が起動前に
        // 設定する想定の環境変数。通常起動では未設定のため、WM_MOUSELEAVEの抑制ロジックは
        // 無効のままとなり実操作の挙動には一切影響しない
        wchar_t automationFlag[8]{};
        m_MouseLeaveSuppressionEnabled =
            GetEnvironmentVariableW(L"KURENAI_INPUT_AUTOMATION", automationFlag, static_cast<DWORD>(sizeof(automationFlag) / sizeof(automationFlag[0]))) > 0
            && automationFlag[0] == L'1';

        // 初期のDPIスケールを取得する。プロセスがDPI非対応の場合はGetDpiForWindowが常に96を
        // 返す仕様のため1.0になる
        const UINT dpi = GetDpiForWindow(m_Handle);
        if (dpi == 0)
        {
            Logger::Warning(
                "Window",
                "GetDpiForWindowに失敗したためDPIスケールを1.0として扱います (GetLastError: " +
                    std::to_string(GetLastError()) + ")");
        }
        else
        {
            m_DpiScale.store(static_cast<float>(dpi) / 96.0f, std::memory_order_relaxed);
        }

        // 前回終了時の位置・サイズ・最大化状態を復元してから表示する。
        // 表示前に配置を済ませることで、既定サイズのウィンドウが一瞬見えてから
        // 復元後のサイズへ飛ぶのを避ける
        const int showCommand = ApplySavedPlacement();
        ShowWindow(m_Handle, showCommand);

        // 復元や最大化でクライアント領域はコンストラクタ引数のサイズと変わるため、
        // WM_SIZE経由の更新を待たずここで実測して確定させる。この値がそのまま
        // スワップチェインの初期サイズになる(KurenaiEngineBaseのコンストラクタ)
        RECT clientRect{};
        if (GetClientRect(m_Handle, &clientRect))
        {
            const LONG clientWidth = clientRect.right - clientRect.left;
            const LONG clientHeight = clientRect.bottom - clientRect.top;
            if (clientWidth > 0 && clientHeight > 0)
            {
                m_Width.store(static_cast<uint32_t>(clientWidth), std::memory_order_relaxed);
                m_Height.store(static_cast<uint32_t>(clientHeight), std::memory_order_relaxed);
            }
        }
        else
        {
            Logger::Warning(
                "Window",
                "GetClientRectに失敗したため要求サイズをそのまま使います (GetLastError: " +
                    std::to_string(GetLastError()) + ")");
        }
    }

    Window::~Window()
    {
        if (m_Handle)
        {
            // DestroyWindow後はGetWindowPlacementが使えなくなるため、破棄する前に保存する
            SaveCurrentPlacement();
            DestroyWindow(m_Handle);

            // 【重要】DestroyWindowが同期的に送るWM_DESTROYの中でPostQuitMessage(0)を呼んでいるため、
            // この時点で「ウィンドウではなくスレッド」のメッセージキューにWM_QUITが1件積まれている。
            // 1プロセスの中でWindowを作り直す場合(グラフィックスAPIの実行時切り替え)、この残骸を
            // 次のWindowのPumpMessages()が拾ってm_ShouldCloseを立ててしまい、新しいウィンドウが
            // 開いた直後に終了する(実際に発生: DX12へ切り替えるとシーン読み込み完了の直後に落ちた)。
            // WM_QUITはウィンドウに紐づかずスレッドに属するため、破棄したウィンドウの後始末として
            // ここで取り除いておく。プロセスを終了する通常の経路では、この後もう誰も
            // メッセージを汲まないので取り除いても影響しない
            MSG quitMessage{};
            PeekMessageW(&quitMessage, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE);
        }
    }

    int Window::ApplySavedPlacement()
    {
        SavedPlacement saved{};
        if (!ReadSavedPlacement(saved))
        {
            return SW_SHOW;
        }

        // 復元した位置が使えなかったときに戻せるよう、CreateWindowExW直後の配置を控えておく
        RECT originalRect{};
        if (!GetWindowRect(m_Handle, &originalRect))
        {
            Logger::Warning(
                "Window",
                "GetWindowRectに失敗したため保存されたウィンドウ配置を復元しません (GetLastError: " +
                    std::to_string(GetLastError()) + ")");
            return SW_SHOW;
        }

        // showCmdにSW_HIDEを指定しても復元後の矩形(rcNormalPosition)は反映されるため、
        // ここでは位置とサイズだけを適用し、表示は呼び出し元のShowWindowに任せる
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        placement.showCmd = SW_HIDE;
        placement.rcNormalPosition = {
            saved.Left,
            saved.Top,
            saved.Left + saved.Width,
            saved.Top + saved.Height,
        };

        if (!SetWindowPlacement(m_Handle, &placement))
        {
            Logger::Warning(
                "Window",
                "SetWindowPlacementに失敗したため既定の位置・サイズで起動します (GetLastError: " +
                    std::to_string(GetLastError()) + ")");
            return SW_SHOW;
        }

        // 保存時からディスプレイの拡大率が変わっている場合、物理ピクセルのまま復元すると
        // 見かけの大きさが変わってしまう。UIの拡大率はDPIに追従する(GetDpiScale参照)ため、
        // ウィンドウだけ取り残されるとUIが窮屈になる。実際に載ったモニタのDPIとの比で補正する
        const UINT currentDpi = GetDpiForWindow(m_Handle);
        if (currentDpi != 0 && currentDpi != saved.Dpi)
        {
            const double ratio = static_cast<double>(currentDpi) / static_cast<double>(saved.Dpi);
            placement.rcNormalPosition.right = saved.Left + static_cast<LONG>(saved.Width * ratio + 0.5);
            placement.rcNormalPosition.bottom = saved.Top + static_cast<LONG>(saved.Height * ratio + 0.5);

            if (SetWindowPlacement(m_Handle, &placement))
            {
                Logger::Info(
                    "Window",
                    "保存時からディスプレイの拡大率が変わったためウィンドウサイズを補正しました (DPI: " +
                        std::to_string(saved.Dpi) + " -> " + std::to_string(currentDpi) + ")");
            }
            else
            {
                // 補正前の配置は適用済みなので、そのまま続行する
                Logger::Warning(
                    "Window",
                    "DPI補正後のSetWindowPlacementに失敗したため保存時のサイズのまま起動します (GetLastError: " +
                        std::to_string(GetLastError()) + ")");
            }
        }
        else if (currentDpi == 0)
        {
            Logger::Warning("Window", "GetDpiForWindowに失敗したためウィンドウサイズのDPI補正を行いません");
        }

        // モニタを外す・配置を変えるなどで、保存された位置が画面外になっていないか確認する。
        // rcNormalPositionはワークスペース座標(タスクバーを除いた領域が基準)でスクリーン座標と
        // 一致しないことがあるため、適用後のGetWindowRect(スクリーン座標)で判定する
        RECT appliedRect{};
        if (!GetWindowRect(m_Handle, &appliedRect))
        {
            Logger::Warning(
                "Window",
                "復元後のGetWindowRectに失敗したため画面外かどうかを確認できません (GetLastError: " +
                    std::to_string(GetLastError()) + ")");
            return saved.Maximized ? SW_SHOWMAXIMIZED : SW_SHOW;
        }

        if (MonitorFromRect(&appliedRect, MONITOR_DEFAULTTONULL) == nullptr)
        {
            Logger::Warning(
                "Window",
                "保存されたウィンドウ位置がどのモニタにも掛からないため既定の位置・サイズで起動します (" +
                    std::to_string(saved.Left) + ", " + std::to_string(saved.Top) + ")");

            SetWindowPos(
                m_Handle,
                nullptr,
                originalRect.left,
                originalRect.top,
                originalRect.right - originalRect.left,
                originalRect.bottom - originalRect.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return SW_SHOW;
        }

        return saved.Maximized ? SW_SHOWMAXIMIZED : SW_SHOW;
    }

    void Window::SaveCurrentPlacement() const
    {
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (!GetWindowPlacement(m_Handle, &placement))
        {
            Logger::Error(
                "Window",
                "GetWindowPlacementに失敗したためウィンドウ配置を保存できません (GetLastError: " +
                    std::to_string(GetLastError()) + ")");
            return;
        }

        SavedPlacement saved{};
        saved.Left = static_cast<int>(placement.rcNormalPosition.left);
        saved.Top = static_cast<int>(placement.rcNormalPosition.top);
        saved.Width = static_cast<int>(placement.rcNormalPosition.right - placement.rcNormalPosition.left);
        saved.Height = static_cast<int>(placement.rcNormalPosition.bottom - placement.rcNormalPosition.top);

        // 最小化したまま終了した場合、最小化状態そのものを保存すると次回もアイコン化した状態で
        // 起動してしまう。WPF_RESTORETOMAXIMIZEDを見て「元に戻したときの状態」を保存する
        saved.Maximized = placement.showCmd == SW_SHOWMAXIMIZED ||
            (placement.showCmd == SW_SHOWMINIMIZED && (placement.flags & WPF_RESTORETOMAXIMIZED) != 0);

        const UINT dpi = GetDpiForWindow(m_Handle);
        if (dpi == 0)
        {
            Logger::Warning(
                "Window",
                "GetDpiForWindowに失敗したためウィンドウ配置のDPIを96として保存します (GetLastError: " +
                    std::to_string(GetLastError()) + ")");
        }
        saved.Dpi = dpi != 0 ? dpi : 96;

        const std::wstring path = GetPlacementFilePath();
        std::ofstream file(path.c_str(), std::ios::trunc);
        if (!file)
        {
            Logger::Warning(
                "Window", "ウィンドウ配置の保存ファイルを開けませんでした (" + WideToUtf8(path) + ")");
            return;
        }

        file << "# KurenaiEngineが終了時に自動生成するウィンドウ配置の記録。\n";
        file << "# 削除すると次回は既定のサイズで起動する。位置とサイズは物理ピクセル。\n";
        file << "Left=" << saved.Left << "\n";
        file << "Top=" << saved.Top << "\n";
        file << "Width=" << saved.Width << "\n";
        file << "Height=" << saved.Height << "\n";
        file << "Maximized=" << (saved.Maximized ? 1 : 0) << "\n";
        file << "Dpi=" << saved.Dpi << "\n";
        file.flush();

        if (!file)
        {
            Logger::Warning(
                "Window", "ウィンドウ配置の書き込みに失敗しました (" + WideToUtf8(path) + ")");
            return;
        }

        Logger::Info(
            "Window",
            "ウィンドウ配置を保存しました (" + std::to_string(saved.Width) + "x" + std::to_string(saved.Height) +
                ", 最大化: " + (saved.Maximized ? "はい" : "いいえ") + ")");
    }

    void Window::SetTitle(const std::wstring& title)
    {
        SetWindowTextW(m_Handle, title.c_str());
    }

    void Window::PumpMessages()
    {
        // WasKeyPressed/WasMouseButtonPressedは「このPumpMessages呼び出し中に起きた押下」を返すため、
        // メッセージ処理の前にエッジフラグをクリアする(呼び出し側は1フレームにつき1回呼ぶ想定)
        std::fill(std::begin(m_KeyPressedEdge), std::end(m_KeyPressedEdge), false);
        std::fill(std::begin(m_MouseButtonPressedEdge), std::end(m_MouseButtonPressedEdge), false);

        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                m_ShouldClose = true;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void Window::ForwardQueuedMessagesToImGui()
    {
        std::vector<PendingWndProcMessage> messages;
        {
            std::lock_guard<std::mutex> lock(m_PendingImGuiMessagesMutex);
            messages.swap(m_PendingImGuiMessages);
        }

        // PostMessageによる自動操作(実カーソルはウィンドウ外にあることが多い)では、注入した
        // WM_MOUSEMOVEの直後にTrackMouseEvent(Window/ImGui双方が個別に登録している)が実カーソル位置に
        // 基づいてWM_MOUSELEAVEを生成してしまい、ImGui側のio.MousePosが-FLT_MAXへ戻されてクリック判定が
        // 成立しなくなることがある。このWM_MOUSELEAVEは注入したWM_MOUSEMOVEと同じバッチに載らず
        // 数フレーム遅れて届くこともあるため、直近kMouseLeaveSuppressionウィンドウ内にWM_MOUSEMOVEを
        // 転送していればWM_MOUSELEAVEは実カーソル基準の古い判定によるノイズとみなして転送しない
        // (実際に離れた場合も、新たなWM_MOUSEMOVEが来ない限りこの猶予はすぐ過ぎるため、後続の
        // WM_MOUSELEAVEは通常どおり転送される)
        constexpr auto kMouseLeaveSuppression = std::chrono::milliseconds(100);
        const auto now = std::chrono::steady_clock::now();

        for (const auto& message : messages)
        {
            if (message.Message == WM_MOUSEMOVE)
            {
                m_LastForwardedMouseMoveTime = now;
            }
            else if (message.Message == WM_MOUSELEAVE && (now - m_LastForwardedMouseMoveTime) < kMouseLeaveSuppression)
            {
                continue;
            }

            ImGui_ImplWin32_WndProcHandler(m_Handle, message.Message, message.WParam, message.LParam);
        }
    }

    bool Window::IsMouseButtonDown(MouseButton button) const
    {
        return m_MouseButtonDown[static_cast<size_t>(button)];
    }

    bool Window::WasMouseButtonPressed(MouseButton button) const
    {
        return m_MouseButtonPressedEdge[static_cast<size_t>(button)];
    }

    bool Window::IsKeyDown(KeyCode key) const
    {
        return key >= 0 && key < 256 && m_KeyDown[key];
    }

    bool Window::WasKeyPressed(KeyCode key) const
    {
        return key >= 0 && key < 256 && m_KeyPressedEdge[key];
    }

    LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        Window* window = nullptr;

        if (message == WM_NCCREATE)
        {
            auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            window = static_cast<Window*>(createStruct->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
            window->m_Handle = hwnd;
        }
        else
        {
            window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (window)
        {
            return window->HandleMessage(message, wParam, lParam);
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT Window::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        // ImGuiがマウス/キーボード入力を使う場合でも、リサイズ等のウィンドウ管理は
        // このエンジン側で引き続き処理する必要があるため、早期returnはしない。
        // ImGui_ImplWin32_WndProcHandlerはこのスレッド(Updateスレッド)から直接呼ばず、
        // キューに積んでForwardQueuedMessagesToImGui経由でRenderスレッドへ転送する
        // (ImGui::NewFrame()等と同時にImGuiの内部状態を触るとデータ競合になるため)
        {
            std::lock_guard<std::mutex> lock(m_PendingImGuiMessagesMutex);
            m_PendingImGuiMessages.push_back({ message, wParam, lParam });
        }

        switch (message)
        {
        case WM_CLOSE:
            m_ShouldClose = true;
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED)
            {
                m_Width = LOWORD(lParam);
                m_Height = HIWORD(lParam);
                if (m_ResizeCallback)
                {
                    m_ResizeCallback(m_Width, m_Height);
                }
            }
            return 0;

        case WM_DPICHANGED:
        {
            // ウィンドウの伸縮そのものはWindowsの既定の挙動に任せ、ここでは何もしない。
            // Per-Monitor V2では、DPIの違うモニタへ移る直前にWindowsがWM_GETDPISCALEDSIZEで
            // 「新しいDPIでのウィンドウの希望サイズ」を尋ねてくる(Windows 10 1703以降)。
            // これを未処理のままにしておくとWindowsがDPIの比率で線形に伸縮させた値をそのまま
            // 適用するので、一般的なWindowsアプリと同じ挙動になる。
            //
            // ここで更新するのはUIの拡大率に使う値だけ。ウィンドウのドラッグ中はアプリが
            // 1フレームも描画しないため、この値が実際にUIへ反映されるのはマウスを離した後になる。
            // Windowsの拡大率(=ドラッグ中の引き伸ばしと同じ比)を使っているのはそのためで、
            // 違う比にすると離した瞬間に見た目が飛ぶ(GetDpiScaleのコメント参照)
            const UINT newDpi = HIWORD(wParam);
            if (newDpi == 0)
            {
                Logger::Warning("Window", "WM_DPICHANGEDのDPIが0だったため無視します");
                return 0;
            }

            m_DpiScale.store(static_cast<float>(newDpi) / 96.0f, std::memory_order_relaxed);
            Logger::Info("Window", "モニタのDPIが変わりました (DPI: " + std::to_string(newDpi) + ")");
            return 0;
        }

        case WM_MOUSEMOVE:
            m_MousePosition.x = GET_X_LPARAM(lParam);
            m_MousePosition.y = GET_Y_LPARAM(lParam);
            if (!m_TrackingMouseLeave)
            {
                // WM_MOUSELEAVEはデフォルトでは発生しないため、TrackMouseEventで明示的に要求する。
                // このフラグを立てておかないとWM_MOUSEMOVEのたびに再登録してしまう
                TRACKMOUSEEVENT trackEvent{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, m_Handle, 0 };
                TrackMouseEvent(&trackEvent);
                m_TrackingMouseLeave = true;
            }
            m_MouseInClient = true;
            m_LastMouseMoveTime = std::chrono::steady_clock::now();
            return 0;

        case WM_MOUSELEAVE:
            // m_MouseLeaveSuppressionEnabled(環境変数KURENAI_INPUT_AUTOMATION=1)が有効な場合のみ、
            // PostMessageによる自動操作(実カーソルはウィンドウ外にあることが多い)で注入した
            // WM_MOUSEMOVEの直後にTrackMouseEventが実カーソル位置に基づいてWM_MOUSELEAVEを生成し
            // m_MouseInClientが真になった直後に偽へ戻ることを繰り返してしまう問題を抑制する
            // (ForwardQueuedMessagesToImGuiのkMouseLeaveSuppressionと同種の問題)。
            // 直近kMouseLeaveSuppressionウィンドウ内にWM_MOUSEMOVEを処理していれば、このWM_MOUSELEAVEは
            // 実カーソル基準の古い判定によるノイズとみなしてm_MouseInClientを戻さない。
            // 通常起動時(環境変数未設定)はこのブロック自体を通らず、従来どおり即座にfalseへ戻す
            if (m_MouseLeaveSuppressionEnabled)
            {
                constexpr auto kMouseLeaveSuppression = std::chrono::milliseconds(100);
                if ((std::chrono::steady_clock::now() - m_LastMouseMoveTime) < kMouseLeaveSuppression)
                {
                    m_TrackingMouseLeave = false;
                    return 0;
                }
            }
            m_MouseInClient = false;
            m_TrackingMouseLeave = false;
            return 0;

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        {
            const size_t index = message == WM_LBUTTONDOWN ? 0 : message == WM_RBUTTONDOWN ? 1 : 2;
            if (!m_MouseButtonDown[index])
            {
                m_MouseButtonPressedEdge[index] = true;
            }
            m_MouseButtonDown[index] = true;
            return 0;
        }

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        {
            const size_t index = message == WM_LBUTTONUP ? 0 : message == WM_RBUTTONUP ? 1 : 2;
            m_MouseButtonDown[index] = false;
            return 0;
        }

        case WM_KEYDOWN:
        {
            const KeyCode key = static_cast<KeyCode>(wParam);
            if (key >= 0 && key < 256)
            {
                // bit30(前回のキー状態)が立っている場合はオートリピートによる再送のため、
                // エッジ検出(WasKeyPressed)には反映しない
                const bool isRepeat = (lParam & (1 << 30)) != 0;
                if (!isRepeat)
                {
                    m_KeyPressedEdge[key] = true;
                }
                m_KeyDown[key] = true;
            }
            return 0;
        }

        case WM_KEYUP:
        {
            const KeyCode key = static_cast<KeyCode>(wParam);
            if (key >= 0 && key < 256)
            {
                m_KeyDown[key] = false;
            }
            return 0;
        }

        case WM_KILLFOCUS:
            // フォーカスを失った時点のキー/ボタン押下状態を持ち越すと、フォーカスが戻った後も
            // 実際には離されているキーが押されたまま扱われてしまうため、ここで全てクリアする
            std::fill(std::begin(m_KeyDown), std::end(m_KeyDown), false);
            std::fill(std::begin(m_MouseButtonDown), std::end(m_MouseButtonDown), false);
            return DefWindowProcW(m_Handle, message, wParam, lParam);

        default:
            return DefWindowProcW(m_Handle, message, wParam, lParam);
        }
    }
}
