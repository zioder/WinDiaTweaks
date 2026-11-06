@echo off
setlocal
where cl >nul 2>&1
if not errorlevel 1 goto build
set "native_vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%native_vswhere%" for /f "delims=" %%I in ('where vswhere.exe 2^>nul') do set "native_vswhere=%%I"
if not exist "%native_vswhere%" (
    echo Error: cl.exe is not on PATH and Visual Studio Installer vswhere.exe was not found. 1>&2
    exit /b 1
)
for /f "usebackq delims=" %%I in (`"%native_vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "native_vs_root=%%I"
if not defined native_vs_root (
    echo Error: no Visual Studio installation with the C++ x64 build tools was found. 1>&2
    exit /b 1
)
if not exist "%native_vs_root%\VC\Auxiliary\Build\vcvars64.bat" (
    echo Error: the selected Visual Studio installation has no vcvars64.bat. 1>&2
    exit /b 1
)
call "%native_vs_root%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
:build
pushd "%~dp0.."
if not exist bin mkdir bin
cl /nologo /std:c++17 /O2 /EHsc /W4 /Fe:bin\dia-native-layout-test.exe /Fo:bin\ src\dia_native_layout.cpp tests\dia_native_layout_native.cpp
if errorlevel 1 (popd & exit /b 1)
bin\dia-native-layout-test.exe %*
set "test_result=%errorlevel%"
popd
exit /b %test_result%
