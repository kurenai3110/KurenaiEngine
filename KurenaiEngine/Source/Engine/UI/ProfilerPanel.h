#pragma once

#include "UI/IPanel.h"

namespace Kurenai
{
    class KurenaiEngine3D;
}

namespace Kurenai::UI
{
    // FPS・CPU/GPUフレーム時間とパスごとの内訳(表示のみ、操作系ウィジェットは持たない)
    class ProfilerPanel final : public IPanel
    {
    public:
        explicit ProfilerPanel(KurenaiEngine3D& engine) : m_Engine(engine) {}

        // ###以降がウィンドウIDになる。imgui.iniとドックレイアウトのキーになるため
        // ###以降は変更しないこと(表示名だけなら変更してよい)
        const char* GetWindowName() const override { return "プロファイラ###Profiler"; }
        const char* GetMenuLabel() const override { return "プロファイラ"; }
        void Draw(const PanelDrawContext& context) override;

    private:
        KurenaiEngine3D& m_Engine;
    };
}
