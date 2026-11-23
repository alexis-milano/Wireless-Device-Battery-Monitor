@echo off
rem Builds BatteryHub.exe with Microsoft Visual C++ (PR #632). Same sources and libraries as build.bat.
rem Run it from any prompt: it finds Visual Studio (2019 or newer, "Desktop development with C++") itself.
rem Usage: build-msvc.bat [version] [channel]
setlocal enabledelayedexpansion
cd /d "%~dp0"

where cl.exe >nul 2>&1
if %ERRORLEVEL% equ 0 goto COMPILE
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_DIR=%%i"
)
if defined VS_DIR if exist "!VS_DIR!\VC\Auxiliary\Build\vcvarsall.bat" (
    call "!VS_DIR!\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
    goto COMPILE
)
echo Visual Studio with the C++ x64 tools was not found.
echo Install the "Desktop development with C++" workload, or build with build.bat (MinGW-w64).
exit /b 1

:COMPILE
if not exist build mkdir build
if not exist build_msvc mkdir build_msvc
del /q build_msvc\*.obj >nul 2>&1
set "APP_VERSION=%~1"
if not defined APP_VERSION set "APP_VERSION=0.0.0"
set "APP_CHANNEL=%~2"
if not defined APP_CHANNEL set "APP_CHANNEL=dev"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\generate-version.ps1 -Version "%APP_VERSION%" -Channel "%APP_CHANNEL%" || exit /b 1
(for %%f in (lang\*.txt) do (echo @file %%~nf& type "%%f"& echo.)) > build\languages.txt
rc.exe /nologo /i res /i build /fo build_msvc\app.res res\app.rc || exit /b 1

set SOURCES=
for %%f in (src\*.cpp src\drivers\*.cpp) do set SOURCES=!SOURCES! %%f
cl.exe /nologo /std:c++17 /permissive- /O2 /EHsc /utf-8 /W3 /MT ^
    /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /D_UNICODE /DUNICODE /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 ^
    /c !SOURCES! /Fobuild_msvc\ || exit /b 1
link.exe /nologo /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup /OPT:REF ^
    build_msvc\*.obj build_msvc\app.res ^
    hid.lib setupapi.lib cfgmgr32.lib ole32.lib oleaut32.lib comctl32.lib gdi32.lib user32.lib shell32.lib advapi32.lib ^
    runtimeobject.lib ws2_32.lib winhttp.lib bcrypt.lib version.lib bluetoothapis.lib d3d11.lib dxgi.lib d2d1.lib dcomp.lib dwmapi.lib ^
    /OUT:BatteryHub.exe || exit /b 1
echo Built BatteryHub.exe with MSVC
