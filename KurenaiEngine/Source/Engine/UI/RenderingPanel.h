#pragma once

#include "UI/IPanel.h"

namespace Kurenai
{
    class KurenaiEngine3D;
}

namespace Kurenai::UI
{
    // 「シーンをどう照らすか」に属する品質設定。
    // AO/間接光・シャドウ・IBL/環境光・SSRをCollapsingHeaderで節に分けて扱う
    class RenderingPanel final : public IPanel
    {
    public:
        explicit RenderingPanel(KurenaiEngine3D& engine) : m_Engine(engine) {}

        // ###以降がウィンドウIDになる。imgui.iniとドックレイアウトのキーになるため
        // ###以降は変更しないこと(表示名だけなら変更してよい)
        const char* GetWindowName() const override { return "レンダリング###Rendering"; }
        const char* GetMenuLabel() const override { return "レンダリング"; }
        void Draw(const PanelDrawContext& context) override;

    private:
        void DrawAOSection();
        void DrawShadowSection();
        void DrawIBLSection();
        void DrawSSRSection();

        KurenaiEngine3D& m_Engine;
    };
}
