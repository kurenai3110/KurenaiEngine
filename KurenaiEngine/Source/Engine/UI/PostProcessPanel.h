#pragma once

#include "UI/IPanel.h"

namespace Kurenai
{
    class KurenaiEngine3D;
}

namespace Kurenai::UI
{
    // ライティング結果を最終画像へ写す段の設定(トーンマップ・ブルーム・自動露出)
    class PostProcessPanel final : public IPanel
    {
    public:
        explicit PostProcessPanel(KurenaiEngine3D& engine) : m_Engine(engine) {}

        // ###以降がウィンドウIDになる。imgui.iniとドックレイアウトのキーになるため
        // ###以降は変更しないこと(表示名だけなら変更してよい)
        const char* GetWindowName() const override { return "ポストプロセス###Post Processing"; }
        const char* GetMenuLabel() const override { return "ポストプロセス"; }
        void Draw(const PanelDrawContext& context) override;

    private:
        void DrawTonemapSection();
        void DrawTAASection();
        void DrawBloomSection();
        void DrawAutoExposureSection();

        KurenaiEngine3D& m_Engine;
    };
}
