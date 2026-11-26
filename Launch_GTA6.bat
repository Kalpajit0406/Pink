@echo off
setlocal
cd /d "%~dp0"

echo [Leonida] Launching GTA VI Unified Launcher (Native Windows App)...

if exist "tools\launcher_app\bin\Release\net8.0-windows\LeonidaLauncher.exe" (
    start "" "tools\launcher_app\bin\Release\net8.0-windows\LeonidaLauncher.exe"
    exit /b 0
)

if exist "E:\Leonida\Launcher\LeonidaLauncher.exe" (
    start "" "E:\Leonida\Launcher\LeonidaLauncher.exe"
    exit /b 0
)

if exist "E:\Leonida\LeonidaLauncher.exe" (
    start "" "E:\Leonida\LeonidaLauncher.exe"
    exit /b 0
)

echo [Leonida] Building launcher executable...
dotnet build tools\launcher_app\LeonidaLauncher.csproj -c Release
if exist "tools\launcher_app\bin\Release\net8.0-windows\LeonidaLauncher.exe" (
    start "" "tools\launcher_app\bin\Release\net8.0-windows\LeonidaLauncher.exe"
    exit /b 0
)

echo [Error] Failed to locate or build LeonidaLauncher.exe.
pause
exit /b 1
