#include "ImGuiDockLayout.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <string>

#include "Core/Logger.h"

namespace Kurenai::Core
{
    bool ImGuiDockLayout::HasNode(unsigned int dockSpaceId)
    {
        if (dockSpaceId == 0)
        {
            Logger::Error("ImGuiDockLayout", "HasNodeにdockSpaceId=0が渡されました");
            return false;
        }

        return ImGui::DockBuilderGetNode(static_cast<ImGuiID>(dockSpaceId)) != nullptr;
    }

    bool ImGuiDockLayout::BuildDefault(
        unsigned int dockSpaceId, float width, float height, const ImGuiDockSlotDesc* slots, std::size_t slotCount)
    {
        if (dockSpaceId == 0 || slots == nullptr || slotCount == 0 || width <= 0.0f || height <= 0.0f)
        {
            Logger::Error(
                "ImGuiDockLayout",
                "既定のドックレイアウト構築に失敗しました(引数が不正: dockSpaceId=" + std::to_string(dockSpaceId) +
                    ", width=" + std::to_string(width) + ", height=" + std::to_string(height) +
                    ", slotCount=" + std::to_string(slotCount) + ")");
            return false;
        }

        const ImGuiID rootId = static_cast<ImGuiID>(dockSpaceId);

        // 既存のノードを一度すべて消してから作り直す。DockBuilderSplitNodeは分割前に
        // ノードサイズが確定していないと分割比が信用できないため、AddNodeの直後に必ず
        // SetNodeSizeを呼ぶ(imgui_internal.hのDockBuilder関連コメント参照)
        ImGui::DockBuilderRemoveNode(rootId);
        ImGui::DockBuilderAddNode(rootId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(rootId, ImVec2(width, height));

        // 左22% / 右30% / 下28%を確保し、残りを中央(3D映像を透過表示する空ノード)にする
        ImGuiID centerId = rootId;
        ImGuiID leftId = ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Left, 0.22f, nullptr, &centerId);
        ImGuiID rightId = ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Right, 0.30f, nullptr, &centerId);
        const ImGuiID bottomId = ImGui::DockBuilderSplitNode(centerId, ImGuiDir_Down, 0.28f, nullptr, &centerId);

        ImGuiID leftBottomId = 0;
        leftId = ImGui::DockBuilderSplitNode(leftId, ImGuiDir_Up, 0.40f, nullptr, &leftBottomId);

        ImGuiID rightBottomId = 0;
        rightId = ImGui::DockBuilderSplitNode(rightId, ImGuiDir_Up, 0.55f, nullptr, &rightBottomId);

        for (std::size_t i = 0; i < slotCount; ++i)
        {
            if (slots[i].WindowName == nullptr)
            {
                Logger::Warning(
                    "ImGuiDockLayout", "WindowNameがnullptrのスロットをスキップしました (index=" + std::to_string(i) + ")");
                continue;
            }

            ImGuiID target = leftId;
            switch (slots[i].Slot)
            {
            case ImGuiDockSlot::Left:        target = leftId;        break;
            case ImGuiDockSlot::LeftBottom:  target = leftBottomId;  break;
            case ImGuiDockSlot::Right:       target = rightId;       break;
            case ImGuiDockSlot::RightBottom: target = rightBottomId; break;
            case ImGuiDockSlot::Bottom:      target = bottomId;      break;
            }
            ImGui::DockBuilderDockWindow(slots[i].WindowName, target);
        }

        ImGui::DockBuilderFinish(rootId);

        Logger::Info(
            "ImGuiDockLayout", "既定のドックレイアウトを構築しました (" + std::to_string(slotCount) + "パネル)");
        return true;
    }
}
