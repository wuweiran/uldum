#Requires -Version 5.1
<#
.SYNOPSIS
Build all engine targets.

.PARAMETER Release
Build optimized Release (/O2 /DNDEBUG). Default is Debug (/Od /RTC1 with PDBs).
Release is what you want for profiling or packaging; Debug is what you want
for day-to-day iteration.

.NOTES
The build tree is shared between configs: switching Debug <-> Release triggers
a full rebuild (Ninja honors CMAKE_BUILD_TYPE changes). This is fine for the
99% case where you pick one config and stick with it.
#>
[CmdletBinding()]
param([switch]$Release)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '_lib\vcvars.ps1')

$config = if ($Release) { 'Release' } else { 'Debug' }
$Root   = Split-Path -Parent $PSScriptRoot
$Build  = Join-Path $Root 'build'

Write-Host "Engine build: $config"

cmake -S $Root -B $Build -G Ninja "-DCMAKE_BUILD_TYPE=$config"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

cmake --build $Build
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }
