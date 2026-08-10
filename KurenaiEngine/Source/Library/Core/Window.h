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

        // Windowsのディスプレイ設定で指定されている拡大率(96 DPIを1.0とする倍率)。
        // UIの拡大率はこの値に追従させる。
        //
        // モニタの物理サイズから求めた「実際のピクセル密度」に合わせた方が、モニタ間で文字の
        // 実寸はきれいに揃う(実測: 27インチ4Kの実DPIは163だがWindowsの割り当ては150%=144で、
        // 24インチFHD(実DPI 92, 100%)との間で実寸が15%ずれる)。それでもWindowsの拡大率を
        // 使っているのは、**ウィンドウのドラッグ中はアプリが1フレームも描画しない**ため。
        // その間ウィンドウはWindowsの拡大率の比でリサイズされ、画面には直前のフレームが
        // 同じ比で引き伸ばされて表示される。UIの拡大率をそれと違う比にすると、マウスを離して
        // 描画が再開した瞬間に見た目が飛ぶ(実測で18%のジャンプ)。
        // 引き伸ばしと同じ比にしておけば、離してもUIの大きさが変わらない。
        //
        // WM_DPICHANGED(Updateスレッド)が書き、UIのスタイル再適用(Renderスレッド)が読むためatomic。
        // プロセスがDPI非対応の場合はGetDpiForWindowが常に96を返す仕様のため1.0になる
        float GetDpiScale() const { return m_DpiScale.load(std::memory_order_relaxed); }

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
        // 直前のPumpMessages()呼び出し中にボタンが離された(押された状態から離れた状態になった)か。
        // 一般的なUIのボタンは「押下」ではなく「押下→同じボタン上での解放」でクリックを確定する
        // (押したまま領域外へ出せばキャンセルできる)ため、その判定に使う。
        // IsMouseButtonDown()の前フレーム値を呼び出し側で保持する方法と違い、
        // 1回のPumpMessages()の中で押して離した場合も取りこぼさない
        bool WasMouseButtonReleased(MouseButton button) const;
        // 現在キーが押された状態かどうか(WASD移動のような「押している間」継続する操作に使う)
        bool IsKeyDown(KeyCode key) const;
        // 直前のPumpMessages()呼び出し中にキーが押された(離れた状態から押された状態になった)か。
        // オートリピートによる連続したWM_KEYDOWNは無視する
        bool WasKeyPressed(KeyCode key) const;
        // 直前のPumpMessages()呼び出し中にキーが離された(押された状態から離れた状態になった)か
        bool WasKeyReleased(KeyCode key) const;
        // クライアント座標(原点は左上、Y-down。Win32の標準的な座標系)
        POINT GetClientMousePosition() const { return m_MousePosition; }
        // 直前のPumpMessages()呼び出し中に回転したホイールのノッチ数(WHEEL_DELTA単位)。
        // 奥へ回すと正。回転していなければ0。1回のPumpMessages()の中で複数の
        // WM_MOUSEWHEELが届いた場合は合算される(押下エッジと同じ「1フレームぶん」の寿命)。
        // 高分解能ホイールは1ノッチ未満の値を刻んで送ってくるため、整数ではなくfloatで返す
        float GetMouseWheelDelta() const { return m_MouseWheelDelta; }

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

        // 前回終了時のウィンドウ位置・サイズ・最大化状態(KurenaiEngine.dllと同じフォルダの
        // window.ini)を復元する。読み込めなかった場合や保存された位置が現在のモニタ構成では
        // 画面外になる場合は、CreateWindowExW直後の位置・サイズのままにする。
        // 戻り値は最初の表示に使うShowWindowのコマンド(最大化を保存していればSW_SHOWMAXIMIZED)
        int ApplySavedPlacement();
        // 現在のウィンドウ配置をwindow.iniへ書き出す。デストラクタでDestroyWindowする前に呼ぶ
        void SaveCurrentPlacement() const;

        HWND m_Handle = nullptr;
        // Render()を別スレッドで動かす場合、WM_SIZE(PumpMessages呼び出し元スレッド)による書き込みと
        // GetWidth/GetHeight(描画スレッドからの読み取り)が同時に発生し得るためatomicにしておく
        std::atomic<uint32_t> m_Width;
        std::atomic<uint32_t> m_Height;
        // GetDpiScale()参照。WM_DPICHANGEDを処理するUpdateスレッドが書き、Renderスレッドが読む
        std::atomic<float> m_DpiScale{ 1.0f };
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
        bool m_MouseButtonReleasedEdge[3]{};
        bool m_KeyDown[256]{};
        bool m_KeyPressedEdge[256]{};
        bool m_KeyReleasedEdge[256]{};
        // WM_MOUSEWHEELの回転量をWHEEL_DELTA単位で累積する。エッジフラグと同じく
        // PumpMessagesの先頭でリセットするため、寿命は「1回のPumpMessages呼び出し」ぶん
        float m_MouseWheelDelta = 0.0f;

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
