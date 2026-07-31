#pragma once

#include <array>

#include "UI/IPanel.h"

namespace Kurenai
{
    class KurenaiEngine3D;
}

namespace Kurenai::UI
{
    // 反射プローブの一覧・追加/削除・プロパティ編集・再ベイクを行うパネル。
    // ライティングパネルと同じ「一覧 + 選択したものの編集」の構成にしてある
    class ReflectionProbePanel final : public IPanel
    {
    public:
        explicit ReflectionProbePanel(KurenaiEngine3D& engine) : m_Engine(engine) {}

        // ###以降がウィンドウIDになる。imgui.iniとドックレイアウトのキーになるため
        // ###以降は変更しないこと(表示名だけなら変更してよい)
        const char* GetWindowName() const override { return "反射プローブ###Reflection Probes"; }
        const char* GetMenuLabel() const override { return "反射プローブ"; }
        void Draw(const PanelDrawContext& context) override;

    private:
        void DrawGlobalSettings();
        void DrawProbeList();
        void DrawSelectedProbeEditor();

        KurenaiEngine3D& m_Engine;

        // ImGui::InputTextは複数フレームにまたがって自身のバッファを編集し、IMEの未確定文字列も
        // そこに入る。毎フレームprobe.Nameから詰め直すと変換中の文字が消えてしまうため、
        // 選択中のプローブが変わったときだけ同期する。-1は「未同期(次に詰め直す)」を表す番兵値
        // (LightingPanelのm_NameBufferLightIndexと同じ作法)
        int m_NameBufferProbeIndex = -1;
        std::array<char, 128> m_NameBuffer{};
    };
}
