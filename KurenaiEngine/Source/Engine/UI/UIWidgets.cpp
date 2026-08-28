#include "UI/UIWidgets.h"

#include <string>

#include "Core/Logger.h"

namespace Kurenai::UI
{
    namespace
    {
        // 引数チェックの共通部分。UI側の実装ミスは黙って無視すると原因追跡が難しいためログに残す
        bool ValidatePointer(const void* value, const char* function, const char* label)
        {
            if (value != nullptr)
            {
                return true;
            }

            Core::Logger::Error(
                "UIWidgets",
                std::string(function) + " にvalue=nullptrが渡されました (label=" + (label != nullptr ? label : "(null)") + ")");
            return false;
        }

        // 直前のウィジェットへ右クリックメニューを付ける共通処理。
        // menuLabelの項目が選ばれたらtrueを返す。IDは直前のウィジェットのものが使われるため、
        // 同じラベルのウィジェットが複数あってもPushID側で区別されていれば衝突しない
        bool DrawResetContextMenu(const char* menuLabel)
        {
            bool selected = false;
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem(menuLabel))
                {
                    selected = true;
                }
                ImGui::EndPopup();
            }
            return selected;
        }

        constexpr const char* kResetMenuLabel = "既定値に戻す";
        constexpr const char* kRecalcMenuLabel = "シーンから再計算";
    }

    void ItemHelp(const char* description)
    {
        if (description == nullptr)
        {
            return;
        }
        ImGui::SetItemTooltip("%s", description);
    }

    void DrawUsageHint()
    {
        // パネルはドックの幅次第で狭くなるため、1行で書くと右端で切れる。折り返して出す
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("Ctrl+クリックで数値入力 / 右クリックで既定値に戻す");
        ImGui::PopStyleColor();
    }

    void BeginParamGroup()
    {
        // ラベル用に確保する幅。負値のPushItemWidthは「右から何px空けるか」の指定になる。
        // 日本語ラベルは英語より横に広いため10文字ぶん程度を見込む。
        // フォントサイズ基準にしているのはDPIスケールへ自動追従させるため
        ImGui::PushItemWidth(-ImGui::GetFontSize() * 10.0f);
    }

    void EndParamGroup()
    {
        ImGui::PopItemWidth();
    }

    bool SliderFloatEx(
        const char* label, float* value, float minValue, float maxValue, float defaultValue, const char* format,
        ImGuiSliderFlags flags, const char* help)
    {
        if (!ValidatePointer(value, "SliderFloatEx", label))
        {
            return false;
        }

        bool changed = ImGui::SliderFloat(label, value, minValue, maxValue, format, flags);
        ItemHelp(help);
        if (DrawResetContextMenu(kResetMenuLabel))
        {
            *value = defaultValue;
            changed = true;
        }
        return changed;
    }

    bool SliderIntEx(const char* label, int* value, int minValue, int maxValue, int defaultValue, const char* help)
    {
        if (!ValidatePointer(value, "SliderIntEx", label))
        {
            return false;
        }

        bool changed = ImGui::SliderInt(label, value, minValue, maxValue);
        ItemHelp(help);
        if (DrawResetContextMenu(kResetMenuLabel))
        {
            *value = defaultValue;
            changed = true;
        }
        return changed;
    }

    bool SliderUIntEx(
        const char* label, uint32_t* value, int minValue, int maxValue, uint32_t defaultValue, const char* help)
    {
        if (!ValidatePointer(value, "SliderUIntEx", label))
        {
            return false;
        }

        int temporary = static_cast<int>(*value);
        const bool changed = SliderIntEx(label, &temporary, minValue, maxValue, static_cast<int>(defaultValue), help);
        if (changed)
        {
            // スライダーの範囲は呼び出し側が0以上で渡す前提だが、負値が来ても
            // uint32_tへ巨大な値として化けないようここで丸める
            *value = static_cast<uint32_t>(temporary < 0 ? 0 : temporary);
        }
        return changed;
    }

    bool CheckboxEx(const char* label, bool* value, bool defaultValue, const char* help)
    {
        if (!ValidatePointer(value, "CheckboxEx", label))
        {
            return false;
        }

        bool changed = ImGui::Checkbox(label, value);
        ItemHelp(help);
        if (DrawResetContextMenu(kResetMenuLabel))
        {
            *value = defaultValue;
            changed = true;
        }
        return changed;
    }

    bool ComboEx(
        const char* label, int* currentIndex, const char* const items[], int itemCount, int defaultIndex,
        const char* help)
    {
        if (!ValidatePointer(currentIndex, "ComboEx", label))
        {
            return false;
        }
        if (items == nullptr || itemCount <= 0)
        {
            Core::Logger::Error(
                "UIWidgets",
                std::string("ComboExに空の選択肢が渡されました (label=") + (label != nullptr ? label : "(null)") + ")");
            return false;
        }

        bool changed = ImGui::Combo(label, currentIndex, items, itemCount);
        ItemHelp(help);
        if (DrawResetContextMenu(kResetMenuLabel))
        {
            *currentIndex = defaultIndex;
            changed = true;
        }
        return changed;
    }

    bool SliderFloatSceneDependent(
        const char* label, float* value, float minValue, float maxValue, bool& outRecalcRequested, const char* format,
        ImGuiSliderFlags flags, const char* help)
    {
        if (!ValidatePointer(value, "SliderFloatSceneDependent", label))
        {
            return false;
        }

        const bool changed = ImGui::SliderFloat(label, value, minValue, maxValue, format, flags);
        ItemHelp(help);
        if (DrawResetContextMenu(kRecalcMenuLabel))
        {
            outRecalcRequested = true;
        }
        return changed;
    }
}
