#pragma once

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <string>

#include "KurenaiTypes.h"

// dllexportされたクラスが非export型(std::function<...>)をメンバに持つことによる
// C4251警告を抑制する。KurenaiEngine.dllと各サンプルは常に同一コンパイラ・同一ランタイム
// ライブラリ設定でビルドされるため、実務上は問題にならない
#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai::Core
{
    class KURENAI_API Window
    {
    public:
        using ResizeCallback = std::function<void(uint32_t, uint32_t)>;

        Window(const std::wstring& title, uint32_t width, uint32_t height);
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        void PumpMessages();
        bool ShouldClose() const { return m_ShouldClose; }

        HWND GetHandle() const { return m_Handle; }
        uint32_t GetWidth() const { return m_Width; }
        uint32_t GetHeight() const { return m_Height; }

        void SetResizeCallback(ResizeCallback callback) { m_ResizeCallback = std::move(callback); }
        void SetTitle(const std::wstring& title);

        // 以下はWM_MOUSEMOVE/WM_LBUTTONDOWN等のウィンドウメッセージから状態を更新する
        // (GetAsyncKeyState/GetCursorPosと違いウィンドウスコープなので、フォーカスを失っていれば
        // 反応しない。PostMessageによるテスト自動化とも整合する)

        // カーソルがクライアント領域内にあるか
        bool IsMouseOverWindow() const { return m_MouseInClient; }
        // 現在ボタンが押された状態かどうか(WASD移動のような「押している間」継続する操作に使う)
        bool IsMouseButtonDown(MouseButton button) const;
        // 直前のPumpMessages()呼び出し中にボタンが押された(離れた状態から押された状態になった)か
        bool WasMouseButtonPressed(MouseButton button) const;
        // 現在キーが押された状態かどうか(WASD移動のような「押している間」継続する操作に使う)
        bool IsKeyDown(KeyCode key) const;
        // 直前のPumpMessages()呼び出し中にキーが押された(離れた状態から押された状態になった)か。
        // オートリピートによる連続したWM_KEYDOWNは無視する
        bool WasKeyPressed(KeyCode key) const;
        // クライアント座標(原点は左上、Y-down。Win32の標準的な座標系)
        POINT GetClientMousePosition() const { return m_MousePosition; }

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

        HWND m_Handle = nullptr;
        uint32_t m_Width;
        uint32_t m_Height;
        bool m_ShouldClose = false;
        ResizeCallback m_ResizeCallback;

        bool m_MouseInClient = false;
        bool m_TrackingMouseLeave = false;
        POINT m_MousePosition{};
        bool m_MouseButtonDown[3]{};
        bool m_MouseButtonPressedEdge[3]{};
        bool m_KeyDown[256]{};
        bool m_KeyPressedEdge[256]{};
    };
}

#pragma warning(pop)
