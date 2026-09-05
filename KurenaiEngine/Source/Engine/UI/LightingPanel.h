#pragma once

#include <array>

#include "UI/IPanel.h"

namespace Kurenai
{
    class KurenaiEngine3D;
}

namespace Kurenai::UI
{
    // 太陽(時刻・方位角・シーン全体の露出)と、ポイント/スポットライトの一覧・編集
    class LightingPanel final : public IPanel
    {
    public:
        explicit LightingPanel(KurenaiEngine3D& engine) : m_Engine(engine) {}

        // ###以降がウィンドウIDになる。imgui.iniとドックレイアウトのキーになるため
        // ###以降は変更しないこと(表示名だけなら変更してよい)
        const char* GetWindowName() const override { return "ライティング###Lighting"; }
        const char* GetMenuLabel() const override { return "ライティング"; }
        void Draw(const PanelDrawContext& context) override;

    private:
        void DrawSunSection();
        // エミッシブ光源(自発光メッシュから起こす光源プロキシ)のつまみ。
        // 「自発光の強度」スライダーの直下に置く
        void DrawEmissiveLightControls();
        void DrawLightsSection(const PanelDrawContext& context);
        void DrawSelectedLightEditor();

        KurenaiEngine3D& m_Engine;

        // ImGui::InputTextは複数フレームにまたがって自身のバッファを編集し、IMEの未確定文字列も
        // そこに入る。毎フレームlight.Nameから詰め直すと変換中の文字が消えてしまうため、
        // 選択中のライトが変わったときだけ同期する。-1は「未同期(次に詰め直す)」を表す番兵値
        int m_NameBufferLightIndex = -1;
        std::array<char, 128> m_NameBuffer{};
    };
}
