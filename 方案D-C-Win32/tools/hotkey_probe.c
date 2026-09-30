/*
 * hotkey_probe.c - 探测哪些 Ctrl+Alt+<字母> 组合当前空闲
 *
 * 原理：RegisterHotKey 对一个已被他进程注册的组合会返回失败
 *       （GetLastError = 1409, ERROR_HOTKEY_ALREADY_REGISTERED）。
 *       本程序在一个隐藏窗口上挨个尝试注册 A-Z、0-9、F1-F12，
 *       注册成功的立刻注销，把结果打印到控制台。
 *
 * 编译（MinGW）：
 *   g++ -O2 -municode -o hotkey_probe.exe hotkey_probe.c -luser32
 * 注意：本程序是控制台程序，运行时会弹一个黑窗口，几秒后自动关闭。
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static const DWORD MODS = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;

static void ProbeKey(int id, UINT vk, const wchar_t* label)
{
    if (RegisterHotKey(NULL, id, MODS, vk)) {
        UnregisterHotKey(NULL, id);
        wprintf(L"FREE  Ctrl+Alt+%s\n", label);
    } else {
        DWORD err = GetLastError();
        if (err == 1409) {
            wprintf(L"USED  Ctrl+Alt+%s   <== already taken\n", label);
        } else {
            wprintf(L"ERR%lu Ctrl+Alt+%s\n", (unsigned long)err, label);
        }
    }
}

int wmain(void)
{
    SetConsoleOutputCP(65001);

    wprintf(L"==== Ctrl+Alt+<key> availability ====\n\n");

    wprintf(L"-- letters A-Z --\n");
    int id = 2000;
    for (wchar_t c = L'A'; c <= L'Z'; ++c) {
        wchar_t lbl[2] = { c, 0 };
        ProbeKey(id++, (UINT)c, lbl);
    }

    wprintf(L"\n-- digits 0-9 --\n");
    for (wchar_t c = L'0'; c <= L'9'; ++c) {
        wchar_t lbl[2] = { c, 0 };
        ProbeKey(id++, (UINT)c, lbl);
    }

    wprintf(L"\n-- function keys F1-F12 --\n");
    for (int i = 0; i < 12; ++i) {
        wchar_t lbl[8];
        _snwprintf(lbl, 8, L"F%d", i + 1);
        ProbeKey(id++, VK_F1 + i, lbl);
    }

    wprintf(L"\nDone.\n");
    return 0;
}
