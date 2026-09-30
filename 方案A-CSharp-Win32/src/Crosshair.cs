// ============================================================================
//  屏幕准星 Crosshair Overlay —— 方案 A：C# + native Win32
// ----------------------------------------------------------------------------
//  样式：屏幕正中央一个**红色圆点**（默认直径 8 px）。
//        大小、颜色可调，其余一概不做。
//
//  设计原则（对应需求方案.md 的 8 条技术要点）：
//    1) 只用一个原生 Win32 窗口，不引入 WinForms 控件树 —— 消息泵最轻
//    2) WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW
//    3) LWA_COLORKEY 让背景色透明，红点用另一种颜色绘制以保留
//    4) 不开定时器循环重绘 —— 只在「启动」和「参数变化」时重绘一次
//    5) 双缓冲（内存 DC 画完再 BitBlt），杜绝闪烁
//    6) SetProcessDPIAware + GetSystemMetrics 取物理像素，保证居中
//    7) 不注入、不 hook 游戏
//    8) 独占全屏下不可见（Windows 架构限制，非 bug）
// ============================================================================

using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

namespace CrosshairOverlay
{
    /// <summary>
    /// 配置：屏幕中央的一个红色圆点。
    /// 只有两项可调 —— 直径 与 颜色。
    /// 存于 exe 同目录 config.txt，支持热键实时调整并立即落盘。
    /// </summary>
    internal sealed class Config
    {
        public int Diameter = 8;       // 圆点直径（像素）
        public int ColorR = 255;       // 圆点颜色（默认纯红）
        public int ColorG = 0;
        public int ColorB = 0;

        private static string Path =>
            System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "config.txt");

        public Color DotColor => Color.FromArgb(ColorR, ColorG, ColorB);

        public static Config Load()
        {
            var c = new Config();
            try
            {
                if (!File.Exists(Path)) { c.Save(); return c; }
                foreach (var raw in File.ReadAllLines(Path))
                {
                    var line = raw.Trim();
                    if (line.Length == 0 || line.StartsWith("#") || line.StartsWith("//")) continue;
                    int eq = line.IndexOf('=');
                    if (eq <= 0) continue;
                    string k = line.Substring(0, eq).Trim().ToLowerInvariant();
                    string v = line.Substring(eq + 1).Trim();
                    int n;
                    switch (k)
                    {
                        // 兼容旧版配置：老版本用 size 表示臂长，这里映射为直径
                        case "diameter":
                        case "size":
                            if (int.TryParse(v, out n)) c.Diameter = Clamp(n, 2, 200);
                            break;
                        case "color":
                            var rgb = v.Split(',');
                            if (rgb.Length == 3)
                            {
                                int r, g, b;
                                if (int.TryParse(rgb[0], out r) && int.TryParse(rgb[1], out g) && int.TryParse(rgb[2], out b))
                                { c.ColorR = Clamp(r, 0, 255); c.ColorG = Clamp(g, 0, 255); c.ColorB = Clamp(b, 0, 255); }
                            }
                            break;
                    }
                }
            }
            catch { /* 配置坏了就用默认值，绝不因配置问题崩掉 */ }
            return c;
        }

        public void Save()
        {
            try
            {
                var sb = new StringBuilder();
                sb.AppendLine("# 屏幕准星配置 —— 改完保存即可");
                sb.AppendLine("# diameter : 圆点直径（像素），2~200");
                sb.AppendLine("# color    : 圆点颜色 RGB，如 255,0,0 为纯红");
                sb.AppendLine("diameter=" + Diameter);
                sb.AppendLine("color=" + ColorR + "," + ColorG + "," + ColorB);
                File.WriteAllText(Path, sb.ToString(), Encoding.UTF8);
            }
            catch { }
        }

        private static int Clamp(int v, int lo, int hi) => v < lo ? lo : (v > hi ? hi : v);
    }

    /// <summary>
    /// Win32 互操作声明。全部集中在底部，方便对照需求方案里的 8 条要点。
    /// </summary>
    internal static class Native
    {
        public const int GWL_EXSTYLE = -20;

        public const int WS_EX_LAYERED = 0x00080000;
        public const int WS_EX_TRANSPARENT = 0x00000020;
        public const int WS_EX_TOPMOST = 0x00000008;
        public const int WS_EX_TOOLWINDOW = 0x00000080;   // 不出现在 Alt+Tab

        public const int WS_POPUP = unchecked((int)0x80000000);

        public const int LWA_COLORKEY = 0x00000001;
        public const int LWA_ALPHA = 0x00000002;

        public const int SM_CXSCREEN = 0;
        public const int SM_CYSCREEN = 1;

        public const int SW_HIDE = 0;
        public const int SW_SHOWNOACTIVATE = 4;           // 显示但不抢焦点

        public const uint MOD_ALT = 0x0001;
        public const uint MOD_CONTROL = 0x0002;
        public const uint MOD_SHIFT = 0x0004;
        public const uint MOD_NOREPEAT = 0x4000;

        public const int WM_HOTKEY = 0x0312;
        public const int WM_DESTROY = 0x0002;
        public const int WM_APP = 0x8000;
        public const int WM_TRAYICON = WM_APP + 1;
        public const int WM_LBUTTONUP = 0x0202;
        public const int WM_RBUTTONUP = 0x0205;
        public const int WM_PAINT = 0x000F;

        [DllImport("user32.dll", SetLastError = true)]
        public static extern int GetWindowLong(IntPtr hWnd, int nIndex);

        [DllImport("user32.dll", SetLastError = true)]
        public static extern int SetWindowLong(IntPtr hWnd, int nIndex, int dwNewLong);

        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool SetLayeredWindowAttributes(IntPtr hwnd, int crKey, byte bAlpha, int dwFlags);

        [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode, EntryPoint = "RegisterClassExW")]
        public static extern ushort RegisterClassEx(ref WNDCLASSEX lpwcx);

        [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode, EntryPoint = "CreateWindowExW")]
        public static extern IntPtr CreateWindowEx(int dwExStyle, string lpClassName, string lpWindowName,
            int dwStyle, int x, int y, int nWidth, int nHeight,
            IntPtr hWndParent, IntPtr hMenu, IntPtr hInstance, IntPtr lpParam);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode, EntryPoint = "GetModuleHandleW")]
        public static extern IntPtr GetModuleHandle(string lpModuleName);

        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool DestroyWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        public static extern int GetSystemMetrics(int nIndex);

        [DllImport("user32.dll")]
        public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

        [DllImport("user32.dll")]
        public static extern bool SetProcessDPIAware();

        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool RegisterHotKey(IntPtr hWnd, int id, uint fsModifiers, uint vk);

        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool UnregisterHotKey(IntPtr hWnd, int id);

        [DllImport("user32.dll")]
        public static extern IntPtr BeginPaint(IntPtr hWnd, out PAINTSTRUCT lpPaint);

        [DllImport("user32.dll")]
        public static extern bool EndPaint(IntPtr hWnd, ref PAINTSTRUCT lpPaint);

        [StructLayout(LayoutKind.Sequential)]
        public struct PAINTSTRUCT
        {
            public IntPtr hdc;
            public bool fErase;
            public RECT rcPaint;
            public bool fRestore;
            public bool fIncUpdate;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)]
            public byte[] rgbReserved;
        }

        [DllImport("user32.dll")]
        public static extern bool InvalidateRect(IntPtr hWnd, IntPtr lpRect, bool bErase);

        [DllImport("user32.dll")]
        public static extern IntPtr GetDC(IntPtr hWnd);

        [DllImport("user32.dll")]
        public static extern int ReleaseDC(IntPtr hWnd, IntPtr hDC);

        [DllImport("user32.dll")]
        public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);

        [DllImport("gdi32.dll")]
        public static extern bool BitBlt(IntPtr hdcDest, int xDest, int yDest, int w, int h,
            IntPtr hdcSrc, int xSrc, int ySrc, int rop);

        [DllImport("gdi32.dll")]
        public static extern IntPtr CreateCompatibleDC(IntPtr hdc);

        [DllImport("gdi32.dll")]
        public static extern IntPtr CreateCompatibleBitmap(IntPtr hdc, int w, int h);

        [DllImport("gdi32.dll")]
        public static extern IntPtr SelectObject(IntPtr hdc, IntPtr h);

        [DllImport("gdi32.dll")]
        public static extern bool DeleteObject(IntPtr hObject);

        [DllImport("gdi32.dll")]
        public static extern bool DeleteDC(IntPtr hdc);

        public const int SRCCOPY = 0x00CC0020;

        [StructLayout(LayoutKind.Sequential)]
        public struct RECT { public int Left, Top, Right, Bottom; }

        [StructLayout(LayoutKind.Sequential)]
        public struct POINT { public int X, Y; }

        [StructLayout(LayoutKind.Sequential)]
        public struct MSG
        {
            public IntPtr hwnd;
            public uint message;
            public IntPtr wParam;
            public IntPtr lParam;
            public uint time;
            public POINT pt;
        }

        [DllImport("user32.dll")]
        public static extern int GetMessage(out MSG lpMsg, IntPtr hWnd, uint wMsgFilterMin, uint wMsgFilterMax);

        [DllImport("user32.dll")]
        public static extern bool TranslateMessage(ref MSG lpMsg);

        [DllImport("user32.dll")]
        public static extern IntPtr DispatchMessage(ref MSG lpMsg);

        [DllImport("user32.dll")]
        public static extern void PostQuitMessage(int nExitCode);

        [DllImport("user32.dll")]
        public static extern IntPtr DefWindowProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    internal struct WNDCLASSEX
    {
        public uint cbSize;
        public uint style;
        public WndProcDelegate lpfnWndProc;
        public int cbClsExtra;
        public int cbWndExtra;
        public IntPtr hInstance;
        public IntPtr hIcon;
        public IntPtr hCursor;
        public IntPtr hbrBackground;
        public string lpszMenuName;
        public string lpszClassName;
        public IntPtr hIconSm;
    }

    internal delegate IntPtr WndProcDelegate(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    internal static class Program
    {
        // ---------------------------------------------------------------------
        //  常量
        // ---------------------------------------------------------------------
        private const string WINDOW_CLASS = "CrosshairOverlayClass";
        private const string WINDOW_TITLE = "Crosshair Overlay";

        private static readonly Color TransparentKey = Color.FromArgb(1, 2, 3);

        // 热键 ID
        private const int HK_TOGGLE = 1;   // Ctrl+Alt+H 隐藏/显示
        private const int HK_EXIT = 2;     // Ctrl+Alt+Q 退出
        private const int HK_BIGGER = 3;   // Ctrl+Alt+Up 变大
        private const int HK_SMALLER = 4;  // Ctrl+Alt+Down 变小
        private const int HK_HUE = 5;      // Ctrl+Alt+C 换颜色

        // ---------------------------------------------------------------------
        //  状态
        // ---------------------------------------------------------------------
        private static IntPtr _hwnd = IntPtr.Zero;
        private static IntPtr _hotkeyHwnd = IntPtr.Zero;   // 接收全局热键的隐藏窗口
        private static int _screenW;
        private static int _screenH;
        private static Config _cfg;
        private static bool _visible = true;
        private static int _colorIndex;
        private static NotifyIcon _tray;
        private static volatile bool _dirty = true;   // 需要重绘

        // 预设颜色（红系为主，兼顾可辨性）
        private static readonly Color[] Palette =
        {
            Color.FromArgb(255, 0, 0),     // 纯红
            Color.FromArgb(255, 40, 40),   // 亮红
            Color.FromArgb(200, 0, 0),     // 深红
            Color.FromArgb(255, 0, 255),   // 品红
            Color.FromArgb(0, 255, 0),     // 绿
            Color.FromArgb(0, 255, 255),   // 青
            Color.FromArgb(255, 255, 0),   // 黄
            Color.FromArgb(255, 255, 255), // 白
        };

        private static WndProcDelegate _wndProcRef;   // 防 GC 回收

        [STAThread]
        private static void Main()
        {
            // 崩溃时把异常写进 crash.log，方便定位（无控制台窗口可用）
            AppDomain.CurrentDomain.UnhandledException += (s, e) =>
            {
                Log("未处理异常: " + e.ExceptionObject);
            };

            try
            {
                // 自测模式：不创建窗口，把准星渲染到 PNG 并输出几何信息。
                // 用于无人值守的渲染验证（不弹窗、不抢焦点）。
                if (Environment.GetCommandLineArgs().Length > 1
                    && Environment.GetCommandLineArgs()[1].Equals("--selftest", StringComparison.OrdinalIgnoreCase))
                {
                    SelfTest();
                    return;
                }

                Native.SetProcessDPIAware();

                _screenW = Native.GetSystemMetrics(Native.SM_CXSCREEN);
                _screenH = Native.GetSystemMetrics(Native.SM_CYSCREEN);

                _cfg = Config.Load();
                _colorIndex = FindClosestPaletteIndex(_cfg.DotColor);

                if (!CreateOverlayWindow())
                    return;

                CreateTray();

                RunMessageLoop();
            }
            catch (Exception ex)
            {
                Log("[Main] " + ex);
            }
        }

        // ---------------------------------------------------------------------
        //  自测：纯内存渲染，产出 PNG + 几何报告
        // ---------------------------------------------------------------------
        private static void SelfTest()
        {
            Native.SetProcessDPIAware();
            int w = Native.GetSystemMetrics(Native.SM_CXSCREEN);
            int h = Native.GetSystemMetrics(Native.SM_CYSCREEN);

            var cfg = Config.Load();
            // 只截取中心区域，方便肉眼/程序核对
            int crop = 160;
            int cx = w / 2, cy = h / 2;

            var report = new StringBuilder();
            report.AppendLine("=== 准星自测报告（圆点样式）===");
            report.AppendLine("屏幕物理分辨率 : " + w + " x " + h);
            report.AppendLine("圆心坐标       : (" + cx + ", " + cy + ")");
            report.AppendLine("圆点直径       : " + cfg.Diameter + " px");
            report.AppendLine("颜色 RGB       : " + cfg.ColorR + "," + cfg.ColorG + "," + cfg.ColorB);

            using (var bmp = new Bitmap(crop * 2, crop * 2))
            {
                using (var g = Graphics.FromImage(bmp))
                {
                    g.Clear(Color.Black);   // 用黑底便于看清红色圆点
                    g.SmoothingMode = SmoothingMode.AntiAlias;
                    g.PixelOffsetMode = PixelOffsetMode.HighQuality;

                    float local = crop;     // 局部坐标下的圆心
                    float r = cfg.Diameter / 2f;

                    using (var brush = new SolidBrush(cfg.DotColor))
                        g.FillEllipse(brush, local - r, local - r, cfg.Diameter, cfg.Diameter);

                    // 统计像素：确认真有红色被画出来
                    int redCount = 0;
                    for (int y = 0; y < bmp.Height; y++)
                        for (int x = 0; x < bmp.Width; x++)
                        {
                            var p = bmp.GetPixel(x, y);
                            if (p.R > 128 && p.G < 80 && p.B < 80) redCount++;
                        }
                    report.AppendLine("渲染像素数(红) : " + redCount);
                }

                string outPath = System.IO.Path.Combine(
                    AppDomain.CurrentDomain.BaseDirectory, "selftest.png");
                bmp.Save(outPath, System.Drawing.Imaging.ImageFormat.Png);
                report.AppendLine("输出图片       : " + outPath);
            }

            File.WriteAllText(
                System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "selftest.txt"),
                report.ToString(), Encoding.UTF8);
        }

        // ---------------------------------------------------------------------
        //  日志（无控制台，写同目录 crash.log）
        // ---------------------------------------------------------------------
        private static void Log(string text)
        {
            try
            {
                File.AppendAllText(
                    System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "crash.log"),
                    DateTime.Now + " " + text + Environment.NewLine);
            }
            catch { }
        }

        // ---------------------------------------------------------------------
        //  创建窗口
        // ---------------------------------------------------------------------
        private static bool CreateOverlayWindow()
        {
            var hInstance = Native.GetModuleHandle(null);
            _wndProcRef = WndProc;

            var wc = new WNDCLASSEX();
            wc.cbSize = (uint)Marshal.SizeOf(typeof(WNDCLASSEX));
            wc.style = 0;
            wc.lpfnWndProc = _wndProcRef;
            wc.hInstance = hInstance;
            wc.lpszClassName = WINDOW_CLASS;

            ushort atom = Native.RegisterClassEx(ref wc);
            if (atom == 0)
            {
                int err = Marshal.GetLastWin32Error();
                // ERROR_CLASS_ALREADY_EXISTS(1410) 不算真失败，继续走
                if (err != 1410)
                {
                    Log("RegisterClassEx 失败, GetLastError=" + err);
                    return false;
                }
            }

            // 背景色用「透明键色」铺底，SetLayeredWindowAttributes 时该色整体变透明
            int exStyle = Native.WS_EX_LAYERED
                        | Native.WS_EX_TRANSPARENT
                        | Native.WS_EX_TOPMOST
                        | Native.WS_EX_TOOLWINDOW;

            _hwnd = Native.CreateWindowEx(
                exStyle, WINDOW_CLASS, WINDOW_TITLE,
                Native.WS_POPUP,
                0, 0, _screenW, _screenH,
                IntPtr.Zero, IntPtr.Zero, hInstance, IntPtr.Zero);

            if (_hwnd == IntPtr.Zero)
            {
                Log("CreateWindowEx 失败, GetLastError=" + Marshal.GetLastWin32Error());
                return false;
            }

            // 把「透明键色」声明为透明 —— 准星本体颜色必须与之不同
            int key = TransparentKey.R | (TransparentKey.G << 8) | (TransparentKey.B << 16);
            Native.SetLayeredWindowAttributes(_hwnd, key, 0, Native.LWA_COLORKEY);

            Native.ShowWindow(_hwnd, Native.SW_SHOWNOACTIVATE);
            return true;
        }

        // ---------------------------------------------------------------------
        //  热键
        // ---------------------------------------------------------------------
        private static void RegisterHotkeys()
        {
            uint mods = Native.MOD_CONTROL | Native.MOD_ALT | Native.MOD_NOREPEAT;
            var reg = new (int id, string name, uint vk)[]
            {
                (HK_TOGGLE,  "Ctrl+Alt+H 显示/隐藏", 0x48),
                (HK_EXIT,    "Ctrl+Alt+Q 退出",      0x51),
                (HK_BIGGER,  "Ctrl+Alt+↑ 变大",      0x26),
                (HK_SMALLER, "Ctrl+Alt+↓ 变小",      0x28),
                (HK_HUE,     "Ctrl+Alt+C 换色",      0x43),
            };

            var failed = new StringBuilder();
            foreach (var r in reg)
            {
                if (!Native.RegisterHotKey(_hotkeyHwnd, r.id, mods, r.vk))
                    failed.Append(r.name).Append("; ");
            }

            if (failed.Length > 0)
                Log("以下热键被占用，注册失败 → " + failed);
        }

        private static void UnregisterHotkeys()
        {
            for (int id = 1; id <= 5; id++)
                Native.UnregisterHotKey(_hotkeyHwnd, id);
        }

        // ---------------------------------------------------------------------
        //  托盘
        // ---------------------------------------------------------------------
        private static void CreateTray()
        {
            _tray = new NotifyIcon();
            _tray.Icon = SystemIcons.Application;
            _tray.Text = "屏幕准星（红点）";
            _tray.Visible = true;

            var menu = new ContextMenuStrip();
            menu.Items.Add("隐藏 / 显示 (Ctrl+Alt+H)", null, (s, e) => ToggleVisible());
            menu.Items.Add("变大 (Ctrl+Alt+↑)", null, (s, e) => AdjustDiameter(2));
            menu.Items.Add("变小 (Ctrl+Alt+↓)", null, (s, e) => AdjustDiameter(-2));
            menu.Items.Add("换个颜色 (Ctrl+Alt+C)", null, (s, e) => CycleColor());
            menu.Items.Add(new ToolStripSeparator());
            menu.Items.Add("退出 (Ctrl+Alt+Q)", null, (s, e) => ExitApp());

            _tray.ContextMenuStrip = menu;
        }

        // ---------------------------------------------------------------------
        //  参数调整
        // ---------------------------------------------------------------------
        private static void AdjustDiameter(int delta)
        {
            _cfg.Diameter = Math.Max(2, Math.Min(200, _cfg.Diameter + delta));
            _cfg.Save();
            RequestRedraw();
        }

        private static void CycleColor()
        {
            _colorIndex = (_colorIndex + 1) % Palette.Length;
            var c = Palette[_colorIndex];
            _cfg.ColorR = c.R; _cfg.ColorG = c.G; _cfg.ColorB = c.B;
            _cfg.Save();
            RequestRedraw();
        }

        private static int FindClosestPaletteIndex(Color c)
        {
            int best = 0, bestDist = int.MaxValue;
            for (int i = 0; i < Palette.Length; i++)
            {
                int d = (Palette[i].R - c.R) * (Palette[i].R - c.R)
                      + (Palette[i].G - c.G) * (Palette[i].G - c.G)
                      + (Palette[i].B - c.B) * (Palette[i].B - c.B);
                if (d < bestDist) { bestDist = d; best = i; }
            }
            return best;
        }

        private static void ToggleVisible()
        {
            _visible = !_visible;
            Native.ShowWindow(_hwnd, _visible ? Native.SW_SHOWNOACTIVATE : Native.SW_HIDE);
        }

        private static void RequestRedraw()
        {
            _dirty = true;
            Native.InvalidateRect(_hwnd, IntPtr.Zero, false);
        }

        // ---------------------------------------------------------------------
        //  绘制：双缓冲 + GDI+
        //  只在需要时画一次，之后静止 —— 这是性能关键
        // ---------------------------------------------------------------------
        private static void Paint()
        {
            IntPtr hdcScreen = Native.GetDC(_hwnd);
            if (hdcScreen == IntPtr.Zero) return;

            IntPtr hdcMem = Native.CreateCompatibleDC(hdcScreen);
            IntPtr hBmp = Native.CreateCompatibleBitmap(hdcScreen, _screenW, _screenH);
            IntPtr hOld = Native.SelectObject(hdcMem, hBmp);

            try
            {
                using (var g = Graphics.FromHdc(hdcMem))
                {
                    // 1) 先用透明键色铺满整张画布 —— 这部分最终会变透明
                    using (var bg = new SolidBrush(TransparentKey))
                        g.FillRectangle(bg, 0, 0, _screenW, _screenH);

                    // 2) 在屏幕正中画一个红色圆点
                    g.SmoothingMode = SmoothingMode.AntiAlias;
                    g.PixelOffsetMode = PixelOffsetMode.HighQuality;

                    float cx = _screenW / 2f;
                    float cy = _screenH / 2f;
                    float r = _cfg.Diameter / 2f;   // 半径

                    using (var brush = new SolidBrush(_cfg.DotColor))
                        g.FillEllipse(brush, cx - r, cy - r, _cfg.Diameter, _cfg.Diameter);
                }

                // 3) 一次性贴到屏幕，避免闪烁
                Native.BitBlt(hdcScreen, 0, 0, _screenW, _screenH, hdcMem, 0, 0, Native.SRCCOPY);
            }
            finally
            {
                Native.SelectObject(hdcMem, hOld);
                Native.DeleteObject(hBmp);
                Native.DeleteDC(hdcMem);
                Native.ReleaseDC(_hwnd, hdcScreen);
            }
        }

        // ---------------------------------------------------------------------
        //  消息循环
        //  用 WinForms 的消息泵承载（托盘图标依赖它），
        //  但准星窗口本身仍是手写的原生 Win32 窗口，不引入控件树。
        // ---------------------------------------------------------------------
        private static void RunMessageLoop()
        {
            // 首次绘制
            _dirty = false;
            Paint();

            using (var ctx = new TrayContext())
            {
                // 隐藏窗口就绪后再注册热键（句柄此时才有效）
                RegisterHotkeys();
                Application.Run(ctx);
            }
        }

        /// <summary>
        /// 消息泵载体：不创建可见窗体，只注册一个隐藏的 NativeWindow 用来
        /// 接收本线程的 WM_HOTKEY（热键消息 PostThreadMessage 到线程队列）。
        /// </summary>
        private sealed class TrayContext : ApplicationContext
        {
            private readonly HotkeyWindow _hook;

            public TrayContext()
            {
                _hook = new HotkeyWindow();
                _hotkeyHwnd = _hook.Handle;
            }
        }

        /// <summary>隐藏窗口：专门处理全局热键消息。</summary>
        private sealed class HotkeyWindow : NativeWindow, IDisposable
        {
            public HotkeyWindow()
            {
                CreateHandle(new CreateParams());
            }

            protected override void WndProc(ref Message m)
            {
                if (m.Msg == Native.WM_HOTKEY)
                {
                    HandleHotkey(m.WParam.ToInt32());
                    return;
                }
                base.WndProc(ref m);
            }

            public void Dispose()
            {
                DestroyHandle();
            }
        }

        private static void HandleHotkey(int id)
        {
            switch (id)
            {
                case HK_TOGGLE: ToggleVisible(); break;
                case HK_EXIT: ExitApp(); break;
                case HK_BIGGER: AdjustDiameter(2); break;
                case HK_SMALLER: AdjustDiameter(-2); break;
                case HK_HUE: CycleColor(); break;
            }
        }

        private static void ExitApp()
        {
            UnregisterHotkeys();
            if (_tray != null) { _tray.Visible = false; _tray.Dispose(); }
            if (_hwnd != IntPtr.Zero) Native.DestroyWindow(_hwnd);
            Environment.Exit(0);
        }

        // ---------------------------------------------------------------------
        //  窗口过程
        // ---------------------------------------------------------------------
        private static IntPtr WndProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam)
        {
            switch (msg)
            {
                case Native.WM_PAINT:
                {
                    // 必须成对调用 BeginPaint/EndPaint 清除无效区域，
                    // 否则 InvalidateRect 会不断触发 WM_PAINT 造成刷屏死循环。
                    Native.PAINTSTRUCT ps;
                    Native.BeginPaint(hWnd, out ps);
                    if (_dirty) { _dirty = false; Paint(); }
                    Native.EndPaint(hWnd, ref ps);
                    return IntPtr.Zero;
                }

                case Native.WM_TRAYICON:
                    if ((int)lParam == Native.WM_RBUTTONUP && _tray != null)
                        _tray.ContextMenuStrip.Show(Cursor.Position);
                    return IntPtr.Zero;

                case Native.WM_DESTROY:
                    Native.PostQuitMessage(0);
                    return IntPtr.Zero;
            }
            return Native.DefWindowProc(hWnd, msg, wParam, lParam);
        }
    }
}
