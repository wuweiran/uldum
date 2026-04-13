@echo off
setlocal

set "BIN=%~dp0..\build\bin"

if not exist "%BIN%\uldum_dev.exe" (
    echo ERROR: uldum_dev.exe not found. Run scripts\build.bat first.
    exit /b 1
)

echo Starting host...
start "Uldum Host" /D "%BIN%" uldum_dev.exe --host

:: Wait for host to start listening before client connects
timeout /t 2 /nobreak >nul

echo Starting client...
start "Uldum Client" /D "%BIN%" uldum_dev.exe --connect 127.0.0.1

echo Both instances launched. Close the windows to stop.
