#include "Window.h"

#include <backends/imgui_impl_win32.h>

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

        default:
            return DefWindowProcW(m_Handle, message, wParam, lParam);
        }
    }
}
