#pragma once

#include "UI/IPanel.h"

namespace Kurenai
{
    class KurenaiEngine3D;
}

namespace Kurenai::UI
{
    // シーンの切り替え。一覧はKurenaiEngine3D::DiscoverScenesが構築したものをそのまま並べる
    class ScenePanel final : public IPanel
    {
    public:
        explicit ScenePanel(KurenaiEngine3D& engine) : m_Engine(engine) {}

        // ###以降がウィンドウIDになる。imgui.iniとドックレイアウトのキーになるため
        // ###以降は変更しないこと(表示名だけなら変更してよい)
        const char* GetWindowName() const override { return "シーン###Scenes"; }
        const char* GetMenuLabel() const override { return "シーン"; }
        void Draw(const PanelDrawContext& context) override;

    private:
        KurenaiEngine3D& m_Engine;
    };
}
