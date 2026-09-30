# 屏幕准星 Crosshair Overlay

Windows 桌面正中央的一个**红色圆点**，覆盖在游戏画面上，用作 FPS 瞄准参考。

> ⚠️ **只提供视觉显示。不注入游戏、不修改游戏文件、不读写游戏内存。**
> 就是一个显示在最顶层的透明窗口，鼠标点击会直接穿透到下面的游戏。

---

## 一、直接下载（推荐）

不想自己编译的话，直接去 [**Releases 页面**](https://github.com/yansuan212/crosshair-overlay/releases/latest) 下载现成的：

| 压缩包 | 说明 |
|---|---|
| `Crosshair-SchemeA-CSharp-v1.0.0.zip` | **方案 A**，exe **18.9 KB**，依赖系统自带的 .NET Framework 4.8 |
| `Crosshair-SchemeD-PureC-v1.0.0.zip` | **方案 D**，exe **55 KB**，**零运行时依赖**，另含热键占用探测工具 |

解压后双击 exe 即可运行，首次启动会在同目录自动生成配置文件。

---

## 二、两个方案，任选其一

本仓库包含两份**完全独立、互不干扰**的实现，功能一致，都是「屏幕中央一个红色圆点」。
选哪个都行，看你更在意什么：

| | **方案 A**（C#） | **方案 D**（纯 C） |
|---|---|---|
| 目录 | `方案A-CSharp-Win32/` | `方案D-C-Win32/` |
| 语言 | C# + 原生 Win32 互操作 | C + Win32，无框架 |
| 编译依赖 | .NET SDK（目标框架 net48） | MinGW-w64 g++ |
| 运行依赖 | .NET Framework 4.8（系统自带） | **无**（仅系统 DLL） |
| exe 体积 | **18.9 KB** | 55 KB |
| 内存更省 | | ✅ 少约 4.6 MB |
| 4K 屏下 | ✅ 不随分辨率增长 | 内存随分辨率线性增长 |
| 构建链 | ✅ 只要 dotnet SDK | 需要 MinGW |

**懒人结论**：屏幕 ≤2560×1600 且想要「绝对最轻、零依赖」→ 方案 D；
会换 4K 屏或更看重改代码方便 → 方案 A。
**只关心会不会拖累游戏帧数 → 两个都一样，空闲 CPU 都是 0.000%。**

详细的实测数据见 [`性能实测报告.md`](性能实测报告.md)。

---

## 三、快速开始

两个方案的用法完全一样：

1. 进入对应方案目录，双击 `crosshair.exe`（方案 A 在 `dist/` 里）
2. 屏幕正中央立刻出现红色圆点
3. 右下角托盘区出现图标，**右键**它是菜单

首次运行会自动生成配置文件。

### 热键

全部为 **Ctrl + Alt + 组合键**：

| 热键 | 功能 |
|---|---|
| `Ctrl + Alt + H` | 显示 / 隐藏红点 |
| `Ctrl + Alt + ↑` | 变大（直径 +2 px） |
| `Ctrl + Alt + ↓` | 变小（直径 −2 px） |
| `Ctrl + Alt + C` | 换个颜色（红→亮红→深红→品红→绿→青→黄→白 循环） |
| `Ctrl + Alt + I` | 重新加载配置文件（改完配置按一下，不用重启） |
| `Ctrl + Alt + Q` | 退出 |

用 `↑` / `↓` / `C` 调出来的大小和颜色会**立即写回配置文件**，重启后仍然保持。

> ⚠️ **为什么重载键是 I 而不是 R**：本机 `Ctrl+Alt+R` 被 NVIDIA 游戏内覆盖占用，
> 注册会静默失败。实测已被占用的组合：`Ctrl+Alt+{ F, L, M, O, R, X, Z }`。
> 方案 D 附带 `tools/hotkey_probe.exe`，换热键前可以先跑它探测哪些键空闲。

### 配置

方案 A 用 `dist/config.txt`，方案 D 用 `crosshair.ini`，都只有两项核心参数：

```ini
# 方案 A
diameter=8       # 圆点直径（像素）
color=255,0,0    # 颜色 RGB
```

```ini
# 方案 D
[crosshair]
size=8           # 圆点直径（像素）
colorR=255
colorG=0
colorB=0
opacity=230      # 整体不透明度
outline=0        # 是否加黑色描边（亮背景下更清晰）
monitor=0        # 显示器序号
```

---

## 四、自己编译

### 方案 A（C#）

需要 .NET SDK，然后双击 `方案A-CSharp-Win32/build.bat`，或在 `src` 目录下：

```bat
dotnet build Crosshair.csproj -c Release -o ..\dist
```

### 方案 D（纯 C）

需要 MinGW-w64 的 g++，然后双击 `方案D-C-Win32/build.bat`，或：

```bat
g++ -O2 -s -municode -mwindows -o crosshair.exe src\crosshair.cpp ^
    -lgdi32 -luser32 -lshell32 -lole32
```

---

## 五、不用眼睛看也能验证画对了

两个方案都内置**自检模式**：不创建窗口、不抢焦点，纯内存渲染后导出图片和报告。

```bat
方案A-CSharp-Win32\dist\Crosshair.exe --selftest
方案D-C-Win32\crosshair.exe --selftest
```

会导出 PNG（中心区域渲染图）和 TXT 报告（圆心坐标、包围盒尺寸、红色像素数、
抗锯齿像素数），并逐项给出 `[OK]` / `[!!]` 判定。

实测基准：2560×1600 屏、直径 8 px → 圆心 (1280, 800)，包围盒 8×8，抗锯齿生效。

---

## 六、技术要点

1. **窗口样式四件套**：`WS_EX_LAYERED`（分层透明）+ `WS_EX_TRANSPARENT`（鼠标穿透）
   + `WS_EX_TOPMOST`（置顶）+ `WS_EX_TOOLWINDOW`（不出现在 Alt+Tab）
2. **性能秘诀 = 不重绘**：没有定时器、没有循环重绘。只在启动、改配置、
   分辨率变化时画一次，之后窗口静止，消息循环阻塞在 `GetMessage` 上。
   实测两个方案的空闲 CPU 都是 **0.000%**。
3. **方案 D 的透明路线**：自建 32 位 ARGB DIB，`memset` 清零 = 全透明，
   只在中心填一个圆，用 `UpdateLayeredWindow` 加 `ULW_ALPHA` 一次性提交。
   （避开了 `LWA_COLORKEY` —— 它和 `LWA_ALPHA` 互斥，且分层窗口下直接 `BitBlt` 不生效）
4. **抗锯齿**：边缘像素按覆盖率做 alpha 混合，所以圆边平滑不毛糙。
5. **DPI 感知**：`SetProcessDPIAware()` + `GetSystemMetrics` 取物理像素，
   4K / 缩放环境下红点仍在正中心。
6. **落盘日志**：WinExe 没有控制台，热键注册结果和配置值都写进日志文件，
   否则「按了没反应」无从查起。

---

## 七、已知限制

- ⚠️ **独占全屏（Exclusive Fullscreen）模式下红点不可见** —— 这是 Windows 架构限制，
  所有同类工具都一样。请把游戏设为**无边框窗口 / 窗口模式**。
- ⚠️ **反作弊**：本程序不注入、不 hook，对 VAC / EAC 这类常规反作弊是安全的；
  但**内核级反作弊**可能检测顶层 overlay 窗口，存在风险，请自行确认所玩游戏的策略。
- ⚠️ 部分游戏开 HDR 或走特殊渲染路径时，overlay 表现可能异常。
- 方案 D 的副屏（`monitor > 0`）代码路径尚未充分验证。

---

## 八、目录结构

```
准星/
├── README.md                  # 本文件
├── 需求方案.md                # 需求梳理与技术选型调研
├── 性能实测报告.md            # 两个方案的实测性能对比
├── 方案A-CSharp-Win32/        # 方案 A：C#
│   ├── src/                   # 源码 + 工程文件 + manifest
│   ├── dist/                  # 编译产物
│   ├── build.bat
│   └── 使用说明.md
└── 方案D-C-Win32/             # 方案 D：纯 C
    ├── src/crosshair.cpp      # 全部源码（单文件）
    ├── tools/                 # 热键占用探测工具
    ├── crosshair.exe
    ├── crosshair.ini
    ├── build.bat
    └── 使用说明.md
```
