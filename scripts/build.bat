@echo off
setlocal

set "ROOT=%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake -S "%ROOT%" -B "%ROOT%\build" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build "%ROOT%\build"
