@echo off
setlocal

set "ROOT=%~dp0.."

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    where vswhere >nul 2>&1
    if errorlevel 1 (
        echo ERROR: Cannot find vswhere.exe. Is Visual Studio installed?
        exit /b 1
    )
    set "VSWHERE=vswhere"
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_PATH=%%i"
)

if not defined VS_PATH (
    echo ERROR: No Visual Studio installation with C++ tools found.
    exit /b 1
)

call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake -S "%ROOT%" -B "%ROOT%\build" -G "Ninja" -DCMAKE_BUILD_TYPE=Debug
cmake --build "%ROOT%\build" --target uldum_game
