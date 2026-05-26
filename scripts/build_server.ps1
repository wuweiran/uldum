#Requires -Version 5.1
# Build the server-side binaries (headless, no renderer/audio/window):
#   - uldum_worker   per-session worker (one process per active game session)
#   - uldum_server   orchestrator (HTTP API, spawns workers)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '_lib\vcvars.ps1')

$Root  = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root 'build'

cmake -S $Root -B $Build -G Ninja -DCMAKE_BUILD_TYPE=Debug
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

cmake --build $Build --target uldum_worker uldum_server
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
