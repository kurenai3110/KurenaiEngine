#pragma once

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "KurenaiTypes.h"

// dllexportされたクラスが非export型(std::function<...>)をメンバに持つことによる
// C4251警告を抑制する。KurenaiEngine.dllと各サンプルは常に同一コンパイラ・同一ランタイム
// ライブラリ設定でビルドされるため、実務上は問題にならない
#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai::Core
{
    class KURENAI_LIB_API Window
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

        // WndProc(PumpMessages呼び出し元スレッド=Updateスレッド)で受け取ったメッセージのうち
        // ImGui向けにキューイングされた分を、呼び出し元スレッド上でImGui_ImplWin32_WndProcHandlerへ
        // 転送する。Dear ImGuiはシングルスレッド前提のライブラリで、ImGui::NewFrame()/描画処理を
        // 行うスレッド以外からImGuiの状態を触ってはならないため、Renderスレッドを持つ構成(KurenaiEngine3D)
        // では毎フレームNewFrame()の直前に「Renderスレッド自身から」この関数を呼び出すこと
        // (WndProc側で直接ImGui_ImplWin32_WndProcHandlerを呼ぶと、Renderスレッドと同時に
        // ImGuiの内部状態を書き換えてしまいデータ競合になる)
        void ForwardQueuedMessagesToImGui();

    private:
        struct PendingWndProcMessage
        {
            UINT Message;
            WPARAM WParam;
            LPARAM LParam;
        };

        static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

        HWND m_Handle = nullptr;
        // Render()を別スレッドで動かす場合、WM_SIZE(PumpMessages呼び出し元スレッド)による書き込みと
        // GetWidth/GetHeight(描画スレッドからの読み取り)が同時に発生し得るためatomicにしておく
        std::atomic<uint32_t> m_Width;
        std::atomic<uint32_t> m_Height;
        bool m_ShouldClose = false;
        ResizeCallback m_ResizeCallback;

        bool m_MouseInClient = false;
        bool m_TrackingMouseLeave = false;
        // 環境変数KURENAI_INPUT_AUTOMATION=1が設定されている場合のみ有効になる。
        // PostMessageによる自動操作時、TrackMouseEventが実カーソル位置基準で生成するWM_MOUSELEAVEが
        // m_MouseInClientを誤ってfalseに戻すのを抑制する(HandleMessage内のWM_MOUSELEAVEケース参照)。
        // 通常起動(環境変数未設定)では従来どおり即座にfalseへ戻すため、実操作には一切影響しない
        bool m_MouseLeaveSuppressionEnabled = false;
        // HandleMessage内で直近のWM_MOUSEMOVE処理時刻を記録する(HandleMessageはWndProcから
        // 直接呼ばれ単一スレッドでのみ読み書きするためlock不要)
        std::chrono::steady_clock::time_point m_LastMouseMoveTime{};
        POINT m_MousePosition{};
        bool m_MouseButtonDown[3]{};
        bool m_MouseButtonPressedEdge[3]{};
        bool m_KeyDown[256]{};
        bool m_KeyPressedEdge[256]{};

        // WndProc(Updateスレッド)からForwardQueuedMessagesToImGui呼び出し元(Renderスレッド)への
        // メッセージ受け渡し用。ImGui自体はこのキューにもWndProc処理にも関与しないため、
        // ここは単純なvector+mutexで足りる
        std::mutex m_PendingImGuiMessagesMutex;
        std::vector<PendingWndProcMessage> m_PendingImGuiMessages;
        // ForwardQueuedMessagesToImGui呼び出し元(Renderスレッド)のみが読み書きする
        std::chrono::steady_clock::time_point m_LastForwardedMouseMoveTime{};
    };
}

#pragma warning(pop)
