#pragma once

#include "UI/IPanel.h"

namespace Kurenai
{
    class KurenaiEngine3D;
}

namespace Kurenai::UI
{
    class UIManager;

    // 画質ではなく動作・表示に関わる設定(垂直同期・フレームレート制限・内部レンダー解像度・
    // グラフィックスAPI)と、UI自体の操作
    class SystemPanel final : public IPanel
    {
    public:
        SystemPanel(KurenaiEngine3D& engine, UIManager& uiManager) : m_Engine(engine), m_UIManager(uiManager) {}

        // ###以降がウィンドウIDになる。imgui.iniとドックレイアウトのキーになるため
        // ###以降は変更しないこと(表示名だけなら変更してよい)
        const char* GetWindowName() const override { return "システム###System"; }
        const char* GetMenuLabel() const override { return "システム"; }
        void Draw(const PanelDrawContext& context) override;

    private:
        void DrawDisplaySection();
        void DrawResolutionSection();
        void DrawGraphicsAPISection();
        void DrawUISection();

        KurenaiEngine3D& m_Engine;
        UIManager& m_UIManager;
    };
}
