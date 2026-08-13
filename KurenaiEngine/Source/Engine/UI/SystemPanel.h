#pragma once

#include "UI/IPanel.h"

namespace Kurenai
{
    class KurenaiEngine3D;
}

namespace Kurenai::UI
{
    class UIManager;

    // 動作・表示に関わる設定(垂直同期・フレームレート制限・内部レンダー解像度・
    // グラフィックスAPI)と、UI自体の操作。
    // 加えて品質プリセットもここに置く。個々の画質設定は「レンダリング」「ポストプロセス」
    // パネルにあるが、プリセットはその両方にまたがる横断的なつまみで、
    // フレーム時間を一括で振るという役目が内部レンダー解像度と同じ性質のため
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
        void DrawQualityPresetSection();
        void DrawResolutionSection();
        void DrawGraphicsAPISection();
        void DrawUISection();

        KurenaiEngine3D& m_Engine;
        UIManager& m_UIManager;
    };
}
