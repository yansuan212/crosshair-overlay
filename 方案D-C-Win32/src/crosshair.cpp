/*
 * crosshair.cpp - 屏幕准星（Crosshair Overlay）方案 D：纯 C + Win32，无第三方框架
 *
 * 设计要点（对标 GitHub 上的 simple-crosshair-overlay / mesrefoglu/Crosshair）：
 *   1. 分层窗口 WS_EX_LAYERED + 鼠标穿透 WS_EX_TRANSPARENT + 置顶 WS_EX_TOPMOST
 *   2. 用 SetLayeredWindowAttributes(LWA_COLORKEY) 把背景色声明为透明
 *      -> 窗口全屏覆盖，但只有准星线条可见，其余区域完全透明
 *   3. 性能秘诀：不开定时器循环重绘。只在启动时画一次；参数变化时按需重绘。
 *      平时窗口静止，CPU 占用为 0。
 *   4. 双缓冲：先画到内存 DC，再一次性 BitBlt 贴出，避免闪烁。
 *   5. DPI 感知：SetProcessDPIAware() + GetSystemMetrics(SM_CXSCREEN)
 *      取物理像素，保证 4K / 缩放环境下准星不偏离屏幕中心。
 *
 * 交互（极简）：
 *   - 启动即显示，无窗口、无任务栏图标，仅一个托盘图标
 *   - 托盘右键菜单：显示/隐藏、调大小、换色、重载配置、打开配置文件、退出
 *   - 全局热键（与方案 A 对齐）：
 *       Ctrl+Alt+H     显示 / 隐藏准星
 *       Ctrl+Alt+↑     变大（直径 +2 px，立即写回 ini）
 *       Ctrl+Alt+↓     变小（直径 -2 px，立即写回 ini）
 *       Ctrl+Alt+C     循环换色（红→亮红→深红→品红→绿→青→黄→白）
 *       Ctrl+Alt+I     重新加载配置（改完 crosshair.ini 后按一下即可生效）
 *       Ctrl+Alt+Q     退出
 *     ⚠ 原本重载键用 Ctrl+Alt+R，但被 NVIDIA Overlay 占用，故改为 I。
 *       换热键前请先跑 tools/hotkey_probe.exe 探测占用情况。
 *
 * 配置：exe 同目录的 crosshair.ini，改动后按 Ctrl+Alt+I 生效。
 *       用 ↑/↓/C 调出来的参数会立即写回 ini，重启后依然保持。
 *
 * 编译（MinGW-w64）：
 *   g++ -O2 -s -mwindows -o crosshair.exe src/crosshair.cpp -lgdi32 -luser32 -lshell32 -lole32
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

/* 目标 Windows 版本：Win7 以上，保证 SetProcessDPIAware / MOD_NOREPEAT
 * 等较新的 API 声明可见（MinGW 默认 _WIN32_WINNT 偏低会隐藏它们） */
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

/* MinGW 8.1 的头文件里没有这两个定义，手动补上 */
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif
/* SetProcessDPIAware 在 user32 中导出，ProgMan 头文件缺失时手动声明 */
#ifndef DECLSPEC_HAS_DPIAWARE
typedef BOOL (WINAPI *PFN_SetProcessDPIAware)(void);
#endif

/* ------------------------------------------------------------------ */
/* 常量定义                                                            */
/* ------------------------------------------------------------------ */

#define APP_CLASS_NAME      L"CrosshairOverlayWnd"
#define APP_TITLE           L"屏幕准星"
#define DEFAULT_INI_NAME    L"crosshair.ini"

/* 用来做透明键的背景色。选一个几乎不会被用到的颜色（品红），
 * 窗口背景刷成这个色 -> LWA_COLORKEY 把该色当作透明 -> 只露出准星线条 */
#define TRANSPARENT_KEY     RGB(255, 0, 255)

/* 托盘图标及菜单 ID */
#define WM_TRAYICON         (WM_USER + 1)
#define IDM_TOGGLE          1001
#define IDM_RELOAD          1002
#define IDM_OPEN_INI        1003
#define IDM_ABOUT           1004
#define IDM_EXIT            1005
#define IDM_OPENGITHUB      1006
#define IDM_BIGGER          1007
#define IDM_SMALLER         1008
#define IDM_HUE             1009

/* 全局热键 ID
 *
 * ⚠ 热键选型避坑记录（2026-09-30 实测）：
 *   本机 Ctrl+Alt+R 被 NVIDIA Overlay 占用（游戏内录制），
 *   注册会静默失败。实测已被占用的组合：
 *       Ctrl+Alt+{ F, L, M, O, R, X, Z }
 *   故重载配置改用 Ctrl+Alt+I。
 *   （改热键前请先跑 tools/hotkey_probe.exe 探测，别凭感觉挑） */
#define HOTKEY_TOGGLE       101   /* Ctrl+Alt+H    */
#define HOTKEY_RELOAD       102   /* Ctrl+Alt+I    */
#define HOTKEY_EXIT         103   /* Ctrl+Alt+Q    */
#define HOTKEY_BIGGER       104   /* Ctrl+Alt+Up   */
#define HOTKEY_SMALLER      105   /* Ctrl+Alt+Down */
#define HOTKEY_HUE          106   /* Ctrl+Alt+C    */

/* ------------------------------------------------------------------ */
/* 配置结构                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    int   size;        /* 准星单臂长度（像素），从中心向外延伸 */
    int   thickness;   /* 线条粗细（像素） */
    int   gap;         /* 中心空隙（像素），0 = 实心十字，无空隙 */
    int   colorR;      /* 红色分量 0-255 */
    int   colorG;      /* 绿色分量 0-255 */
    int   colorB;      /* 蓝色分量 0-255 */
    int   opacity;     /* 图层整体不透明度 0-255，255 = 完全不透明 */
    int   centerDot;   /* 是否画中心点：0/1 */
    int   dotSize;     /* 中心点直径（像素） */
    int   outline;     /* 是否加描边（黑色细边，提升亮背景下的可读性）：0/1 */
    int   monitor;     /* 显示器序号，0 = 主显示器，1/2/... = 第 n 块 */
} Config;

static Config g_cfg;
static int    g_visible = 1;   /* 准星当前是否可见 */

/* 运行时句柄 */
static HWND  g_hwnd        = NULL;
static HDC   g_memDC       = NULL;   /* 内存 DC（双缓冲） */
static HBITMAP g_memBmp    = NULL;
static int   g_screenW     = 0;      /* 物理像素宽度 */
static int   g_screenH     = 0;      /* 物理像素高度 */
static int   g_originX     = 0;      /* 准星中心 X（屏幕物理坐标） */
static int   g_originY     = 0;      /* 准星中心 Y（屏幕物理坐标） */

/* ------------------------------------------------------------------ */
/* 工具函数                                                            */
/* ------------------------------------------------------------------ */

/* 取 exe 所在目录，拼出 crosshair.ini 的完整路径 */
static void GetIniPath(wchar_t* buf, int cch)
{
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    /* 去掉文件名，保留目录（含结尾反斜杠） */
    wchar_t* p = wcsrchr(exePath, L'\\');
    if (p) *(p + 1) = L'\0';

    _snwprintf(buf, cch, L"%s%s", exePath, DEFAULT_INI_NAME);
    buf[cch - 1] = L'\0';
}

/* ------------------------------------------------------------------ */
/* 落盘日志（WinExe 没有控制台，出问题只能靠日志排查）                 */
/* 日志文件：exe 同目录 crosshair.ini.log                              */
/*                                                                     */
/* ⚠ MinGW 的 _wfopen(..., L"ccs=UTF-8") 对宽字符输出支持不完整，      */
/*   混合 %s/%ls 会输出错乱。改为手动把宽字符串转 UTF-8 后以二进制写。 */
/* ------------------------------------------------------------------ */

static void LogWriteUtf8(const wchar_t* text)
{
    int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (need <= 0) return;

    char* buf = (char*)malloc(need);
    if (!buf) return;

    WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, need, NULL, NULL);

    wchar_t ini[MAX_PATH];
    GetIniPath(ini, MAX_PATH);
    wchar_t logPath[MAX_PATH + 8];
    _snwprintf(logPath, MAX_PATH + 8, L"%s.log", ini);
    logPath[MAX_PATH + 7] = L'\0';

    FILE* fp = _wfopen(logPath, L"ab");   /* 二进制追加，不经过任何编码转换 */
    if (fp) {
        fwrite(buf, 1, need - 1, fp);     /* 不写结尾的 \0 */
        fwrite("\r\n", 1, 2, fp);
        fclose(fp);
    }
    free(buf);
}

/* 日志：统一用宽字符串组装，最后一次性转 UTF-8 写出 */
static void LogLine(const wchar_t* fmt, ...)
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t body[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(body, 1024, fmt, ap);
    va_end(ap);
    body[1023] = L'\0';

    wchar_t full[1200];
    _snwprintf(full, 1200, L"[%02d:%02d:%02d] %s",
               st.wHour, st.wMinute, st.wSecond, body);
    full[1199] = L'\0';

    LogWriteUtf8(full);
}

/* 读整数配置项，带默认值与范围钳制。
 *
 * 注：GetPrivateProfileIntW 本身对 UTF-8(含BOM) / ANSI 的 ini 都能正确解析，
 * 实测已确认（Windows Vista+ 支持 BOM 识别）。此处保持简单实现。
 * 唯一要注意的是：钳制下界若为 1，用户把 size 写成 0 会被钳到 1，
 * 准星变成 1x1 像素、几乎不可见 —— 这是"看起来没生效"的常见错觉，
 * 已在自动生成的 ini 注释里写明。 */
static int ReadInt(const wchar_t* ini, const wchar_t* section,
                   const wchar_t* key, int def, int lo, int hi)
{
    int v = GetPrivateProfileIntW(section, key, def, ini);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

/* 加载配置。文件不存在则写入一份默认配置。 */
static void LoadConfig(void)
{
    wchar_t ini[MAX_PATH];
    GetIniPath(ini, MAX_PATH);

    /* 首次运行：生成默认配置文件，方便用户直接改。
     * 注意：这里用 GBK（系统 ANSI 代码页）写出，保证 GetPrivateProfileIntW
     *       能正确解析带中文注释的 ini（UTF-8 无 BOM 时该 API 会读失败）。 */
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) {
        const char* defUtf8 =
            "; ============================================================\n"
            ";  屏幕准星配置文件（圆点样式）\n"
            ";  修改后保存，按 Ctrl+Alt+I 热键重新加载（无需重启程序）\n"
            ";  ------------------------------------------------------------\n"
            ";  注意：size 不要填 0，最小值会被钳到 1，红点会小到看不见。\n"
            "; ============================================================\n"
            "\n"
            "[crosshair]\n"
            "; 红点直径（像素）。建议 3 - 30\n"
            "size=8\n"
            "; 以下两项为旧十字样式的参数，圆点模式不使用，保留仅为兼容\n"
            "thickness=2\n"
            "gap=0\n"
            "; 颜色（红 绿 蓝，0-255）。默认纯红\n"
            "colorR=255\n"
            "colorG=0\n"
            "colorB=0\n"
            "; 整体不透明度 0-255，255 = 完全不透明\n"
            "opacity=230\n"
            "; 以下两项为旧的中心点参数，圆点模式不使用，保留仅为兼容\n"
            "centerDot=0\n"
            "dotSize=3\n"
            "; 是否给红点加黑色描边 0/1。加了亮背景下更清晰，但红点显小\n"
            "outline=0\n"
            "; 显示器序号：0 = 主显示器，1 = 第二块，以此类推\n"
            "monitor=0\n";

        /* UTF-8 -> 系统 ANSI(GBK) 转换后写出 */
        int need = MultiByteToWideChar(CP_UTF8, 0, defUtf8, -1, NULL, 0);
        wchar_t* wbuf = (wchar_t*)malloc(sizeof(wchar_t) * need);
        FILE* fp = NULL;
        if (wbuf) {
            MultiByteToWideChar(CP_UTF8, 0, defUtf8, -1, wbuf, need);
            int ansiNeed = WideCharToMultiByte(CP_ACP, 0, wbuf, -1, NULL, 0, NULL, NULL);
            char* ansi = (char*)malloc(ansiNeed);
            if (ansi) {
                WideCharToMultiByte(CP_ACP, 0, wbuf, -1, ansi, ansiNeed, NULL, NULL);
                fp = _wfopen(ini, L"wb");
                if (fp) {
                    fwrite(ansi, 1, ansiNeed - 1, fp);   /* 不含结尾 \0 */
                    fclose(fp);
                }
                free(ansi);
            }
            free(wbuf);
        }
    }

    const wchar_t* SEC = L"crosshair";
    /* size = 红点直径（像素） */
    g_cfg.size      = ReadInt(ini, SEC, L"size",       8,  2, 400);
    g_cfg.thickness = ReadInt(ini, SEC, L"thickness",  2,  1,  50);
    g_cfg.gap       = ReadInt(ini, SEC, L"gap",        0,  0, 200);
    g_cfg.colorR    = ReadInt(ini, SEC, L"colorR",   255,  0, 255);
    g_cfg.colorG    = ReadInt(ini, SEC, L"colorG",     0,  0, 255);
    g_cfg.colorB    = ReadInt(ini, SEC, L"colorB",     0,  0, 255);
    g_cfg.opacity   = ReadInt(ini, SEC, L"opacity",  230,  1, 255);
    g_cfg.centerDot = ReadInt(ini, SEC, L"centerDot",  0,  0,   1);
    g_cfg.dotSize   = ReadInt(ini, SEC, L"dotSize",    3,  1, 100);
    g_cfg.outline   = ReadInt(ini, SEC, L"outline",    0,  0,   1);
    g_cfg.monitor   = ReadInt(ini, SEC, L"monitor",    0,  0,  15);
}

/* ------------------------------------------------------------------ */
/* 就地更新 ini 里的键值（热键调完参数后落盘用）                       */
/*                                                                     */
/* 为什么不用 WritePrivateProfileStringW：                              */
/*   它会重排整个文件、丢掉注释；而且本 ini 是 GBK 编码，交给它写回      */
/*   中文注释容易出问题。                                              */
/* 这里自己读全文件，只替换目标行的值，其余字节原样保留 —— 编码不变。    */
/* ------------------------------------------------------------------ */
static void WriteIniValues(const wchar_t* iniPath,
                           const char* const* keys,
                           const char* const* vals,
                           int n)
{
    if (n <= 0) return;

    FILE* fp = _wfopen(iniPath, L"rb");
    if (!fp) return;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) { fclose(fp); return; }

    char* src = (char*)malloc((size_t)size + 1);
    if (!src) { fclose(fp); return; }
    size_t rd = fread(src, 1, (size_t)size, fp);
    src[rd] = '\0';
    fclose(fp);

    /* 输出缓冲：每行都可能从 LF 补成 CRLF（各多 1 字节），
     * 最坏也要能装下 2 倍输入，再留一点余量。 */
    size_t cap = (size_t)rd * 2 + 1024;
    char* out = (char*)malloc(cap);
    if (!out) { free(src); return; }

    size_t oi = 0;
    const char* p = src;
    int replaced = 0;

    while (*p) {
        const char* lineEnd = p;
        while (*lineEnd && *lineEnd != '\n') lineEnd++;
        size_t lineLen = (size_t)(lineEnd - p);

        /* 去掉行尾的 \r 与空白 */
        size_t contentLen = lineLen;
        while (contentLen > 0 &&
               (p[contentLen-1] == '\r' || p[contentLen-1] == ' ' || p[contentLen-1] == '\t'))
            contentLen--;

        /* 跳过前导空白后判断是不是 "key=" 行 */
        size_t lead = 0;
        while (lead < contentLen && (p[lead] == ' ' || p[lead] == '\t')) lead++;

        int hit = -1;
        if (lead < contentLen && p[lead] != ';' && p[lead] != '#') {
            for (int i = 0; i < n; ++i) {
                size_t klen = strlen(keys[i]);
                if (contentLen - lead >= klen + 1 &&
                    strncmp(p + lead, keys[i], klen) == 0 &&
                    p[lead + klen] == '=') {
                    hit = i;
                    break;
                }
            }
        }

        /* 按「这一行实际需要多少字节」判断能否写下 —— 不能用固定阈值，
         * 否则输出接近容量上限时会整行丢失（曾把 ini 尾部截断）。 */
        size_t need = (hit >= 0)
                    ? (strlen(keys[hit]) + strlen(vals[hit]) + 4)
                    : (contentLen + 2);
        if (oi + need <= cap) {
            if (hit >= 0) {
                int w = snprintf(out + oi, cap - oi, "%s=%s\r\n", keys[hit], vals[hit]);
                if (w > 0) oi += (size_t)w;
                replaced = 1;
            } else {
                memcpy(out + oi, p, contentLen);
                oi += contentLen;
                out[oi++] = '\r';
                out[oi++] = '\n';
            }
        }

        p = (*lineEnd == '\n') ? lineEnd + 1 : lineEnd;
    }

    if (replaced) {
        FILE* wf = _wfopen(iniPath, L"wb");
        if (wf) { fwrite(out, 1, oi, wf); fclose(wf); }
    }

    free(src);
    free(out);
}

/* ------------------------------------------------------------------ */
/* 换色用的候选调色板（与方案 A 保持一致）                             */
/* ------------------------------------------------------------------ */
static const int kPalette[8][3] = {
    { 255,   0,   0 },   /* 纯红（默认） */
    { 255,  40,  40 },   /* 亮红 */
    { 200,   0,   0 },   /* 深红 */
    { 255,   0, 255 },   /* 品红 */
    {   0, 255,   0 },   /* 绿 */
    {   0, 255, 255 },   /* 青 */
    { 255, 255,   0 },   /* 黄 */
    { 255, 255, 255 },   /* 白 */
};
#define PALETTE_COUNT 8

/* 找与当前颜色最接近的调色板下标，作为循环换色的起点 */
static int NearestPaletteIndex(void)
{
    int best = 0;
    long bestD = -1;
    for (int i = 0; i < PALETTE_COUNT; ++i) {
        long dr = (long)kPalette[i][0] - g_cfg.colorR;
        long dg = (long)kPalette[i][1] - g_cfg.colorG;
        long db = (long)kPalette[i][2] - g_cfg.colorB;
        long d  = dr * dr + dg * dg + db * db;
        if (bestD < 0 || d < bestD) { bestD = d; best = i; }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* 计算准星中心位置（支持多显示器 + DPI 物理像素）                     */
/* ------------------------------------------------------------------ */

static void ComputeOrigin(void)
{
    /* 声明进程 DPI 感知后，这些 API 返回的就是物理像素 */
    g_screenW = GetSystemMetrics(SM_CXSCREEN);
    g_screenH = GetSystemMetrics(SM_CYSCREEN);

    g_originX = g_screenW / 2;
    g_originY = g_screenH / 2;

    /* 若指定了非主显示器，改用该显示器的实际区域中心 */
    if (g_cfg.monitor > 0) {
        DISPLAY_DEVICEW dd;
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);

        /* 先枚举出目标显示器的设备名 */
        wchar_t targetDevice[CCHDEVICENAME] = {0};
        int idx = 0;
        for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); ++i) {
            if (dd.StateFlags & DISPLAY_DEVICE_ACTIVE) {
                if (idx == g_cfg.monitor) {
                    wcsncpy(targetDevice, dd.DeviceName, CCHDEVICENAME - 1);
                    break;
                }
                idx++;
            }
            ZeroMemory(&dd, sizeof(dd));
            dd.cb = sizeof(dd);
        }

        if (targetDevice[0]) {
            DEVMODEW dm;
            ZeroMemory(&dm, sizeof(dm));
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(targetDevice, ENUM_CURRENT_SETTINGS, &dm)) {
                /* dmPosition 是虚拟桌面坐标，需要减去主显示器的起点偏移 */
                int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
                int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
                (void)vx; (void)vy;
                g_originX = dm.dmPosition.x - GetSystemMetrics(SM_XVIRTUALSCREEN)
                            + (int)dm.dmPelsWidth / 2;
                g_originY = dm.dmPosition.y - GetSystemMetrics(SM_YVIRTUALSCREEN)
                            + (int)dm.dmPelsHeight / 2;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 绘制准星：32 位 ARGB 内存 DIB -> UpdateLayeredWindow                 */
/*                                                                     */
/* 为什么不用 LWA_COLORKEY + BitBlt：                                  */
/*   分层窗口(WS_EX_LAYERED)的内容必须通过 UpdateLayeredWindow 提交，   */
/*   直接 BitBlt 到窗口 DC 不会生效/会闪烁。且 LWA_COLORKEY 与          */
/*   LWA_ALPHA 互斥，无法同时控制整体不透明度。                         */
/*   => 正确做法：自己维护一张 32 位带 alpha 的位图，逐像素填充，        */
/*      再用 UpdateLayeredWindow 一次性提交。这就是 alpha=0 全透明、    */
/*      准星线条 alpha=opacity 的效果，且 CPU 只在重绘时消耗。          */
/* ------------------------------------------------------------------ */

/* 画一个抗锯齿实心圆点（带可选黑色描边）。
 *
 * 抗锯齿做法：对每个像素算到圆心的距离 d。
 *   d <= r_in  -> 完全填充
 *   d >= r_out -> 完全透明
 *   中间       -> 按覆盖率做 alpha 混合（这就是边缘平滑的来源）
 * 描边同理：在 r 和 r+outlineW 之间画一圈黑色。
 *
 * fillR/G/B  主色；outlineW 描边宽度（0 = 不描边）
 */
static void DrawDotARGB(void* bits, int bmpW, int bmpH,
                        int cx, int cy, double radius,
                        unsigned char a,
                        unsigned char fillR, unsigned char fillG, unsigned char fillB,
                        int outlineW)
{
    double rOuter = radius + (double)outlineW + 1.0;   /* 多留 1 像素给抗锯齿过渡 */

    int x0 = (int)(cx - rOuter); if (x0 < 0) x0 = 0;
    int y0 = (int)(cy - rOuter); if (y0 < 0) y0 = 0;
    int x1 = (int)(cx + rOuter) + 1; if (x1 > bmpW) x1 = bmpW;
    int y1 = (int)(cy + rOuter) + 1; if (y1 > bmpH) y1 = bmpH;

    unsigned char* p = (unsigned char*)bits;

    for (int y = y0; y < y1; ++y) {
        unsigned int* row = (unsigned int*)(p + (size_t)y * bmpW * 4);
        double dy = (double)y + 0.5 - (double)cy;

        for (int x = x0; x < x1; ++x) {
            double dx = (double)x + 0.5 - (double)cx;
            double d  = sqrt(dx * dx + dy * dy);

            /* 该像素属于哪一层？越内层优先级越高 */
            unsigned char sr, sg, sb;
            double covFill = 0.0, covLine = 0.0;

            if (outlineW > 0) {
                /* 外圈：黑色描边 */
                covLine = radius + 0.5 - d;          /* >0 表示在内 */
                if (covLine > 1.0) covLine = 1.0;    /* 完全覆盖 */
                /* 填充部分 */
                covFill = radius - (double)outlineW + 0.5 - d;
                if (covFill > 1.0) covFill = 1.0;
            } else {
                covFill = radius + 0.5 - d;
                if (covFill > 1.0) covFill = 1.0;
            }

            if (covLine <= 0.0 && covFill <= 0.0) continue;   /* 完全在外面 */

            if (covLine > covFill) {
                sr = 0; sg = 0; sb = 0;
            } else {
                sr = fillR; sg = fillG; sb = fillB;
            }

            double cov = covFill > covLine ? covFill : covLine;
            if (cov <= 0.0) continue;
            if (cov > 1.0) cov = 1.0;

            int outA = (int)((double)a * cov + 0.5);
            if (outA <= 0) continue;
            if (outA > 255) outA = 255;

            row[x] = ((unsigned int)outA << 24) |
                     ((unsigned int)sr << 16) |
                     ((unsigned int)sg <<  8) |
                     ((unsigned int)sb);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 把准星渲染到一块 32 位 ARGB 缓冲（与窗口无关，自检也走这条路径）    */
/*   bits     : 目标缓冲，尺寸 w*h，每像素 4 字节 BGRA                  */
/*   cx, cy   : 圆心坐标（缓冲内坐标）                                 */
/* ------------------------------------------------------------------ */
static void RenderCrosshairToBuffer(void* bits, int w, int h, int cx, int cy)
{
    if (!bits || w <= 0 || h <= 0) return;

    /* 1. 整块清零 = 全透明（alpha=0） */
    memset(bits, 0, (size_t)w * h * 4);

    /* 2. 画一个圆的红色实心点 */
    unsigned char cr = (unsigned char)g_cfg.colorR;
    unsigned char cg = (unsigned char)g_cfg.colorG;
    unsigned char cb = (unsigned char)g_cfg.colorB;
    unsigned char a  = (unsigned char)g_cfg.opacity;
    int len = g_cfg.size;

    /* ini 的 size 是"直径"（更符合直觉：看到的圆有多大就填多少），
     * 这里换算成半径。最小半径 1（直径 2），保证不会小到看不见。 */
    double radius = (double)len / 2.0;
    if (radius < 1.0) radius = 1.0;

    DrawDotARGB(bits, w, h, cx, cy, radius, a,
                cr, cg, cb, g_cfg.outline ? 1 : 0);
}

static void RedrawCrosshair(void)
{
    if (!g_hwnd) return;

    ComputeOrigin();

    int winW = g_screenW;
    int winH = g_screenH;
    if (winW <= 0 || winH <= 0) return;

    /* --- 准备 32 位 top-down DIB --- */
    static int cachedW = 0, cachedH = 0;
    static void* dibBits = NULL;

    if (!g_memDC || cachedW != winW || cachedH != winH) {
        if (g_memBmp) { DeleteObject(g_memBmp); g_memBmp = NULL; }

        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = winW;
        bi.bmiHeader.biHeight      = -winH;   /* 负数 = top-down，行序从上到下 */
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        HDC screenDC = GetDC(NULL);
        if (!g_memDC) g_memDC = CreateCompatibleDC(screenDC);
        g_memBmp = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &dibBits, NULL, 0);
        ReleaseDC(NULL, screenDC);

        if (!g_memBmp || !dibBits) return;
        SelectObject(g_memDC, g_memBmp);
        cachedW = winW;
        cachedH = winH;
    }
    if (!dibBits) return;

    /* --- 1. 计算准星中心 --- */
    int cx, cy;
    if (g_cfg.monitor == 0) {
        cx = winW / 2;
        cy = winH / 2;
    } else {
        cx = g_originX - GetSystemMetrics(SM_XVIRTUALSCREEN);
        cy = g_originY - GetSystemMetrics(SM_YVIRTUALSCREEN);
    }

    /* --- 2. 渲染准星到 DIB（清屏 + 画圆点都在里面）--- */
    RenderCrosshairToBuffer(dibBits, winW, winH, cx, cy);

    /* --- 3. 一次性提交到分层窗口 --- */
    HDC screenDC = GetDC(NULL);

    POINT ptSrc = { 0, 0 };
    POINT ptDst = { GetSystemMetrics(SM_XVIRTUALSCREEN),
                    GetSystemMetrics(SM_YVIRTUALSCREEN) };
    SIZE  sz    = { winW, winH };
    BLENDFUNCTION bf;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = 255;   /* 每像素 alpha 已经带好，这里取满 */
    bf.AlphaFormat         = AC_SRC_ALPHA;

    /* 先把窗口放到目标位置和尺寸（不激活、不抢焦点）。
     * SWP_NOREDRAW：不要在此时擦除，内容马上由 UpdateLayeredWindow 提交，
     * 否则中间会出现一帧空白 → 视觉上就是"闪一下"。 */
    SetWindowPos(g_hwnd, HWND_TOPMOST,
                 ptDst.x, ptDst.y, winW, winH,
                 SWP_NOACTIVATE | SWP_NOREDRAW | SWP_SHOWWINDOW);

    UpdateLayeredWindow(g_hwnd, screenDC, &ptDst, &sz,
                        g_memDC, &ptSrc, 0, &bf, ULW_ALPHA);

    ReleaseDC(NULL, screenDC);
}

/* ------------------------------------------------------------------ */
/* 窗口样式：置顶 + 分层 + 鼠标穿透 + 透明键                            */
/* ------------------------------------------------------------------ */

static void ApplyWindowStyles(HWND hwnd)
{
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex |= WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);

    /* 注意：这里不能用 SetLayeredWindowAttributes —— 
     * 我们走的是 UpdateLayeredWindow（每像素 alpha）路线，
     * 两者对同一个窗口互斥。内容与透明度全部由 RedrawCrosshair 提交。 */

    /* 只更新 z-order（保持置顶），不要 SWP_FRAMECHANGED：
     * 那个标志会强制 DWM 丢弃分层窗口已提交的内容，
     * 一旦在 UpdateLayeredWindow 之后执行，准星就会消失。 */
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                 SWP_NOREDRAW | SWP_NOOWNERZORDER);
}

/* ------------------------------------------------------------------ */
/* 托盘图标                                                            */
/* ------------------------------------------------------------------ */

static HICON CreateAppIcon(void)
{
    /* 生成一个简单的红准星图标（32x32） */
    int SZ = 32;
    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, SZ, SZ);
    HBITMAP mask  = CreateBitmap(SZ, SZ, 1, 1, NULL);
    SelectObject(dc, color);

    /* 背景填品红（会被当作透明键） */
    RECT r = {0, 0, SZ, SZ};
    HBRUSH bg = CreateSolidBrush(TRANSPARENT_KEY);
    FillRect(dc, &r, bg);
    DeleteObject(bg);

    HBRUSH red = CreateSolidBrush(RGB(230, 30, 30));
    SetRect(&r, SZ/2 - 2, 4, SZ/2 + 2, SZ - 4);
    FillRect(dc, &r, red);
    SetRect(&r, 4, SZ/2 - 2, SZ - 4, SZ/2 + 2);
    FillRect(dc, &r, red);
    DeleteObject(red);

    SelectObject(dc, mask);
    SelectObject(dc, color);
    DeleteDC(dc);
    ReleaseDC(NULL, screen);

    ICONINFO ii;
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask  = mask;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);
    return hIcon;
}

static void ShowTrayMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE,
                g_visible ? L"隐藏准星\tCtrl+Alt+H" : L"显示准星\tCtrl+Alt+H");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_BIGGER,  L"变大\tCtrl+Alt+↑");
    AppendMenuW(menu, MF_STRING, IDM_SMALLER, L"变小\tCtrl+Alt+↓");
    AppendMenuW(menu, MF_STRING, IDM_HUE,     L"换个颜色\tCtrl+Alt+C");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_RELOAD, L"重新加载配置\tCtrl+Alt+I");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_INI, L"打开配置文件");
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"关于");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出\tCtrl+Alt+Q");

    /* 让菜单正确响应（需要窗口在前台） */
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

static void AddTrayIcon(HWND hwnd)
{
    HICON icon = CreateAppIcon();

    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd;
    nid.uID    = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon  = icon;
    wcsncpy(nid.szTip, APP_TITLE L" - 右键菜单", 127);

    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void RemoveTrayIcon(HWND hwnd)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd;
    nid.uID    = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

/* ------------------------------------------------------------------ */
/* 动作                                                                */
/* ------------------------------------------------------------------ */

static void ToggleVisible(void)
{
    g_visible = !g_visible;
    if (g_visible) {
        /* 先确保窗口在目标位置并更新内容，再显示（顺序很关键：
         * UpdateLayeredWindow 负责内容，ShowWindow 只负责可见性） */
        RedrawCrosshair();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(g_hwnd, SW_HIDE);
    }
    LogLine(L"准星 %s", g_visible ? L"已显示" : L"已隐藏");
}

static void ReloadConfig(void)
{
    LoadConfig();
    /* 顺序：改窗口样式 -> 重绘内容 -> 恢复可见状态。
     * 注意不能先 ShowWindow 再 RedrawCrosshair，否则会闪烁或内容丢失。 */
    ApplyWindowStyles(g_hwnd);
    RedrawCrosshair();
    ShowWindow(g_hwnd, g_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    LogLine(L"配置已重载: size=%d thickness=%d gap=%d color=(%d,%d,%d) opacity=%d centerDot=%d dotSize=%d outline=%d monitor=%d",
            g_cfg.size, g_cfg.thickness, g_cfg.gap,
            g_cfg.colorR, g_cfg.colorG, g_cfg.colorB,
            g_cfg.opacity, g_cfg.centerDot, g_cfg.dotSize,
            g_cfg.outline, g_cfg.monitor);
}

/* 热键：调红点大小（±2 px），并立即写回 ini */
static void AdjustSizeBy(int delta)
{
    g_cfg.size += delta;
    if (g_cfg.size < 2)   g_cfg.size = 2;
    if (g_cfg.size > 400) g_cfg.size = 400;

    wchar_t ini[MAX_PATH];
    GetIniPath(ini, MAX_PATH);

    char val[16];
    snprintf(val, sizeof(val), "%d", g_cfg.size);
    const char* keys[1] = { "size" };
    const char* vals[1] = { val };
    WriteIniValues(ini, keys, vals, 1);

    RedrawCrosshair();
    LogLine(L"热键调大小: size=%d (直径, 已写回 ini)", g_cfg.size);
}

/* 热键：循环换色，并立即写回 ini */
static void CycleColor(void)
{
    int idx = (NearestPaletteIndex() + 1) % PALETTE_COUNT;
    g_cfg.colorR = kPalette[idx][0];
    g_cfg.colorG = kPalette[idx][1];
    g_cfg.colorB = kPalette[idx][2];

    wchar_t ini[MAX_PATH];
    GetIniPath(ini, MAX_PATH);

    char vr[16], vg[16], vb[16];
    snprintf(vr, sizeof(vr), "%d", g_cfg.colorR);
    snprintf(vg, sizeof(vg), "%d", g_cfg.colorG);
    snprintf(vb, sizeof(vb), "%d", g_cfg.colorB);

    const char* keys[3] = { "colorR", "colorG", "colorB" };
    const char* vals[3] = { vr, vg, vb };
    WriteIniValues(ini, keys, vals, 3);

    RedrawCrosshair();
    LogLine(L"热键换色: color=(%d,%d,%d) (已写回 ini)",
            g_cfg.colorR, g_cfg.colorG, g_cfg.colorB);
}

static void OpenIniFile(void)
{
    wchar_t ini[MAX_PATH];
    GetIniPath(ini, MAX_PATH);
    /* 确保文件存在 */
    LoadConfig();
    ShellExecuteW(NULL, L"open", L"notepad.exe", ini, NULL, SW_SHOWNORMAL);
}

static void ShowAbout(HWND hwnd)
{
    MessageBoxW(hwnd,
        L"屏幕准星 v1.1（方案 D：纯 C + Win32）\n\n"
        L"热键：\n"
        L"  Ctrl+Alt+H     显示 / 隐藏准星\n"
        L"  Ctrl+Alt+↑     变大（直径 +2 px）\n"
        L"  Ctrl+Alt+↓     变小（直径 -2 px）\n"
        L"  Ctrl+Alt+C     换个颜色\n"
        L"  Ctrl+Alt+I     重新加载配置\n"
        L"  Ctrl+Alt+Q     退出\n\n"
        L"注：用 ↑/↓/C 调的参数会立即写回 crosshair.ini。\n\n"
        L"注意：仅支持窗口化 / 无边框窗口模式的游戏。\n"
        L"独占全屏模式下准星不可见，这是 Windows 架构限制。",
        APP_TITLE, MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
}

/* ------------------------------------------------------------------ */
/* 热键注册：逐个检查结果并记日志                                      */
/* ------------------------------------------------------------------ */

/* 把热键注册结果写日志：成功了记 ok，失败了记下 GetLastError
 * name 用窄字符常量（纯 ASCII），经 %S 提升为宽字符，避免宽窄混用出错 */
static void RegisterOneHotKey(HWND hwnd, int id, UINT mods, UINT vk, const char* name)
{
    if (RegisterHotKey(hwnd, id, mods | MOD_NOREPEAT, vk)) {
        LogLine(L"热键注册成功: %S  (id=%d)", name, id);
    } else {
        DWORD err = GetLastError();
        LogLine(L"热键注册失败: %S  (id=%d, 错误码=%lu) —— 已被其他程序占用",
                name, id, (unsigned long)err);
    }
}

/* ------------------------------------------------------------------ */
/* 窗口过程                                                            */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
        } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            ToggleVisible();
        }
        return 0;

    case WM_HOTKEY:
        LogLine(L"收到热键消息 id=%d", (int)wParam);
        if (wParam == HOTKEY_TOGGLE)       ToggleVisible();
        else if (wParam == HOTKEY_BIGGER)  AdjustSizeBy(+2);
        else if (wParam == HOTKEY_SMALLER) AdjustSizeBy(-2);
        else if (wParam == HOTKEY_HUE)     CycleColor();
        else if (wParam == HOTKEY_RELOAD)  ReloadConfig();
        else if (wParam == HOTKEY_EXIT)    PostMessage(hwnd, WM_CLOSE, 0, 0);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_TOGGLE:
            LogLine(L"菜单: 切换显示");
            ToggleVisible();
            break;
        case IDM_BIGGER:  AdjustSizeBy(+2); break;
        case IDM_SMALLER: AdjustSizeBy(-2); break;
        case IDM_HUE:     CycleColor();     break;
        case IDM_RELOAD:
            LogLine(L"菜单: 重载配置");
            ReloadConfig();
            break;
        case IDM_OPEN_INI: OpenIniFile();  break;
        case IDM_ABOUT:  ShowAbout(hwnd);  break;
        case IDM_EXIT:   PostMessage(hwnd, WM_CLOSE, 0, 0); break;
        }
        return 0;

    case WM_DISPLAYCHANGE:
        /* 分辨率 / 显示器变化，重新计算并重绘 */
        RedrawCrosshair();
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon(hwnd);
        UnregisterHotKey(hwnd, HOTKEY_TOGGLE);
        UnregisterHotKey(hwnd, HOTKEY_BIGGER);
        UnregisterHotKey(hwnd, HOTKEY_SMALLER);
        UnregisterHotKey(hwnd, HOTKEY_HUE);
        UnregisterHotKey(hwnd, HOTKEY_RELOAD);
        UnregisterHotKey(hwnd, HOTKEY_EXIT);
        if (g_memBmp) DeleteObject(g_memBmp);
        if (g_memDC)  DeleteDC(g_memDC);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* 自检模式（--selftest）：不创建任何窗口、不抢焦点，                  */
/* 纯内存渲染准星 -> 导出 PNG + 文字报告。                             */
/* 用于在无人观看的情况下用数据证明"准星画对了"。                      */
/* ------------------------------------------------------------------ */

/* 极简 PNG 编码器（32 位 RGBA，无压缩存储 = zlib stored 块） */
static void WriteBe32(unsigned char* p, unsigned int v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >>  8);
    p[3] = (unsigned char)(v);
}

/* --- CRC32（PNG 各 chunk 必须带正确的 CRC，否则解码器拒绝加载） --- */
static unsigned int Crc32(const unsigned char* data, size_t len, unsigned int crc)
{
    static unsigned int table[256];
    static int inited = 0;
    if (!inited) {
        for (unsigned int i = 0; i < 256; ++i) {
            unsigned int c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        inited = 1;
    }
    crc = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* 写一个 PNG chunk：长度 + 类型 + 数据 + CRC */
static void WriteChunk(FILE* fp, const char* type,
                       const unsigned char* data, size_t len)
{
    unsigned char hdr[8];
    WriteBe32(hdr, (unsigned int)len);
    fwrite(hdr, 1, 4, fp);
    fwrite(type, 1, 4, fp);
    if (len) fwrite(data, 1, len, fp);

    /* CRC 覆盖「类型 + 数据」 */
    unsigned char* tmp = (unsigned char*)malloc(4 + len);
    if (tmp) {
        memcpy(tmp, type, 4);
        if (len) memcpy(tmp + 4, data, len);
        unsigned int crc = Crc32(tmp, 4 + len, 0);
        unsigned char cb[4];
        WriteBe32(cb, crc);
        fwrite(cb, 1, 4, fp);
        free(tmp);
    } else {
        unsigned char zero[4] = {0,0,0,0};
        fwrite(zero, 1, 4, fp);
    }
}

static int SavePngRGBA(const wchar_t* path, const unsigned char* bgra,
                       int w, int h)
{
    /* --- 把 BGRA 转成 PNG 需要的 RGBA --- */
    size_t rawSize = (size_t)h * (1 + (size_t)w * 4);
    unsigned char* raw = (unsigned char*)malloc(rawSize);
    if (!raw) return 0;

    for (int y = 0; y < h; ++y) {
        unsigned char* dst = raw + (size_t)y * (1 + (size_t)w * 4);
        dst[0] = 0;   /* filter type: none */
        const unsigned char* src = bgra + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[1 + x * 4 + 0] = src[x * 4 + 2];  /* R */
            dst[1 + x * 4 + 1] = src[x * 4 + 1];  /* G */
            dst[1 + x * 4 + 2] = src[x * 4 + 0];  /* B */
            dst[1 + x * 4 + 3] = src[x * 4 + 3];  /* A */
        }
    }

    /* --- zlib 容器：stored（未压缩）块 --- */
    unsigned int adler = 1;
    {
        unsigned int a = 1, b = 0;
        for (size_t i = 0; i < rawSize; ++i) {
            a = (a + raw[i]) % 65521;
            b = (b + a) % 65521;
        }
        adler = (b << 16) | a;
    }

    size_t maxBlocks = (rawSize + 65534) / 65535;
    size_t zSize = 2 + rawSize + maxBlocks * 5 + 4;
    unsigned char* z = (unsigned char*)malloc(zSize);
    if (!z) { free(raw); return 0; }

    size_t zi = 0;
    z[zi++] = 0x78; z[zi++] = 0x01;   /* zlib header */

    size_t off = 0;
    while (off < rawSize) {
        size_t chunk = rawSize - off;
        if (chunk > 65535) chunk = 65535;
        int last = (off + chunk >= rawSize) ? 1 : 0;

        z[zi++] = (unsigned char)last;
        unsigned int len = (unsigned int)chunk;
        unsigned int nlen = (~len) & 0xFFFF;
        z[zi++] = (unsigned char)(len & 0xFF);
        z[zi++] = (unsigned char)((len >> 8) & 0xFF);
        z[zi++] = (unsigned char)(nlen & 0xFF);
        z[zi++] = (unsigned char)((nlen >> 8) & 0xFF);
        memcpy(z + zi, raw + off, chunk);
        zi += chunk;
        off += chunk;
    }
    WriteBe32(z + zi, adler); zi += 4;

    /* --- PNG 结构 --- */
    FILE* fp = _wfopen(path, L"wb");
    if (!fp) { free(raw); free(z); return 0; }

    static const unsigned char SIG[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    fwrite(SIG, 1, 8, fp);

    /* IHDR */
    unsigned char ihdr[13];
    WriteBe32(ihdr + 0, (unsigned int)w);
    WriteBe32(ihdr + 4, (unsigned int)h);
    ihdr[8]  = 8;   /* bit depth */
    ihdr[9]  = 6;   /* color type: RGBA */
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    WriteChunk(fp, "IHDR", ihdr, 13);

    /* IDAT */
    WriteChunk(fp, "IDAT", z, zi);

    /* IEND */
    WriteChunk(fp, "IEND", NULL, 0);

    fclose(fp);
    free(raw);
    free(z);
    return 1;
}

/* 自检主流程：渲染 + 统计 + 导出 */
static int RunSelfTest(void)
{
    LoadConfig();

    /* 只渲染中心 320x320 区域，足够看清单个圆点且体积小 */
    const int CW = 320, CH = 320;
    unsigned char* buf = (unsigned char*)malloc((size_t)CW * CH * 4);
    if (!buf) return 1;

    RenderCrosshairToBuffer(buf, CW, CH, CW / 2, CH / 2);

    /* --- 统计 --- */
    int redPixels = 0, partialPixels = 0;
    int minX = CW, maxX = -1, minY = CH, maxY = -1;
    for (int y = 0; y < CH; ++y) {
        const unsigned char* row = buf + (size_t)y * CW * 4;
        for (int x = 0; x < CW; ++x) {
            unsigned char b = row[x * 4 + 0];
            unsigned char g = row[x * 4 + 1];
            unsigned char r = row[x * 4 + 2];
            unsigned char a = row[x * 4 + 3];
            if (a == 0) continue;
            if (a < 255) partialPixels++;
            if (r > 100 && r > g + 40 && r > b + 40) redPixels++;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }

    wchar_t ini[MAX_PATH];
    GetIniPath(ini, MAX_PATH);
    wchar_t pngPath[MAX_PATH], txtPath[MAX_PATH];
    _snwprintf(pngPath, MAX_PATH, L"%s.selftest.png", ini); pngPath[MAX_PATH-1] = 0;
    _snwprintf(txtPath, MAX_PATH, L"%s.selftest.txt", ini); txtPath[MAX_PATH-1] = 0;

    int ok = SavePngRGBA(pngPath, buf, CW, CH);

    FILE* fp = _wfopen(txtPath, L"wb");
    if (fp) {
        char line[512];
        int n = snprintf(line, sizeof(line),
            "=== 准星自检报告 ===\n"
            "画面尺寸: %d x %d\n"
            "圆心坐标: (%d, %d)\n"
            "配置: size=%d(直径) color=(%d,%d,%d) opacity=%d outline=%d\n"
            "\n"
            "--- 渲染结果 ---\n"
            "非透明像素: %d\n"
            "纯红像素(不透明核心): %d\n"
            "半透明像素(抗锯齿边缘): %d\n"
            "图形包围盒: x[%d..%d] y[%d..%d]  宽=%d 高=%d\n"
            "\n"
            "--- 判定 ---\n",
            CW, CH, CW / 2, CH / 2,
            g_cfg.size,
            g_cfg.colorR, g_cfg.colorG, g_cfg.colorB,
            g_cfg.opacity, g_cfg.outline,
            redPixels + partialPixels, redPixels, partialPixels,
            minX, maxX, minY, maxY, maxX - minX + 1, maxY - minY + 1);
        fwrite(line, 1, n, fp);

        /* 逐项检查 */
        int bboxW = maxX - minX + 1;
        int bboxH = maxY - minY + 1;
        int expect = g_cfg.size + (g_cfg.outline ? 2 : 0);   /* 直径 + 描边 */

        /* 逐项检查辅助宏（printf 到日志行并写文件） */
        #define PUT(...) do { \
            n = snprintf(line, sizeof(line), __VA_ARGS__); \
            fwrite(line, 1, n, fp); fwrite("\n", 1, 1, fp); \
        } while (0)

        if (redPixels > 0) PUT("  [OK] 确实画出了红色像素 (%d 个)", redPixels);
        else               PUT("  [!!] 没有红色像素，绘制失败");

        if (minX >= 0 && bboxW <= expect + 4 && bboxH <= expect + 4)
            PUT("  [OK] 包围盒 %dx%d 与设定直径 %d 相符（大小正确）", bboxW, bboxH, g_cfg.size);
        else
            PUT("  [!!] 包围盒 %dx%d 与设定直径 %d 不符", bboxW, bboxH, g_cfg.size);

        if (bboxW == bboxH) PUT("  [OK] 宽高相等 (%d=%d) = 圆形，不是方形", bboxW, bboxH);
        else                PUT("  [!!] 宽高不等 (%d vs %d)，可能不是圆", bboxW, bboxH);

        if (partialPixels > 0) PUT("  [OK] 有 %d 个半透明边缘像素 = 抗锯齿生效", partialPixels);
        else                   PUT("  [??] 无半透明像素，边缘可能有锯齿");

        #undef PUT

        n = snprintf(line, sizeof(line), "\nPNG 已导出: %s\n", ok ? "(见同目录 .selftest.png)" : "(失败)");
        fwrite(line, 1, n, fp);
        fclose(fp);
    }

    free(buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmdLine, int nShow)
{
    (void)hPrev; (void)nShow;

    /* --selftest：不建窗口，纯内存渲染并导出报告（用于自动化验证） */
    if (cmdLine && wcsstr(cmdLine, L"--selftest")) {
        return RunSelfTest();
    }

    /* 关键：声明 DPI 感知，保证拿到物理像素。
     * 用动态加载的方式调用，避免依赖特定 SDK 版本的头文件与导入库。 */
    {
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32) {
            FARPROC fn = GetProcAddress(hUser32, "SetProcessDPIAware");
            if (fn) {
                typedef BOOL (WINAPI *PFN_DPIAWARE)(void);
                ((PFN_DPIAWARE)(void*)fn)();
            }
        }
    }

    /* 单实例：已有实例在跑就直接退出，避免重复堆叠准星 */
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Global\\CrosshairOverlayMutex_v1");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"屏幕准星已经在运行了。\n请在托盘区找到它的图标。",
                    APP_TITLE, MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        return 0;
    }

    LoadConfig();

    /* 注册窗口类 */
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = APP_CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;   /* 背景完全由我们自己在内存 DC 里画 */

    if (!RegisterClassExW(&wc)) return 1;

    /* 创建窗口：覆盖主显示器，无边框无标题 */
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        APP_CLASS_NAME, APP_TITLE,
        WS_POPUP,
        0, 0, w, h,
        NULL, NULL, hInst, NULL);

    if (!g_hwnd) return 1;

    ApplyWindowStyles(g_hwnd);

    /* 首次绘制 */
    RedrawCrosshair();

    /* 显示（不抢焦点） */
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hwnd);

    /* 托盘 + 全局热键（逐个注册并记录结果，失败会写进日志） */
    AddTrayIcon(g_hwnd);
    RegisterOneHotKey(g_hwnd, HOTKEY_TOGGLE,  MOD_CONTROL | MOD_ALT, 'H',     "Ctrl+Alt+H show/hide");
    RegisterOneHotKey(g_hwnd, HOTKEY_BIGGER,  MOD_CONTROL | MOD_ALT, VK_UP,   "Ctrl+Alt+Up bigger");
    RegisterOneHotKey(g_hwnd, HOTKEY_SMALLER, MOD_CONTROL | MOD_ALT, VK_DOWN, "Ctrl+Alt+Down smaller");
    RegisterOneHotKey(g_hwnd, HOTKEY_HUE,     MOD_CONTROL | MOD_ALT, 'C',     "Ctrl+Alt+C cycle color");
    RegisterOneHotKey(g_hwnd, HOTKEY_RELOAD,  MOD_CONTROL | MOD_ALT, 'I',     "Ctrl+Alt+I reload config");
    RegisterOneHotKey(g_hwnd, HOTKEY_EXIT,    MOD_CONTROL | MOD_ALT, 'Q',     "Ctrl+Alt+Q quit");

    LogLine(L"程序启动完成，已显示准星。");
    {
        wchar_t ini[MAX_PATH];
        GetIniPath(ini, MAX_PATH);
        LogLine(L"配置文件路径: %s", ini);
        LogLine(L"当前配置: size=%d thickness=%d gap=%d color=(%d,%d,%d) opacity=%d centerDot=%d dotSize=%d outline=%d monitor=%d",
                g_cfg.size, g_cfg.thickness, g_cfg.gap,
                g_cfg.colorR, g_cfg.colorG, g_cfg.colorB,
                g_cfg.opacity, g_cfg.centerDot, g_cfg.dotSize,
                g_cfg.outline, g_cfg.monitor);
    }

    /* 消息循环 */
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (mutex) CloseHandle(mutex);
    return 0;
}
