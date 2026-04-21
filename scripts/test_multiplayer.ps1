#Requires -Version 5.1
# Launch a host + client instance of uldum_dev on localhost for multiplayer
# smoke tests. Close the windows manually to stop.

$ErrorActionPreference = 'Stop'

$Bin = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\bin'
$Exe = Join-Path $Bin 'uldum_dev.exe'

if (-not (Test-Path -LiteralPath $Exe)) {
    throw "uldum_dev.exe not found. Run scripts\build.ps1 first."
}

Write-Host 'Starting host...'
Start-Process -FilePath $Exe -ArgumentList '--host' -WorkingDirectory $Bin

# Wait for the host to bind before the client tries to connect.
Start-Sleep -Seconds 2

Write-Host 'Starting client...'
Start-Process -FilePath $Exe -ArgumentList '--connect','127.0.0.1' -WorkingDirectory $Bin

Write-Host 'Both instances launched. Close the windows to stop.'
