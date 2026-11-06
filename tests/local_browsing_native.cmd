@echo off
setlocal
where cl >nul 2>&1
if not errorlevel 1 goto build
set "native_vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%native_vswhere%" (
    for /f "delims=" %%I in ('where vswhere 2^>nul') do set "native_vswhere=%%I"
)
if not exist "%native_vswhere%" (
    echo Error: MSVC is not on PATH and vswhere was not found. Install Visual Studio C++ build tools.
    exit /b 1
)
set "native_vs_root="
for /f "usebackq delims=" %%I in (`"%native_vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "native_vs_root=%%I"
if not defined native_vs_root (
    echo Error: No Visual Studio installation with the MSVC x86/x64 tools was found.
    exit /b 1
)
if not exist "%native_vs_root%\VC\Auxiliary\Build\vcvars64.bat" (
    echo Error: The selected Visual Studio installation has no x64 build environment.
    exit /b 1
)
call "%native_vs_root%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo Error: Could not initialize the Visual Studio x64 build environment.
    exit /b 1
)
:build
pushd "%~dp0.."
if not exist bin mkdir bin
cl /nologo /std:c++17 /EHsc /W4 /Zi /Fe:bin\local-browsing-native-test.exe /Fo:bin\ tests\local_browsing_native.cpp src\dia_native_layout.cpp /link bcrypt.lib
if errorlevel 1 (popd & exit /b 1)
bin\local-browsing-native-test.exe
set "test_result=%errorlevel%"
popd
exit /b %test_result%
