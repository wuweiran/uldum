@echo off
setlocal

set "ROOT=%~dp0.."

:: Find Visual Studio installation using vswhere (ships with VS 2017+)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    :: Fallback: try PATH
    where vswhere >nul 2>&1
    if errorlevel 1 (
        echo ERROR: Cannot find vswhere.exe. Is Visual Studio installed?
        echo Install Visual Studio 2022+ with "Desktop development with C++" workload.
        exit /b 1
    )
    set "VSWHERE=vswhere"
)

:: Get the latest VS installation path
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_PATH=%%i"
)

if not defined VS_PATH (
    echo ERROR: No Visual Studio installation with C++ tools found.
    echo Install Visual Studio 2022+ with "Desktop development with C++" workload.
    exit /b 1
)

:: Set up MSVC environment
call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64

:: Configure and build
cmake -S "%ROOT%" -B "%ROOT%\build" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build "%ROOT%\build"
