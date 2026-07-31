#include "ImGuiContextSetup.h"

#include <imgui.h>

#include "Core/Logger.h"

namespace Kurenai::RHI::Detail
{
    void CreateImGuiContextCommon()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();

        // ドッキングを有効化する。パネルをウィンドウ端へ吸着させたりタブへまとめたりできるようになり、
        // 従来のSetNextWindowPos()による手動の非重なり配置(パネルが伸びるたびにY座標を手で
        // 調整していた)が不要になる
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        // マルチビューポート(ImGuiウィンドウをOSの別ウィンドウとして切り出す機能)は意図的に
        // 無効のままにする。有効にするとパネルがメインウィンドウの外へ出られるようになり、
        // 動作確認で使っているPostMessageベースの自動クリック(対象HWNDのクライアント座標へ
        // メッセージを送る方式)が「どのHWNDへ送ればよいか」を解決できなくなるため。
        // 既定でOFFだが、意図を明示するために明示的に落としておく
        io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;

        // キーボードナビゲーションも無効のままにする。有効にするとImGuiウィンドウにフォーカスが
        // ある間 io.WantCaptureKeyboard が常時trueになり、それを見てWASDカメラ移動を抑止する
        // 仕組み(KurenaiEngine3D::Update)が意図せず効き続けてカメラが動かせなくなる
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;

        // Shiftを押している間だけドッキングを許可する設定は使わない(既定falseのまま明示)
        io.ConfigDockingWithShift = false;

        Core::Logger::Info(
            "ImGui", "ImGuiコンテキストを生成しました(ドッキング有効 / マルチビューポート無効)");
    }
}
