@echo off
setlocal

REM ============================================================
REM  Crosshair Overlay  --  Scheme A / C#
REM  One click build. Output: dist\Crosshair.exe
REM ============================================================

set DOTNET="C:\Program Files\dotnet\dotnet.exe"
if not exist %DOTNET% (
    echo [ERROR] dotnet.exe not found. Please install .NET SDK first.
    pause
    exit /b 1
)

cd /d "%~dp0src"

echo.
echo Building ...
%DOTNET% publish Crosshair.csproj -c Release -o "..\dist"
if errorlevel 1 (
    echo.
    echo [FAILED] Build error. Please send the messages above to the author.
    pause
    exit /b 1
)

if exist "..\dist\Crosshair.pdb" del /q "..\dist\Crosshair.pdb"

echo.
echo ============================================================
echo  Build OK.  Output: dist\Crosshair.exe
echo  Double-click it to run. config.txt is created on first run.
echo ============================================================
echo.
pause
