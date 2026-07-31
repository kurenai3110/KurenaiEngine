#pragma once

#include "UI/IPanel.h"

namespace Kurenai
{
    class KurenaiEngine3D;
}

namespace Kurenai::UI
{
    // Presentパスで表示する内容(各パス中間結果)の切り替えと、その表示補助設定
    class DebugViewPanel final : public IPanel
    {
    public:
        explicit DebugViewPanel(KurenaiEngine3D& engine) : m_Engine(engine) {}

        // ###以降がウィンドウIDになる。imgui.iniとドックレイアウトのキーになるため
        // ###以降は変更しないこと(表示名だけなら変更してよい)
        const char* GetWindowName() const override { return "デバッグ表示###Render Targets"; }
        const char* GetMenuLabel() const override { return "デバッグ表示"; }
        void Draw(const PanelDrawContext& context) override;

    private:
        void DrawBufferPrecisionSection();

        KurenaiEngine3D& m_Engine;
    };
}
