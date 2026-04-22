<#
.SYNOPSIS
Build and package a Uldum game project for Windows.

.DESCRIPTION
Takes a game project directory (a folder containing game.json, branding/,
maps/, ...) and produces a shippable dist folder:
  - <GameName>.exe    (renamed from uldum_game, icon embedded from branding/)
  - engine.uldpak     (packed engine assets)
  - maps/*.uldmap     (only the maps listed in game.json.maps)
  - game.json         (product config — read at runtime)

Per-config CMake build tree under build/games/<project>/<config>/; dist
output at dist/<GameName>/ (Release) or dist/<GameName>-debug/ (Debug). The
engine scripts never write to dist/ — this script is the sole producer of
shippable Windows bits.

.PARAMETER ProjectDir
Path to the game project directory, absolute or relative to repo root.
Defaults to 'sample_game'.

.PARAMETER Release
Build optimized Release. Default is Debug — fine for packaging-flow testing,
but dist output gets a '-debug' suffix so you can't accidentally ship it.

.EXAMPLE
scripts\build_game.ps1                       # sample_game, Debug -> dist\UldumSample-debug\
scripts\build_game.ps1 -Release              # sample_game, Release -> dist\UldumSample\
scripts\build_game.ps1 ..\my-game -Release   # external project, Release
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Position=0)]
    [string]$ProjectDir = 'sample_game',
    [switch]$Release
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '_lib\vcvars.ps1')

$config       = if ($Release) { 'Release' } else { 'Debug' }
$configLower  = $config.ToLower()
$debugSuffix  = if ($Release) { '' } else { '-debug' }

$Root = Split-Path -Parent $PSScriptRoot

# Resolve project dir to an absolute path. Accept relative paths from the
# repo root for convenience ("sample_game") and absolute paths for external
# games ("C:\my-game").
$projectAbs = if ([IO.Path]::IsPathRooted($ProjectDir)) {
    $ProjectDir
} else {
    Join-Path $Root $ProjectDir
}
$projectAbs = [IO.Path]::GetFullPath($projectAbs).TrimEnd('\', '/')

if (-not (Test-Path -LiteralPath $projectAbs -PathType Container)) {
    throw "Game project directory not found: $projectAbs"
}
$gameJsonPath = Join-Path $projectAbs 'game.json'
if (-not (Test-Path -LiteralPath $gameJsonPath)) {
    throw "game.json not found in $projectAbs"
}

$gameJson = Get-Content -LiteralPath $gameJsonPath -Raw | ConvertFrom-Json
if (-not $gameJson.name) { throw "game.json 'name' field is missing" }
$gameName = $gameJson.name
# PascalCase exe name: strip whitespace from display name ("Uldum Sample" ->
# "UldumSample"). Must match the OUTPUT_NAME computed in src/app/CMakeLists.txt.
$exeName = $gameName -replace '\s', ''

$projectDirName = Split-Path -Leaf $projectAbs
# Per-config build tree keeps Debug and Release artifacts isolated — a game
# dev can iterate in Debug and build Release occasionally without clobbering
# either.
$buildTree = Join-Path $Root "build\games\$projectDirName\$configLower"
$distDir   = Join-Path $Root "dist\$exeName$debugSuffix"

Write-Host "Building game: '$gameName' -> $exeName$debugSuffix.exe ($config)"
Write-Host "  project: $projectAbs"
Write-Host "  build:   $buildTree"
Write-Host "  dist:    $distDir"
Write-Host ''

cmake -S $Root -B $buildTree -G Ninja `
    "-DCMAKE_BUILD_TYPE=$config" `
    "-DULDUM_GAME_PROJECT_DIR=$projectAbs"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

cmake --build $buildTree --target uldum_game
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

# Assemble dist/ from the build tree's bin/. Copy only the shipping bits —
# uldum_pack.exe and basisu.exe are build-time tools, skipped.
Write-Host ''
Write-Host "Assembling $distDir ..."
if (Test-Path -LiteralPath $distDir) {
    # Best-effort wipe — a prior run of the exe (or antivirus scanning it)
    # can briefly hold the directory open. If removal fails we fall back to
    # in-place overwrite, which is fine for shippable bits.
    Remove-Item -LiteralPath $distDir -Recurse -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Path $distDir -Force | Out-Null

$binDir = Join-Path $buildTree 'bin'
Copy-Item -LiteralPath (Join-Path $binDir "$exeName.exe")  -Destination (Join-Path $distDir "$exeName$debugSuffix.exe")
Copy-Item -LiteralPath (Join-Path $binDir 'engine.uldpak') -Destination $distDir
Copy-Item -LiteralPath (Join-Path $binDir 'maps')          -Destination $distDir -Recurse
Copy-Item -LiteralPath (Join-Path $binDir 'game.json')     -Destination $distDir

$shellDir = Join-Path $binDir 'shell'
if (Test-Path -LiteralPath $shellDir) {
    Copy-Item -LiteralPath $shellDir -Destination $distDir -Recurse
}

Write-Host ''
Write-Host "Built: $distDir\$exeName$debugSuffix.exe ($config)"
