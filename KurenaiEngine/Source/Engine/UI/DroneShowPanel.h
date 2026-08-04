#pragma once

#include "UI/IPanel.h"

namespace Kurenai
{
    class KurenaiEngine3D;
}

namespace Kurenai::UI
{
    // ドローンショー(発光ドローンの編隊)と星空のパラメータを編集するパネル。
    //
    // 星空をここへ同居させているのは、どちらも「夜空の見え方」を決める設定で、
    // 実際に振るときは必ず一緒に見比べることになるため(レンダリングパネルの雲と同じ理由で
    // 分けてもよかったが、雲と違い星はドローンショーの背景としてしか使い所が無い)
    class DroneShowPanel final : public IPanel
    {
    public:
        explicit DroneShowPanel(KurenaiEngine3D& engine) : m_Engine(engine) {}

        // ###以降がウィンドウIDになる。imgui.iniとドックレイアウトのキーになるため
        // ###以降は変更しないこと(表示名だけなら変更してよい)
        const char* GetWindowName() const override { return "ドローンショー###Drone Show"; }
        const char* GetMenuLabel() const override { return "ドローンショー"; }
        void Draw(const PanelDrawContext& context) override;

    private:
        void DrawShowSection();
        void DrawFormationSection();
        void DrawStarsSection();

        KurenaiEngine3D& m_Engine;
    };
}
