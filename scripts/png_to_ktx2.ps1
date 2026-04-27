<#
.SYNOPSIS
Convert one or more PNG textures to KTX2 + UASTC.

.DESCRIPTION
Encodes each input PNG to a sibling .ktx2 using build\bin\basisu.exe.
Defaults to sRGB (for albedo/diffuse). Use -Linear for normal maps or data
textures. Mipmaps are always generated.

Requires: build\bin\basisu.exe (built by scripts\build.ps1).

.PARAMETER Linear
Encode as linear (for normal maps / data textures).

.PARAMETER Paths
One or more PNG files to convert.

.EXAMPLE
scripts\png_to_ktx2.ps1 textures\grass.png

.EXAMPLE
scripts\png_to_ktx2.ps1 -Linear textures\grass_normal.png textures\dirt_normal.png
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [switch]$Linear,
    [Parameter(Mandatory=$true, Position=0, ValueFromRemainingArguments=$true)]
    [string[]]$Paths
)
$ErrorActionPreference = 'Stop'

$Root   = Split-Path -Parent $PSScriptRoot
$basisu = Join-Path $Root 'build\bin\basisu.exe'
if (-not (Test-Path -LiteralPath $basisu)) {
    throw "$basisu not found. Run scripts\build.ps1 first."
}

foreach ($inPath in $Paths) {
    if (-not (Test-Path -LiteralPath $inPath)) {
        Write-Error "Input not found: $inPath"
        continue
    }
    $outPath = [IO.Path]::ChangeExtension($inPath, '.ktx2')
    Write-Host "Converting `"$inPath`" -> `"$outPath`""

    # `-ktx2_no_zstandard` is mandatory: this project's KTX2 loader
    # (basisu transcoder path) can't handle Zstandard-supercompressed
    # KTX2 — it rejects them at start_transcoding(). All shipped
    # textures must use Supercompression Scheme: NONE.
    $basisuArgs = @('-ktx2','-ktx2_no_zstandard','-uastc','-uastc_level','2','-mipmap',
                    '-output_file', $outPath, $inPath)
    if ($Linear) { $basisuArgs = @('-linear') + $basisuArgs }

    & $basisu @basisuArgs
    if ($LASTEXITCODE -ne 0) { throw "basisu failed on $inPath" }
}
