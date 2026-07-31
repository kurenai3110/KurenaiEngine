#pragma once

#include <cstddef>

#include "KurenaiTypes.h"

namespace Kurenai::Core
{
    // 既定レイアウトで各パネルをどの位置へ配置するか。同じスロットへ複数のパネルを
    // 割り当てると、それらは1つのドックノードのタブとしてまとまる
    enum class ImGuiDockSlot
    {
        Left,         // 左カラム上段
        LeftBottom,   // 左カラム下段
        Right,        // 右カラム上段
        RightBottom,  // 右カラム下段
        Bottom,       // 下段(横いっぱい)
    };

    struct ImGuiDockSlotDesc
    {
        // ImGui::Beginへ渡したウィンドウ名と完全一致させること(一致しないと無視される)
        const char* WindowName = nullptr;
        ImGuiDockSlot Slot = ImGuiDockSlot::Left;
    };

    // ImGuiのドックレイアウトを組むための、Library側に置いた代理API。
    //
    // ImGui::DockBuilderSplitNode / DockBuilderDockWindow はimgui_internal.hにあり、
    // imgui_internal.hにはdllexport指定された構造体が19個含まれる。KurenaiEngine3D.dllは
    // IMGUI_API=__declspec(dllimport)でビルドされるため、あちらからimgui_internal.hを
    // includeするとそれらの型がすべてdllimportクラスになり、「imgui_internal.hはLibraryしか
    // includeしない」という設計上の前提(docs/Architecture.html 12.2節)が崩れる。
    // そこでimgui_internal.hを安全にincludeできるLibrary側にこのクラスを置き、
    // KurenaiEngine3DからはこのAPIだけを呼ぶ構成にした。
    //
    // ImGui::DockSpaceOverViewport自体は公開APIなのでKurenaiEngine3D側から直接呼べる。
    // ここへ集約しているのはDockBuilder系だけであることに注意
    class KURENAI_LIB_API ImGuiDockLayout
    {
    public:
        // dockSpaceIdのノードが既に存在するか(=imgui.iniからレイアウトが復元されたか)。
        // 引数のunsigned intはImGuiID(実体はunsigned int)。このヘッダをimgui.hに
        // 依存させないため素の型で受ける
        static bool HasNode(unsigned int dockSpaceId);

        // dockSpaceId直下を左/右/下に分割し、slotsの各ウィンドウを割り当てる。
        // 既存のレイアウトは破棄して作り直す(=「レイアウトを初期化」の実体)。
        // ImGui::NewFrame()の後、対象ウィンドウのImGui::Beginより前に呼ぶこと。
        // 成功時true。引数が不正な場合は何もせずfalseを返し、理由をLogger::Errorへ出力する
        static bool BuildDefault(
            unsigned int dockSpaceId,
            float width,
            float height,
            const ImGuiDockSlotDesc* slots,
            std::size_t slotCount);

        // UIの拡大率が変わったときに、ImGuiがピクセル単位で保持しているレイアウト情報を
        // まとめてratio倍する。ImGui::NewFrame()の前に呼ぶこと。対象は次の2つ。
        //
        // (1) ドックノードの寸法(Size / SizeRef)
        //     ImGuiは、中央ノードを含む側と分割されているノードの幅・高さを「絶対ピクセル値」
        //     として保持し、ドックスペースが広がっても維持して増分を中央ノードへ全部渡す
        //     (imgui.cppのDockNodeTreeUpdatePosSize、サイズ配分ポリシーの3番)。
        //     そのため拡大率だけ変えると、文字は大きくなるのにパネルの幅は据え置きとなり、
        //     見た目の比率が変わってしまう。
        //
        // (2) 各ウィンドウが持つ、前フレーム由来のレイアウト情報
        //     スクロールバーの有無・長さ・位置は、Begin()内で前フレームの利用可能サイズ
        //     (InnerRectとScrollbarSizes)と内容サイズから決まるため、これらを合わせておかないと
        //     拡大率が変わったフレームだけ表示が乱れる。
        //     スクロール量と、ドッキングしていない(浮いている)パネルのサイズもここで追従する。
        //
        // ドックノードが存在しない(初回起動でまだレイアウトが組まれていない)場合、
        // (1)は何もせずfalseを返すが、(2)は実行する
        static bool ScaleForUIScaleChange(unsigned int dockSpaceId, float ratio);
    };
}
