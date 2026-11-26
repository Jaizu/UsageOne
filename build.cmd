@echo off
rem Builds bin\UsageOne.exe using MSVC.
setlocal
cd /d "%~dp0"

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

rem Optional first argument: output folder (default: bin).
:build
set "OUT=%~1"
if "%OUT%"=="" set "OUT=bin"
if not exist "%OUT%" mkdir "%OUT%"
if not exist obj mkdir obj

set CFLAGS=/nologo /std:c++20 /O2 /GL /Gy /MT /EHsc /W3 /permissive- /utf-8 /DUNICODE /D_UNICODE /DNDEBUG /Fo:obj\
set LFLAGS=/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO

rc /nologo /fo obj\app.res res\app.rc || exit /b 1

cl %CFLAGS% src\main.cpp src\flyout.cpp src\usage.cpp src\settings.cpp src\strings.cpp src\trayicon.cpp src\log.cpp obj\app.res ^
   /Fe:"%OUT%\UsageOne.exe" /link %LFLAGS% /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib dwmapi.lib d2d1.lib dwrite.lib shcore.lib ^
   d3d11.lib coremessaging.lib windowsapp.lib || exit /b 1

rem The Stream Deck plugin (see streamdeck\ and tools\install-plugin.ps1).
cl %CFLAGS% src\plugin.cpp src\settings.cpp src\trayicon.cpp obj\app.res ^
   /Fe:"%OUT%\UsageOne.StreamDeck.exe" /link %LFLAGS% /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib d2d1.lib dwrite.lib windowscodecs.lib ^
   winhttp.lib crypt32.lib shlwapi.lib || exit /b 1

echo.
echo Built %OUT%\UsageOne.exe
