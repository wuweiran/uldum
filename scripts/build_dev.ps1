#Requires -Version 5.1
# Build uldum_dev only (+ engine.uldpak and engine dev maps it depends on).

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '_lib\vcvars.ps1')

$Root  = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root 'build'

cmake -S $Root -B $Build -G Ninja -DCMAKE_BUILD_TYPE=Debug
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

cmake --build $Build --target uldum_dev
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
