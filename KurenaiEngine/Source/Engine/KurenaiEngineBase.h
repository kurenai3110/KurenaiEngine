#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "KurenaiTypes.h"

#include "Core/Window.h"
#include "RHI/IRHIDevice.h"

// dllexportされたクラスが非export型(std::unique_ptr<RHI::IRHIDevice>など)をメンバに持つ
// ことによるC4251警告を抑制する。KurenaiEngine.dllと各サンプルは常に同一コンパイラ・
// 同一ランタイムライブラリ設定でビルドされるため、実務上は問題にならない
#pragma warning(push)
#pragma warning(disable: 4251)

namespace Kurenai
{
    // KurenaiEngine3D/KurenaiEngine2Dに共通する土台(ウィンドウ・デバイス・スワップチェーンの
    // 生成・管理)。サンプルプログラムがこのクラスを直接構築することは想定していない
    // (コンストラクタはprotected)
    class KURENAI_API KurenaiEngineBase
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

    protected:
        KurenaiEngineBase(const std::wstring& title, uint32_t width, uint32_t height, GraphicsAPI api);

        RHI::IRHICommandList* GetCommandList() const;

        std::unique_ptr<Core::Window> m_Window;
        std::unique_ptr<RHI::IRHIDevice> m_Device;
        std::unique_ptr<RHI::IRHISwapChain> m_SwapChain;
    };
}

#pragma warning(pop)
