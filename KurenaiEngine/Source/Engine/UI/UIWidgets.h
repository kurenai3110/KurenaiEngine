#pragma once

#include <imgui.h>

#include <cstdint>

namespace Kurenai::UI
{
    // パネル共通のウィジェットヘルパ。
    //
    // 素のImGui::SliderFloat等をそのまま並べると、
    //  ・ラベルが右端で切れる(ウィジェット幅が既定のままで、右のラベルに幅が残らない)
    //  ・値を既定へ戻す手段が無い
    //  ・略語ラベル(SSIL Slices等)が何を指すのか分からない
    // という問題が出るため、ここで幅の統一・リセット・説明を一括で面倒を見る。
    //
    // 命名規約: labelは "日本語表示###ASCII_ID" 形式で渡す。ImGuiは###以降をIDに使うため、
    // 表示名を変えてもウィジェットのID(=ini上のキーやポップアップの同一性)が変わらない

    // 直前に置いたウィジェットへ説明ツールチップを付ける。
    // ImGui::SetItemTooltipは遅延表示を内蔵しているため、マウスが通り過ぎただけでは出ない
    void ItemHelp(const char* description);

    // 「Ctrl+クリックで数値入力 / 右クリックで既定値に戻す」の案内を薄いグレーで1行出す。
    // 各パネルの先頭で1回呼ぶ
    void DrawUsageHint();

    // ウィジェットの幅を「右端からラベル用の一定幅だけ空けた位置まで」に固定する。
    // 負値のPushItemWidthは「右から何px空けるか」の指定になるため、これで全ウィジェットの
    // 左端とラベル開始位置が縦に揃う。幅をフォントサイズ基準にしているのはDPIへ自動追従させるため。
    // 対でEndParamGroupを呼ぶこと
    void BeginParamGroup();
    void EndParamGroup();

    // ---- 既定値へのリセットを内蔵したウィジェット群 ----
    // いずれも右クリックで「既定値に戻す」ポップアップを出す。
    // helpにnullptr以外を渡すと説明ツールチップも付く

    bool SliderFloatEx(
        const char* label, float* value, float minValue, float maxValue, float defaultValue,
        const char* format = "%.3f", ImGuiSliderFlags flags = 0, const char* help = nullptr);

    bool SliderIntEx(
        const char* label, int* value, int minValue, int maxValue, int defaultValue, const char* help = nullptr);

    // uint32_t版。ImGuiにint版しか無いため呼び出し側の変換を1箇所にまとめる
    bool SliderUIntEx(
        const char* label, uint32_t* value, int minValue, int maxValue, uint32_t defaultValue,
        const char* help = nullptr);

    bool CheckboxEx(const char* label, bool* value, bool defaultValue, const char* help = nullptr);

    bool ComboEx(
        const char* label, int* currentIndex, const char* const items[], int itemCount, int defaultIndex,
        const char* help = nullptr);

    // 既定値を持たない(シーンから再計算する)スライダー。
    // 右クリックメニューの文言が「既定値に戻す」ではなく「シーンから再計算」になる。
    // 再計算が要求されたらtrueを返すので、呼び出し側でResetSceneDependentParams()を呼ぶこと
    bool SliderFloatSceneDependent(
        const char* label, float* value, float minValue, float maxValue, bool& outRecalcRequested,
        const char* format = "%.3f", const char* help = nullptr);
}
