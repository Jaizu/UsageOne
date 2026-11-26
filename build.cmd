@echo off
rem Builds bin\UsageOne.exe (with the Stream Deck plugin inside) using MSVC.
rem   build.cmd [dev] [output folder]
rem "dev" adds the development aids (USAGEONE_DEV): /theme, /icon-preview and
rem timing in the log. Releases are built without it.
setlocal
cd /d "%~dp0"

set "DEFS="
if /i "%~1"=="dev" (
    set "DEFS=/DUSAGEONE_DEV"
    shift
)
set "OUT=%~1"
if "%OUT%"=="" set "OUT=bin"

where cl >nul 2>nul
if not errorlevel 1 goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" goto novs
rem vcvars64 runs vswhere.exe from PATH too; without this it prints an error.
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
goto build

:novs
echo MSVC not found. Install "Desktop development with C++" or the VS Build Tools.
exit /b 1

:build
if not exist "%OUT%" mkdir "%OUT%"
if not exist obj\plugin mkdir obj\plugin

set CFLAGS=/nologo /std:c++20 /O2 /GL /Gy /MT /EHsc /W3 /permissive- /utf-8 /DUNICODE /D_UNICODE /DNDEBUG %DEFS%
set LFLAGS=/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO

rem 1. The Stream Deck plugin, which UsageOne.exe carries as a resource.
rc /nologo /fo obj\plugin.res res\plugin.rc || exit /b 1
cl %CFLAGS% /Fo:obj\plugin\ src\plugin\main.cpp src\plugin\connection.cpp src\plugin\keyimage.cpp ^
   src\plugin\install.cpp src\plugin\usageone.cpp src\settings.cpp src\trayicon.cpp obj\plugin.res ^
   /Fe:obj\UsageOne.StreamDeck.exe /link %LFLAGS% /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib d2d1.lib dwrite.lib windowscodecs.lib ^
   winhttp.lib crypt32.lib shlwapi.lib || exit /b 1

rem 2. The app.
rc /nologo /fo obj\app.res res\app.rc || exit /b 1
cl %CFLAGS% /Fo:obj\ src\main.cpp src\flyout.cpp src\colorpicker.cpp src\usage.cpp src\settings.cpp ^
   src\strings.cpp src\trayicon.cpp src\statuspipe.cpp src\streamdeck.cpp src\updater.cpp src\log.cpp obj\app.res ^
   /Fe:"%OUT%\UsageOne.exe" /link %LFLAGS% /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib dwmapi.lib d2d1.lib dwrite.lib shcore.lib ^
   d3d11.lib coremessaging.lib windowsapp.lib winhttp.lib bcrypt.lib || exit /b 1

echo.
echo Built %OUT%\UsageOne.exe
