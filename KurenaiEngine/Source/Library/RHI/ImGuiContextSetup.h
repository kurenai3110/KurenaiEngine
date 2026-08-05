#pragma once

namespace Kurenai::RHI::Detail
{
    // DX11ImGuiBackend / DX12ImGuiBackend が共通で行うImGuiコンテキストの生成と共通設定。
    // 両バックエンドに同じコード(IMGUI_CHECKVERSION / CreateContext / StyleColorsDark)を
    // 複製せず、ここへ集約する。
    //
    // スタイル・フォント・IniFilenameは「見た目とデータの置き場所」であってRHIの責務ではないため
    // ここでは一切触らない。それらはKurenaiEngine3D側(UI::UITheme)が設定する。
    //
    // 失敗しうる処理はImGui::CreateContext()のみで、これは確保に失敗するとImGui内部の
    // アサートで停止するため、この関数自体は戻り値を持たない。呼び出し側(各バックエンドの
    // コンストラクタ)がImGui_ImplXXX_Init()の失敗を検出して例外を投げる既存の流れは変えない
    void CreateImGuiContextCommon();
}
