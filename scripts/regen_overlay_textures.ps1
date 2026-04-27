<#
.SYNOPSIS
Regenerate the procedural overlay decal textures (engine/textures/overlays/*.ktx2).

.DESCRIPTION
Runs the uldum_gen_overlays tool to emit fresh PNGs into
build/overlay_sources/, then converts each one to KTX2 (linear) at
engine/textures/overlays/. KTX2 files are the runtime assets the
engine loads. PNGs are intermediates — not committed.

.PARAMETER Build
If set, runs scripts\build.ps1 first to ensure the generator and
basisu are up to date.

.PARAMETER Dest
Destination directory for the produced KTX2 files. Defaults to the
test map's overlay folder; pass a map's textures/overlays path to
install there instead.

.EXAMPLE
scripts\regen_overlay_textures.ps1

.EXAMPLE
scripts\regen_overlay_textures.ps1 -Build

.EXAMPLE
scripts\regen_overlay_textures.ps1 -Dest maps\my_map.uldmap\textures\overlays
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [switch]$Build,
    [string]$Dest
)
$ErrorActionPreference = 'Stop'

$Root      = Split-Path -Parent $PSScriptRoot
$Generator = Join-Path $Root 'build\bin\uldum_gen_overlays.exe'
$PngDir    = Join-Path $Root 'build\overlay_sources'
if (-not $Dest) { $Dest = 'maps\test_map.uldmap\textures\overlays' }
$KtxDir    = if ([IO.Path]::IsPathRooted($Dest)) { $Dest } else { Join-Path $Root $Dest }

if ($Build) {
    & (Join-Path $PSScriptRoot 'build.ps1')
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

if (-not (Test-Path -LiteralPath $Generator)) {
    throw "$Generator not found. Run scripts\build.ps1 first or pass -Build."
}

# Step 1: regenerate PNG sources.
& $Generator $PngDir
if ($LASTEXITCODE -ne 0) { throw "uldum_gen_overlays failed" }

# Step 2: convert each PNG → KTX2 (linear; alpha-mask data, not color).
$Pngs = Get-ChildItem -LiteralPath $PngDir -Filter '*.png' | ForEach-Object { $_.FullName }
if (-not $Pngs) { throw "No PNGs found in $PngDir" }

& (Join-Path $PSScriptRoot 'png_to_ktx2.ps1') -Linear @Pngs
if ($LASTEXITCODE -ne 0) { throw "png_to_ktx2 failed" }

# Step 3: move the produced KTX2s next to the PNGs into engine/.
New-Item -ItemType Directory -Force -Path $KtxDir | Out-Null
foreach ($png in $Pngs) {
    $ktx = [IO.Path]::ChangeExtension($png, '.ktx2')
    if (-not (Test-Path -LiteralPath $ktx)) { throw "Missing $ktx" }
    $dest = Join-Path $KtxDir (Split-Path -Leaf $ktx)
    Move-Item -LiteralPath $ktx -Destination $dest -Force
    Write-Host "Installed $dest"
}
