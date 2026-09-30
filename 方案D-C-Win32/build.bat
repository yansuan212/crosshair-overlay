@echo off
REM ============================================================
REM  Screen Crosshair (red dot) - build script
REM  Plan D: pure C + Win32. Needs MinGW-w64 (g++).
REM  Local compiler path: D:////mingw64////bin
REM ============================================================

setlocal

REM ---- compiler path (change this line if MinGW is elsewhere) ----
set GXX=D:////mingw64////bin////g++.exe

cd /d "%~dp0"

echo [1/2] Building crosshair.exe ...
"%GXX%" -O2 -s -Wall -Wextra -municode -mwindows -o crosshair.exe src\crosshair.cpp -lgdi32 -luser32 -lshell32 -lole32

if errorlevel 1 (
    echo.
    echo [FAILED] Compile error, check messages above.
    echo          If g++ is not found, edit the GXX path in this script.
    pause
    exit /b 1
)

echo [2/2] Building tools...
"%GXX%" -O2 -s -municode -o tools\hotkey_probe.exe tools\hotkey_probe.c -luser32

echo.
echo Build OK.
for %%F in (crosshair.exe) do echo       output: %%~fF  (%%~zF bytes)
echo.
echo Double-click crosshair.exe to run: shows a red dot at screen center.
echo Hotkeys: Ctrl+Alt+H show/hide, Ctrl+Alt+I reload config, Ctrl+Alt+Q quit.
echo Self-test: crosshair.exe --selftest  (exports a PNG + a text report)
echo.

endlocal
