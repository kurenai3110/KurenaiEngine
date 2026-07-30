#include "ImGuiDockLayout.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <string>

#include "Core/Logger.h"

namespace Kurenai::Core
{
    namespace
    {
        // ノードツリーを辿って寸法をratio倍する。ImGuiDockNodeはimgui_internal.hの型なので
        // この翻訳単位(Library側)でしか触れない
        void ScaleNodeSizesRecursive(ImGuiDockNode* node, float ratio)
        {
            if (node == nullptr)
            {
                return;
            }

            node->Size.x *= ratio;
            node->Size.y *= ratio;
            node->SizeRef.x *= ratio;
            node->SizeRef.y *= ratio;

            ScaleNodeSizesRecursive(node->ChildNodes[0], ratio);
            ScaleNodeSizesRecursive(node->ChildNodes[1], ratio);
        }
    }

    bool ImGuiDockLayout::ScaleForUIScaleChange(unsigned int dockSpaceId, float ratio)
    {
        if (dockSpaceId == 0 || ratio <= 0.0f)
        {
            Logger::Error(
                "ImGuiDockLayout",
                "レイアウトの拡縮に失敗しました(引数が不正: dockSpaceId=" + std::to_string(dockSpaceId) +
                    ", ratio=" + std::to_string(ratio) + ")");
            return false;
        }

        ImGuiContext* context = ImGui::GetCurrentContext();
        auto* viewport = static_cast<ImGuiViewportP*>(ImGui::GetMainViewport());
        if (context == nullptr || viewport == nullptr)
        {
            Logger::Error("ImGuiDockLayout", "ImGuiのコンテキストまたはメインビューポートを取得できません");
            return false;
        }

        // (2) 各ウィンドウ側。
        //
        // ImGuiにはDPI変更用のScaleWindowsInViewportがあるが、ここでは使わない。
        // あちらはPos / Size / SizeFullを無条件に拡縮するため、ドッキング中のウィンドウでは
        // ドックノードが決めるサイズと一時的に食い違う(実測: 拡縮直後にSize=279x426、
        // Begin後にノードが決めた280x278へ上書き)。浮いているウィンドウだけを対象にする。
        for (ImGuiWindow* window : context->Windows)
        {
            if (window == nullptr || window->Viewport != viewport)
            {
                continue;
            }

            // ドッキングしていないウィンドウは自分でサイズを持つので拡縮する。
            // ドッキング中はドックノードが毎フレーム決めるため触らない
            if (window->DockNode == nullptr)
            {
                const ImVec2 origin = viewport->Pos;
                window->Pos.x = origin.x + (window->Pos.x - origin.x) * ratio;
                window->Pos.y = origin.y + (window->Pos.y - origin.y) * ratio;
                window->Size.x *= ratio;
                window->Size.y *= ratio;
                window->SizeFull.x *= ratio;
                window->SizeFull.y *= ratio;
            }

            // スクロール位置。これをしないとスクロール済みのパネルで表示位置がずれる
            window->Scroll.x *= ratio;
            window->Scroll.y *= ratio;

            // スクロールバーの有無と長さは、Begin()内で「前フレームの利用可能サイズ」
            //   avail_size_from_last_frame = window->InnerRect.GetSize() + scrollbar_sizes_from_last_frame
            // を使って決まる(imgui.cpp)。この2つを拡縮しないと、拡大率が変わったフレームだけ
            // 古いサイズで判定され、スクロールバーの位置・長さが1フレーム乱れる。
            // InnerRectは絶対座標だがここで効くのは大きさだけなので、原点を保って幅・高さに掛ける
            const ImVec2 innerSize = window->InnerRect.GetSize();
            window->InnerRect.Max.x = window->InnerRect.Min.x + innerSize.x * ratio;
            window->InnerRect.Max.y = window->InnerRect.Min.y + innerSize.y * ratio;
            window->ScrollbarSizes.x *= ratio;
            window->ScrollbarSizes.y *= ratio;

            // 内容サイズ。ContentSizeはBegin()の冒頭でCalcWindowContentSizesにより
            //   ContentSize      = CursorMaxPos - CursorStartPos
            //   ContentSizeIdeal = max(CursorMaxPos, IdealMaxPos) - CursorStartPos
            // と再計算されて上書きされるため、ContentSizeを直しても効かない。
            // 元になるDC側を拡縮する。CursorStartPosは内容の起点(絶対座標)で、
            // 効くのはそこからの差分だけなので、起点を保ったまま差分に掛ける
            const ImVec2 start = window->DC.CursorStartPos;
            window->DC.CursorMaxPos.x = start.x + (window->DC.CursorMaxPos.x - start.x) * ratio;
            window->DC.CursorMaxPos.y = start.y + (window->DC.CursorMaxPos.y - start.y) * ratio;
            window->DC.IdealMaxPos.x = start.x + (window->DC.IdealMaxPos.x - start.x) * ratio;
            window->DC.IdealMaxPos.y = start.y + (window->DC.IdealMaxPos.y - start.y) * ratio;
        }

        // (1) ドックノードの寸法
        ImGuiDockNode* root = ImGui::DockBuilderGetNode(static_cast<ImGuiID>(dockSpaceId));
        if (root == nullptr)
        {
            // 初回起動でまだレイアウトが組まれていない。BuildDefaultが現在の拡大率で組むので
            // ここで何もしなくてよい
            return false;
        }

        ScaleNodeSizesRecursive(root, ratio);
        return true;
    }

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
