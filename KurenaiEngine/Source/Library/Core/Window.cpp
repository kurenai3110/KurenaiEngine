#include "Window.h"

#include <backends/imgui_impl_win32.h>
#include <windowsx.h>

#include <algorithm>
#include <stdexcept>

// imgui_impl_win32.hは<windows.h>への依存を避けるためこの宣言を#if 0でコメントアウトしており、
// 呼び出し側でこの1行をコピーして前方宣言することが公式に案内されている
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Kurenai::Core
{
    namespace
    {
        const wchar_t* kWindowClassName = L"KurenaiEngineWindowClass";
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

        ShowWindow(m_Handle, SW_SHOW);
    }

    Window::~Window()
    {
        if (m_Handle)
        {
            DestroyWindow(m_Handle);
        }
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

    bool Window::WasMouseButtonPressed(MouseButton button) const
    {
        return m_MouseButtonPressedEdge[static_cast<size_t>(button)];
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
        // このエンジン側で引き続き処理する必要があるため、早期returnはしない
        ImGui_ImplWin32_WndProcHandler(m_Handle, message, wParam, lParam);

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
            return 0;

        case WM_MOUSELEAVE:
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
