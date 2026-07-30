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

        // UIの大きさの好みを反映する追加倍率。
        //
        // UIの拡大率は Window::GetPhysicalDpiScale()(モニタの実際のピクセル密度から求めた倍率)に
        // 追従し、どのディスプレイでも同じ実寸で表示される。既定ではkBaseFontSizePixelsの文字が
        // どの画面でも約4.2mmになる(16px ÷ 96 DPI = 0.167インチ)。
        //
        // 全体をもう少し大きく/小さくしたい場合だけこの値を変える。1.2fにすれば全モニタで
        // 一律1.2倍になり、モニタ間で実寸が揃う性質はそのまま保たれる
        static constexpr float kUIScaleMultiplier = 1.0f;

        // 配色・寸法とUI拡大率を適用する。
        // 内部でImGuiStyleをいったん既定へ戻してから組み立てるため冪等で、
        // 何度呼び直しても寸法が累積しない。
        // uiScaleが0以下の場合は1.0として扱い、警告ログを出す
        static void Apply(float uiScale);

        // 日本語グリフを持つフォントを読み込む。起動時に一度だけ呼ぶ。
        // 読み込めた場合true、すべて失敗してImGui既定フォントへフォールバックした場合false
        // (falseでも起動は継続できるが、日本語は豆腐になる)
        static bool LoadFonts();
    };
}
