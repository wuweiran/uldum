# Imports MSVC x64 build environment (vcvarsall.bat x64) into the current
# PowerShell session so cmake, cl.exe, link.exe, and rc.exe resolve on PATH.
# Idempotent — dot-sourcing is a no-op once imported (detected via VCINSTALLDIR).
#
# PowerShell can't source .bat files directly (they set env in a child cmd
# that dies on return). Workaround: run vcvarsall in cmd, dump env via `set`,
# re-apply each var to this PowerShell process.

if ($env:VCINSTALLDIR) { return }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    $cmd = Get-Command vswhere -ErrorAction SilentlyContinue
    if (-not $cmd) { throw 'Cannot find vswhere.exe. Is Visual Studio installed?' }
    $vswhere = $cmd.Source
}

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath |
    Select-Object -First 1
if (-not $vsPath) { throw 'No Visual Studio installation with C++ tools found.' }

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path -LiteralPath $vcvars)) { throw "vcvarsall.bat not found at $vcvars" }

$envLines = cmd.exe /c "`"$vcvars`" x64 >nul && set"
foreach ($line in $envLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
