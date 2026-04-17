@echo off
setlocal

set "BIN=%~dp0..\build\bin"

if not exist "%BIN%\uldum_server.exe" (
    echo ERROR: uldum_server.exe not found. Run scripts\build_server.bat first.
    exit /b 1
)

if not exist "%BIN%\uldum_dev.exe" (
    echo ERROR: uldum_dev.exe not found. Run scripts\build.bat first.
    exit /b 1
)

echo Starting dedicated server...
start "Uldum Server" /D "%BIN%" uldum_server.exe

:: Wait for server to start listening
timeout /t 2 /nobreak >nul

echo Starting Player 0 (client)...
start "Uldum Player 0" /D "%BIN%" uldum_dev.exe --connect 127.0.0.1

timeout /t 1 /nobreak >nul

echo Starting Player 1 (client)...
start "Uldum Player 1" /D "%BIN%" uldum_dev.exe --connect 127.0.0.1

echo Server + 2 clients launched. Close the windows to stop.
