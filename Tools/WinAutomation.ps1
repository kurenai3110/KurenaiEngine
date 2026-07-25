# Claudeによる動作確認(PostMessageベースの入力自動化・スクリーンショット)用ヘルパー。
# 使い方: 各PowerShell呼び出しの冒頭で `. Tools\WinAutomation.ps1` としてドットソースする
# (Add-Typeでの型登録はプロセスごとに1回のみ有効なため、新しいPowerShellセッションのたびに
# 再度ドットソートし直す必要がある)。
#
# 対象ウィンドウのHWNDは`Get-Process -Id <pid> | .MainWindowHandle`で取得できるが、
# ウィンドウが最小化されていると`GetClientRect`が0x0を返す(=DwmGetWindowAttributeも
# 不定値を返す)ため、操作前に必ず`ShowWindow($hwnd, 9)`(SW_RESTORE)で復元しておくこと。
#
# 詳細な経緯・注意点はCLAUDE.mdの「Claudeによる動作確認時の入力操作方針」
# 「Claudeによるスクリーンショットの撮り方」を参照。

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Drawing;
using System.Drawing.Imaging;

public static class WinAuto
{
    [DllImport("dwmapi.dll")]
    public static extern int DwmGetWindowAttribute(IntPtr hwnd, int dwAttribute, out RECT pvAttribute, int cbAttribute);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);

    public struct RECT { public int Left, Top, Right, Bottom; }

    public const int WM_MOUSEMOVE = 0x0200;
    public const int WM_LBUTTONDOWN = 0x0201;
    public const int WM_LBUTTONUP = 0x0202;
    public const int WM_KEYDOWN = 0x0100;
    public const int WM_KEYUP = 0x0101;
    public const int MK_LBUTTON = 0x0001;

    public static IntPtr MakeLParam(int x, int y)
    {
        return (IntPtr)((y << 16) | (x & 0xFFFF));
    }

    // GetWindowRectはDWMの不可視リサイズ枠を含んだ座標を返すため撮影範囲がズレる。
    // DWMWA_EXTENDED_FRAME_BOUNDS(値9)で実際に見えているウィンドウ境界(物理ピクセル)を取得する
    public static RECT GetExtendedFrameBounds(IntPtr hwnd)
    {
        RECT rect;
        const int DWMWA_EXTENDED_FRAME_BOUNDS = 9;
        DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, out rect, Marshal.SizeOf(typeof(RECT)));
        return rect;
    }

    // CaptureWindowDirect()で撮ったスクリーンショット上のピクセル座標(タイトルバー込みの
    // 物理ピクセル)を、PostMessageのlParamに使うクライアント論理座標へ変換する。
    // Windowsのディスプレイ拡大率(例: 150%)が100%でない環境では、ウィンドウの内部解像度
    // (KurenaiEngine3Dは既定1280x720固定)と実際の画面上の物理ピクセルサイズが一致しないため、
    // スクリーンショットで目視したピクセル位置をそのままPostMessageの座標として使うと
    // クリック位置が大きくズレる(実際に発生し、ボタンを外し続けてクリックが無反応になった)。
    // GetClientRect(ウィンドウ自身の論理サイズ、DPI仮想化されない)とGetExtendedFrameBounds
    // (物理ピクセル)の比率からスケールとタイトルバー高さを直接計算することで、
    // ClientToScreen(呼び出し元プロセスのDPI認識状態によって値が化けることがある)に
    // 頼らずに変換できる
    public static void ScreenshotPixelToClient(IntPtr hwnd, int shotX, int shotY, out int clientX, out int clientY)
    {
        RECT frameBounds = GetExtendedFrameBounds(hwnd);
        RECT clientRect;
        GetClientRect(hwnd, out clientRect);

        double scale = (double)(frameBounds.Right - frameBounds.Left) / clientRect.Right;
        double titleBarPhysical = (frameBounds.Bottom - frameBounds.Top) - (clientRect.Bottom * scale);

        clientX = (int)(shotX / scale);
        clientY = (int)((shotY - titleBarPhysical) / scale);
    }

    // WM_MOUSEMOVE→WM_LBUTTONDOWN→WM_LBUTTONUPを間隔を空けずに連続送信する
    // (間隔を空けると実カーソル位置ベースのDear ImGuiのフォールバック/WM_MOUSELEAVE検出に
    // よってクリック前にホバー状態が上書きされてしまう。詳細はCLAUDE.md参照)
    public static void ClickClient(IntPtr hwnd, int x, int y)
    {
        IntPtr lp = MakeLParam(x, y);
        PostMessage(hwnd, (uint)WM_MOUSEMOVE, IntPtr.Zero, lp);
        PostMessage(hwnd, (uint)WM_LBUTTONDOWN, (IntPtr)MK_LBUTTON, lp);
        PostMessage(hwnd, (uint)WM_LBUTTONUP, IntPtr.Zero, lp);
    }

    // ウィンドウの見た目上の内容をPrintWindow(PW_RENDERFULLCONTENT)で直接キャプチャする。
    // CopyFromScreen(実画面のピクセルをコピーする方式)は対象ウィンドウが実際に画面の最前面に
    // 出ていることが前提だが、自動化スクリプト(バックグラウンドのPowerShellプロセス)からの
    // SetForegroundWindowはWindowsのフォアグラウンドロックにより戻り値がtrueでも実際には
    // 最前面化されないことがある(実際に発生: 無関係な別ウィンドウ(IDE)がスクリーンショットに
    // 写り込み続けた)。PrintWindowはZオーダー/被覆状態に関係なくDWMにウィンドウの内容を
    // 直接描画させるため、この問題を回避できる(DirectX描画のウィンドウでも
    // PW_RENDERFULLCONTENTフラグ指定でハードウェアアクセラレーション内容が正しく取得できる)
    public static void CaptureWindowDirect(IntPtr hwnd, string outPath)
    {
        RECT rect = GetExtendedFrameBounds(hwnd);
        int width = rect.Right - rect.Left;
        int height = rect.Bottom - rect.Top;
        const uint PW_RENDERFULLCONTENT = 0x00000002;
        using (Bitmap bmp = new Bitmap(width, height))
        {
            using (Graphics g = Graphics.FromImage(bmp))
            {
                IntPtr hdc = g.GetHdc();
                try
                {
                    PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT);
                }
                finally
                {
                    g.ReleaseHdc(hdc);
                }
            }
            bmp.Save(outPath, ImageFormat.Png);
        }
    }
}
"@ -ReferencedAssemblies System.Drawing

