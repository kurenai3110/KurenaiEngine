#include "UI/UITheme.h"

#include <Windows.h>

#include <imgui.h>

#include <string>

#include "Core/Logger.h"
#include "Core/StringUtil.h"

namespace Kurenai::UI
{
    namespace
    {
        struct FontCandidate
        {
            const wchar_t* FileName;
            const char* DisplayName;
        };

        // 優先順。いずれもWindows標準同梱のフォント。
        // .ttc(TrueType Collection)はImFontConfig::FontNoで何番目のフォントを使うか選ぶ形式で、
        // 中身の並びは環境依存のため先頭(0)を使う
        constexpr FontCandidate kFontCandidates[] =
        {
            { L"YuGothM.ttc",       "游ゴシック Medium" },
            { L"YuGothR.ttc",       "游ゴシック Regular" },
            { L"meiryo.ttc",        "メイリオ" },
            { L"BIZ-UDGothicR.ttc", "BIZ UDゴシック" },
            { L"msgothic.ttc",      "MS ゴシック" },
        };
    }

    void UITheme::Apply(float uiScale)
    {
        if (uiScale <= 0.0f)
        {
            Core::Logger::Warning(
                "UITheme", "UI拡大率が不正な値(" + std::to_string(uiScale) + ")だったため1.0として扱います");
            uiScale = 1.0f;
        }

        ImGuiStyle& style = ImGui::GetStyle();

        // ScaleAllSizesは現在値に対する乗算なので、呼ぶたびに寸法が累積する。
        // 拡大率を変えて呼び直せるよう、まずImGuiStyleを既定へ完全に戻してから
        // 組み立て直す(この関数を冪等にするための要)
        style = ImGuiStyle();
        ImGui::StyleColorsDark(&style);

        // --- 寸法(DPIスケールを掛ける前の100%基準値) ---
        style.WindowRounding = 4.0f;
        style.ChildRounding = 4.0f;
        style.FrameRounding = 4.0f;
        style.PopupRounding = 4.0f;
        style.ScrollbarRounding = 6.0f;
        style.GrabRounding = 3.0f;
        style.TabRounding = 4.0f;
        style.WindowBorderSize = 0.0f;
        style.FrameBorderSize = 0.0f;
        style.WindowPadding = ImVec2(10.0f, 10.0f);
        style.FramePadding = ImVec2(8.0f, 4.0f);
        style.ItemSpacing = ImVec2(8.0f, 6.0f);
        style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
        style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

        // --- 配色 ---
        // エンジン名「紅(くれない)」に合わせ、操作対象(スライダーのつまみ・チェック・
        // 選択中タブ)だけを赤系のアクセントにして視線を集める。
        // ImGuiCol_の列挙はバージョンによって増減しうるため、ループで一括変換せず必要な色だけ代入する
        const ImVec4 accent = ImVec4(0.72f, 0.15f, 0.22f, 1.00f);
        const ImVec4 accentHovered = ImVec4(0.85f, 0.22f, 0.30f, 1.00f);
        const ImVec4 accentActive = ImVec4(0.62f, 0.10f, 0.17f, 1.00f);

        // Headerは CollapsingHeader と Selectable の背景を兼ねており面積が大きい。
        // ここにアクセント色をそのまま使うとセクションが増えたときに赤い帯だらけになるため、
        // 平常時は暗いグレーに赤をわずかに混ぜる程度に留め、ホバー/選択時だけアクセントにする
        style.Colors[ImGuiCol_Header] = ImVec4(0.30f, 0.20f, 0.22f, 1.00f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.70f);
        style.Colors[ImGuiCol_HeaderActive] = accentActive;
        style.Colors[ImGuiCol_Button] = ImVec4(0.26f, 0.27f, 0.31f, 1.00f);
        style.Colors[ImGuiCol_ButtonHovered] = accentHovered;
        style.Colors[ImGuiCol_ButtonActive] = accentActive;
        style.Colors[ImGuiCol_SliderGrab] = accent;
        style.Colors[ImGuiCol_SliderGrabActive] = accentHovered;
        style.Colors[ImGuiCol_CheckMark] = accentHovered;
        style.Colors[ImGuiCol_TabSelected] = accent;
        style.Colors[ImGuiCol_TabHovered] = accentHovered;
        style.Colors[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.70f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.22f, 0.10f, 0.13f, 1.00f);
        // パネルの背景をわずかに透かし、下の3D映像が見えるようにする
        style.Colors[ImGuiCol_WindowBg].w = 0.94f;

        // --- 拡大率 ---
        // 余白・角丸・スクロールバー幅などの「寸法」はこれで一括拡大する
        style.ScaleAllSizes(uiScale);
        // フォントは寸法とは別系統。1.92ではGetFontSize()が
        // FontSizeBase * FontScaleMain * FontScaleDpi になる。
        // モニタのDPIには追従させないのでFontScaleDpiは1.0のままにし、
        // アプリが決める倍率という意味が明確なFontScaleMainの方を使う。
        // io.ConfigDpiScaleFonts / ConfigDpiScaleViewportsはimgui.hで[EXPERIMENTAL]と
        // 明記されているため使わない
        style.FontSizeBase = kBaseFontSizePixels;
        style.FontScaleMain = uiScale;
        style.FontScaleDpi = 1.0f;
    }

    bool UITheme::LoadFonts()
    {
        ImGuiIO& io = ImGui::GetIO();

        // フォントフォルダは環境によってC:とは限らないためGetWindowsDirectoryWで取得する
        wchar_t windowsDirectory[MAX_PATH]{};
        const UINT length = GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            Core::Logger::Error(
                "UITheme",
                "Windowsディレクトリの取得に失敗しました (GetLastError: " + std::to_string(GetLastError()) +
                    ")。ImGui既定フォント(日本語なし)へフォールバックします");
            io.Fonts->AddFontDefault();
            return false;
        }

        const std::wstring fontDirectory = std::wstring(windowsDirectory) + L"\\Fonts\\";

        for (const FontCandidate& candidate : kFontCandidates)
        {
            const std::wstring path = fontDirectory + candidate.FileName;
            const std::string pathUtf8 = Core::WideToUtf8(path);

            // AddFontFromFileTTFはファイルを開けないとIM_ASSERT_USER_ERRORで停止する
            // (imgui_draw.cpp)。ImFontFlags_NoLoadErrorで抑止したうえで、
            // そもそも読ませないよう事前に存在確認もする(二重の保険)
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                Core::Logger::Info("UITheme", "フォントが見つかりませんでした: " + pathUtf8 + " (次の候補へ)");
                continue;
            }

            ImFontConfig config;
            config.FontNo = 0;
            config.SizePixels = kBaseFontSizePixels;
            // 読み込み失敗時にアサートで止めず、nullptrを返させる
            config.Flags |= ImFontFlags_NoLoadError;
            // GlyphRangesは指定しない。imgui 1.92は動的フォントアトラス(IMGUI_HAS_TEXTURES)を持ち
            // 必要になったグリフだけを実行時にベイクするため、imgui.h上でも*LEGACY*と明記されている。
            // GetGlyphRangesJapanese()を渡すと起動時に約3000グリフを無駄に焼くことになる

            ImFont* font = io.Fonts->AddFontFromFileTTF(pathUtf8.c_str(), kBaseFontSizePixels, &config);
            if (font != nullptr)
            {
                Core::Logger::Info(
                    "UITheme", std::string("UIフォントに ") + candidate.DisplayName + " を使用します (" + pathUtf8 + ")");
                return true;
            }

            Core::Logger::Warning("UITheme", "フォントの読み込みに失敗しました: " + pathUtf8 + " (次の候補へ)");
        }

        // すべて失敗した場合はImGui既定の内蔵フォントへフォールバックする。
        // 日本語グリフを持たないためラベル・ツールチップの日本語は豆腐(□)になるが、
        // 起動自体は成功させる
        io.Fonts->AddFontDefault();
        Core::Logger::Error(
            "UITheme", "日本語フォントを1つも読み込めませんでした。既定フォントで起動します(日本語は表示されません)");
        return false;
    }
}
