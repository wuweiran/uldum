@echo off
setlocal enabledelayedexpansion
::
:: png_to_ktx2.bat — convert one or more PNG textures to KTX2 + UASTC.
::
:: Usage:
::   scripts\png_to_ktx2.bat [--linear] <file.png> [<file2.png> ...]
::
:: Flags:
::   --linear   Encode as linear (for normal maps / data textures).
::              Default is sRGB (for albedo / diffuse).
::
:: Output: <file>.ktx2 next to each input PNG. Mipmaps generated.
::
:: Requires: build\bin\basisu.exe (built by scripts\build.bat).
::

set "ROOT=%~dp0.."
set "BASISU=%ROOT%\build\bin\basisu.exe"

if not exist "%BASISU%" (
    echo ERROR: %BASISU% not found. Run scripts\build.bat first.
    exit /b 1
)

set "LINEAR="

:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="--linear" (
    set "LINEAR=-linear"
    shift
    goto :parse_args
)
goto :convert

:convert
if "%~1"=="" goto :args_done

set "IN=%~1"
set "OUT=%~dpn1.ktx2"

echo Converting "%IN%" -^> "%OUT%"
"%BASISU%" -ktx2 -uastc -uastc_level 2 -mipmap %LINEAR% -output_file "%OUT%" "%IN%"
if errorlevel 1 (
    echo ERROR: basisu failed on %IN%
    exit /b 1
)

shift
goto :convert

:args_done
endlocal
