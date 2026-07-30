#pragma once

namespace Kurenai::UI
{
    // ImGuiの見た目(配色・寸法)とフォントの設定。
    // ImGuiコンテキスト生成後・最初のNewFrame()より前に、Renderスレッドから呼ぶこと
    class UITheme
    {
    public:
        // ベースフォントサイズ[px]。ImGui既定のProggyClean(13px)より読みやすく、
        // かつ日本語の漢字が潰れないサイズとして16pxを選ぶ
        static constexpr float kBaseFontSizePixels = 16.0f;

        // 配色・寸法とDPIスケールを適用する。
        // 内部でImGuiStyleをいったん既定へ戻してから組み立てるため冪等で、
        // DPIが変わるたびに何度呼び直しても寸法が累積しない。
        // dpiScaleが0以下の場合は1.0として扱い、警告ログを出す
        static void Apply(float dpiScale);

        // 日本語グリフを持つフォントを読み込む。起動時に一度だけ呼ぶ。
        // 読み込めた場合true、すべて失敗してImGui既定フォントへフォールバックした場合false
        // (falseでも起動は継続できるが、日本語は豆腐になる)
        static bool LoadFonts();
    };
}
