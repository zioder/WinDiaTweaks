@echo off
rem Builds bin\DiaTweaksTap.dll (the tweak engine) and bin\diatweaks.exe (the command).
rem Needs Visual Studio 2022 (or Build Tools) with the C++ workload and a Windows 10/11 SDK.
setlocal
cd /d "%~dp0"
rem Version shown in Settings > Patches > Updates: the release tag in CI (WDT_VERSION), else what git
rem says about this checkout (e.g. v1.2.0-3-gabc123), else "dev". Read before PATH is narrowed below.
set "VER=%WDT_VERSION%"
if not defined VER for /f "delims=" %%v in ('git describe --tags --always --dirty 2^>nul') do set "VER=%%v"
if not defined VER set "VER=dev"
where cl >nul 2>nul && goto build

rem Find the C++ toolset and Windows SDK directly. (vcvars64.bat can fail on machines with a very long PATH.)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo Visual Studio C++ build tools not found & exit /b 1)
set /p VCVER=<"%VS%\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"
set "VC=%VS%\VC\Tools\MSVC\%VCVER: =%"
set "SDK=%ProgramFiles(x86)%\Windows Kits\10"
for /f "delims=" %%v in ('dir /b /ad /o-n "%SDK%\Include\10.*" 2^>nul') do if not defined SV set "SV=%%v"
if not defined SV (echo Windows SDK not found & exit /b 1)
set "PATH=%VC%\bin\Hostx64\x64;%SDK%\bin\%SV%\x64;%SystemRoot%\System32;%SystemRoot%"
set "INCLUDE=%VC%\include;%SDK%\Include\%SV%\ucrt;%SDK%\Include\%SV%\um;%SDK%\Include\%SV%\shared;%SDK%\Include\%SV%\winrt"
set "LIB=%VC%\lib\x64;%SDK%\Lib\%SV%\ucrt\x64;%SDK%\Lib\%SV%\um\x64"

:build
if not exist bin mkdir bin
> bin\version.h echo #define WDT_VERSION L"%VER%"
set FLAGS=/nologo /utf-8 /std:c++20 /O2 /EHsc /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /MT
if /i "%~1"=="test-media" (
    cl %FLAGS% tests\media_state_test.cpp /Fobin\ /Febin\media-state-test.exe || exit /b 1
    bin\media-state-test.exe
    exit /b
)
rem C++/WinRT headers for Dia's WinUI live in gen\ (made from Dia's own .winmd files by gen-winrt.cmd).
if not exist gen\winrt\Microsoft.UI.Xaml.Controls.h call "%~dp0gen-winrt.cmd" || exit /b 1
for /f "delims=" %%v in ('dir /b /ad /o-n "%ProgramFiles(x86)%\Windows Kits\10\Include\10.*" 2^>nul') do if not defined SDKV set "SDKV=%%v"
cl %FLAGS% /bigobj /Zi /I gen /I "%ProgramFiles(x86)%\Windows Kits\10\Include\%SDKV%\cppwinrt" /LD src\tap.cpp src\ui.cpp src\peek.cpp src\history.cpp src\media.cpp /Fobin\ /Febin\DiaTweaksTap.dll /link /DEBUG /OPT:REF /DEF:src\tap.def ole32.lib oleaut32.lib user32.lib gdi32.lib shell32.lib windowsapp.lib dwmapi.lib comctl32.lib msimg32.lib d3d11.lib dcomp.lib d2d1.lib || exit /b 1
cl %FLAGS% /I bin /I "%ProgramFiles(x86)%\Windows Kits\10\Include\%SDKV%\cppwinrt" src\diatweaks.cpp /Fobin\ /Febin\diatweaks.exe /link ole32.lib user32.lib shell32.lib windowsapp.lib || exit /b 1
rem DiaLauncher.exe: the same program without a console window, for the "Dia (Tweaks)" shortcut.
cl %FLAGS% /DWDT_LAUNCHER /I bin /I "%ProgramFiles(x86)%\Windows Kits\10\Include\%SDKV%\cppwinrt" src\diatweaks.cpp /Fobin\DiaLauncher.obj /Febin\DiaLauncher.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup ole32.lib user32.lib shell32.lib windowsapp.lib || exit /b 1
rem bin\tweaks.conf also holds your on/off choices: bring in new and changed tweaks, keep your choices.
if exist bin\tweaks.conf (bin\diatweaks.exe merge-conf tweaks.conf bin\tweaks.conf >nul || exit /b 1) else copy /y tweaks.conf bin\tweaks.conf >nul
echo Built bin\diatweaks.exe, bin\DiaLauncher.exe and bin\DiaTweaksTap.dll (%VER%)
