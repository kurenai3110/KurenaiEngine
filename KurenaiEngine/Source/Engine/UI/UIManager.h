#pragma once

#include <memory>
#include <vector>

#include "UI/IPanel.h"

namespace Kurenai
{
    class KurenaiEngine3D;
}

namespace Kurenai::UI
{
    // UIパネル群の所有者。メインメニューバーと画面全体を覆うドックスペースを出し、
    // 表示中のパネルを順に描く。ImGuiはシングルスレッド前提のため、この一式は
    // Renderスレッドからのみ呼ぶこと
    class UIManager
    {
    public:
        explicit UIManager(KurenaiEngine3D& engine);
        ~UIManager();

        UIManager(const UIManager&) = delete;
        UIManager& operator=(const UIManager&) = delete;

        // Renderスレッドから毎フレーム1回、ImGui::NewFrame()の後に呼ぶ。
        // ImGuiパネルが非表示(F1でトグル)のフレームでは呼ばない
        void Draw(const PanelDrawContext& context);

        // レイアウトを既定へ戻す要求。ドックの組み立ては各パネルのImGui::Beginより前で
        // なければ効かないため、ここではフラグを立てるだけにして次のフレームの先頭で消費する
        void RequestResetLayout() { m_ResetLayoutRequested = true; }

        // 各パネルの表示/非表示チェックボックスを並べる。メニューバーとSystemパネルの
        // 両方から同じものを出すためここに置く
        void DrawPanelVisibilityControls();

    private:
        void DrawMainMenuBar();
        // ドックスペースを張り、必要なら既定レイアウトを組む。各パネルのImGui::Beginより前に呼ぶこと
        void DrawDockSpaceAndLayout();

        KurenaiEngine3D& m_Engine;
        std::vector<std::unique_ptr<IPanel>> m_Panels;

        // ImGuiID(実体はunsigned int)。このヘッダをimgui.hに依存させないため素の型で持つ
        unsigned int m_DockSpaceId = 0;
        // imgui.iniにドック情報が無い初回起動かどうかの判定を済ませたか
        bool m_LayoutChecked = false;
        bool m_ResetLayoutRequested = false;
    };
}
