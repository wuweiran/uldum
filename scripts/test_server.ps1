#Requires -Version 5.1
# Launch uldum_server + two uldum_dev clients connecting to localhost.
# For dedicated-server + 2-player smoke tests.

$ErrorActionPreference = 'Stop'

$Bin    = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\bin'
$Server = Join-Path $Bin 'uldum_server.exe'
$Client = Join-Path $Bin 'uldum_dev.exe'

if (-not (Test-Path -LiteralPath $Server)) {
    throw "uldum_server.exe not found. Run scripts\build_server.ps1 first."
}
if (-not (Test-Path -LiteralPath $Client)) {
    throw "uldum_dev.exe not found. Run scripts\build.ps1 first."
}

Write-Host 'Starting dedicated server...'
Start-Process -FilePath $Server -WorkingDirectory $Bin

Start-Sleep -Seconds 2

Write-Host 'Starting Player 0 (client)...'
Start-Process -FilePath $Client -ArgumentList '--connect','127.0.0.1' -WorkingDirectory $Bin

Start-Sleep -Seconds 1

Write-Host 'Starting Player 1 (client)...'
Start-Process -FilePath $Client -ArgumentList '--connect','127.0.0.1' -WorkingDirectory $Bin

Write-Host 'Server + 2 clients launched. Close the windows to stop.'
