#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "KurenaiTypes.h"

#include "Core/AudioEngine.h"
#include "Core/Window.h"
#include "RHI/IRHIDevice.h"

// dllexportされたクラスが非export型(std::unique_ptr<RHI::IRHIDevice>など)をメンバに持つ
// ことによるC4251警告を抑制する。KurenaiEngine.dllと各サンプルは常に同一コンパイラ・
// 同一ランタイムライブラリ設定でビルドされるため、実務上は問題にならない
#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai
{
    // KurenaiEngineBase::LoadSoundが返す不透明なサウンドハンドル。内部ではCore::AudioEngine内の
    // インデックスを保持する
    class SoundHandle
    {
    public:
        SoundHandle() = default;
        bool IsValid() const { return m_Valid; }

    private:
        explicit SoundHandle(uint32_t index) : m_Index(index), m_Valid(true) {}
        uint32_t m_Index = 0;
        bool m_Valid = false;
        friend class KurenaiEngineBase;
    };

    // PlaySoundが返す再生中ボイスのハンドル。loop=trueで再生したボイスをStopSoundで止める際に使う。
    // loop=falseの場合も返るが、単発再生は自動的に終了するため通常は使わなくてよい
    class VoiceHandle
    {
    public:
        VoiceHandle() = default;
        bool IsValid() const { return m_Id != 0; }

    private:
        explicit VoiceHandle(uint64_t id) : m_Id(id) {}
        uint64_t m_Id = 0;
        friend class KurenaiEngineBase;
    };

    // KurenaiEngine3D/KurenaiEngine2Dに共通する土台(ウィンドウ・デバイス・スワップチェーンの
    // 生成・管理)。サンプルプログラムがこのクラスを直接構築することは想定していない
    // (コンストラクタはprotected)
    class KURENAI_LIB_API KurenaiEngineBase
    {
    public:
        virtual ~KurenaiEngineBase();

        KurenaiEngineBase(const KurenaiEngineBase&) = delete;
        KurenaiEngineBase& operator=(const KurenaiEngineBase&) = delete;

        bool ShouldClose() const;
        void PumpEvents();
        // ウィンドウへWM_CLOSEを送り、次のPumpEvents()以降ShouldClose()がtrueを返すようにする
        void Close();

        uint32_t GetWidth() const;
        uint32_t GetHeight() const;

        // 生成したウィンドウのHWND。GetCursorPos+ScreenToClient等、アプリ側で一般的な
        // Win32入力処理を行う際に必要となるため公開する
        HWND GetWindowHandle() const;

        // ウィンドウスコープの入力状態(WM_MOUSEMOVE/WM_LBUTTONDOWN等のメッセージから更新される)。
        // GetAsyncKeyState/GetCursorPosと異なりフォーカスを失った状態では反応せず、PumpEvents()の
        // 呼び出し中に処理されたメッセージのみを反映する
        bool IsMouseOverWindow() const;
        // 現在押された状態かどうか(WASD移動のような「押している間」継続する操作に使う)
        bool IsMouseButtonDown(MouseButton button) const;
        bool IsKeyDown(KeyCode key) const;
        // 直前のPumpEvents()呼び出し中に押された(エッジ検出)か
        bool WasMouseButtonPressed(MouseButton button) const;
        bool WasKeyPressed(KeyCode key) const;
        POINT GetClientMousePosition() const;

        // WAV(PCM)ファイルを読み込み、再生用に登録する
        SoundHandle LoadSound(const std::wstring& filePath);
        // volumeは0.0〜1.0。戻り値はStopSoundへ渡すVoiceHandle
        // (loop=trueのボイスを止める場合に使う。loop=falseは自動的に終了するので通常は使わなくてよい)
        VoiceHandle PlaySound(SoundHandle sound, float volume = 1.0f, bool loop = false);
        // PlaySoundが返したVoiceHandleを指定して再生を即座に停止する。単発再生には通常不要
        void StopSound(VoiceHandle voice);

    protected:
        KurenaiEngineBase(const std::wstring& title, uint32_t width, uint32_t height, GraphicsAPI api);

        RHI::IRHICommandList* GetCommandList() const;

        // WM_SIZEで記録しておいたリサイズ要求があれば、スワップチェーンへ反映する。
        //
        // 【呼び出し規約】描画を所有するスレッドが、フレームの先頭(そのフレームのGPUコマンドを
        // まだ1つも積んでいない時点)で呼ぶこと。KurenaiEngine3DはRenderスレッドのRender()冒頭から、
        // レンダースレッドを持たないKurenaiEngine2Dはメインループから呼ぶ。
        //
        // 【なぜコールバックで直接リサイズしないのか】WM_SIZEはPumpEvents()を呼んだスレッド
        // (KurenaiEngine3DではUpdateスレッド)で同期的に発生するため、そこでResize()を呼ぶと
        // Renderスレッドが描画に使っている最中のスワップチェーンをRHI経由で作り替えることになる。
        // 以前はミューテックスで排他していたが、「RHIの呼び出しは描画を所有するスレッドに閉じる」
        // という方針に合わせ、要求だけ記録して所有スレッドが適用する方式にした
        // (詳細はdocs/Architecture.html 23章)。
        //
        // 副作用として反映が最大1フレーム遅れるが、Presentパスはアスペクト比を保つ
        // レターボックス処理を持つため見た目には出ない
        void ApplyPendingResize();

        std::unique_ptr<Core::Window> m_Window;
        std::unique_ptr<RHI::IRHIDevice> m_Device;
        std::unique_ptr<RHI::IRHISwapChain> m_SwapChain;
        std::unique_ptr<Core::AudioEngine> m_AudioEngine;

    private:
        // WM_SIZEのコールバック(PumpEvents呼び出し元スレッド)が書き込み、
        // ApplyPendingResize(描画を所有するスレッド)が読み取って消費する。
        // 保持するのは最新の1件だけでよい(途中のサイズへ合わせる必要はないため)
        std::mutex m_PendingResizeMutex;
        uint32_t m_PendingResizeWidth = 0;
        uint32_t m_PendingResizeHeight = 0;
        bool m_HasPendingResize = false;
    };
}

#pragma warning(pop)
