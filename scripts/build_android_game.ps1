<#
.SYNOPSIS
Build and package a Uldum game project as an Android APK.

.DESCRIPTION
Takes a game project directory and produces a signed APK with:
  - applicationId     from game.json.android.package_id
  - app launcher name from game.json.android.app_name
  - versionName       from game.json.version
  - launcher icons    from <project>/branding/android/mipmap-*/ic_launcher.png
  - engine.uldpak + maps/*.uldmap + game.json bundled in APK assets

Debug APKs are signed with Android's default debug keystore (implicit). Release
APKs require <project>/keystore.properties (gitignored) pointing at a real
release keystore you generated with `keytool` — the Play Store rejects debug-
signed APKs. See sample_game/keystore.properties.example for format.

Depends on scripts\build.ps1 having run first (needs build\bin\uldum_pack.exe
to pack the game's maps, and build\bin\engine.uldpak for the engine assets).

.PARAMETER ProjectDir
Path to the game project directory, absolute or relative to repo root.
Defaults to 'sample_game'.

.PARAMETER Release
Build a release APK (signed with the project's release keystore). Without
this switch the output is a debug APK, suffixed '-debug' in dist/ to
discourage accidental shipping.

.EXAMPLE
scripts\build_android_game.ps1                    # sample_game debug -> dist\UldumSample-debug.apk
scripts\build_android_game.ps1 -Release           # sample_game release -> dist\UldumSample.apk
#>
#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Position=0)]
    [string]$ProjectDir = 'sample_game',
    [switch]$Release
)
$ErrorActionPreference = 'Stop'

$Root    = Split-Path -Parent $PSScriptRoot
$Android = Join-Path $Root 'platforms\android'

# Per-machine proxy / env overrides (gitignored).
$localOverride = Join-Path $PSScriptRoot 'build_local.ps1'
if (Test-Path -LiteralPath $localOverride) { . $localOverride }

# ── JDK / Android SDK ────────────────────────────────────────────────────
if (-not $env:JAVA_HOME) {
    throw @'
JAVA_HOME is not set. Install JDK 17+ (Android Studio bundles one, or
https://adoptium.net/) and set JAVA_HOME to its install directory.
'@
}
if (-not $env:ANDROID_HOME -and -not $env:ANDROID_SDK_ROOT) {
    $sdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk'
    if (Test-Path -LiteralPath $sdk) {
        $env:ANDROID_HOME = $sdk
        Write-Host "[auto-detect] ANDROID_HOME=$sdk"
    } else {
        throw @'
No Android SDK found. Install via Android Studio or the Command-Line Tools
(https://developer.android.com/studio), then set ANDROID_HOME to the SDK
directory.
'@
    }
}

# ── Resolve + validate the game project ──────────────────────────────────
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

$gameName      = $gameJson.name
$packageId     = $gameJson.android.package_id
$appName       = $gameJson.android.app_name
$versionName   = if ($gameJson.version) { $gameJson.version } else { '0.0.0' }
if (-not $packageId) { throw "game.json 'android.package_id' is missing" }
if (-not $appName)   { throw "game.json 'android.app_name' is missing" }

$brandingAndroid = Join-Path $projectAbs 'branding\android'
if (-not (Test-Path -LiteralPath $brandingAndroid -PathType Container)) {
    throw "Android launcher icons not found at $brandingAndroid."
}

# ── Release keystore ─────────────────────────────────────────────────────
# Release builds must be signed with a real keystore. Fail fast with a
# useful message if the dev hasn't set one up, rather than letting Gradle
# produce an -unsigned.apk that can't be installed anywhere.
if ($Release) {
    $keystoreProps = Join-Path $projectAbs 'keystore.properties'
    if (-not (Test-Path -LiteralPath $keystoreProps)) {
        throw @"
Release APK needs a signing keystore at $keystoreProps.

To generate one:
  keytool -genkeypair -v -keystore $projectAbs\release.keystore ``
          -alias release -keyalg RSA -keysize 2048 -validity 10000

Then copy $projectAbs\keystore.properties.example to keystore.properties
and fill in the passwords you chose.

Back up the keystore securely — the Play Store binds your applicationId to
its signature forever.
"@
    }
}

# ── Desktop build outputs (produced by scripts\build.ps1) ────────────────
$uldumPack    = Join-Path $Root 'build\bin\uldum_pack.exe'
$engineUldpak = Join-Path $Root 'build\bin\engine.uldpak'
if (-not (Test-Path -LiteralPath $uldumPack)) {
    throw "uldum_pack.exe not found at $uldumPack. Run scripts\build.ps1 first."
}
if (-not (Test-Path -LiteralPath $engineUldpak)) {
    throw "engine.uldpak not found at $engineUldpak. Run scripts\build.ps1 first."
}

# ── Stage APK assets ─────────────────────────────────────────────────────
# Flavor-scoped: game flavor reads from src/game/assets/ (dev flavor has its
# own src/dev/assets/ populated by Gradle's stageDevAssets task).
$assetsDir = Join-Path $Android 'app\src\game\assets'
$assetsMapsDir = Join-Path $assetsDir 'maps'
New-Item -ItemType Directory -Path $assetsDir -Force | Out-Null
if (Test-Path -LiteralPath $assetsMapsDir) {
    Remove-Item -LiteralPath $assetsMapsDir -Recurse -Force
}
New-Item -ItemType Directory -Path $assetsMapsDir -Force | Out-Null

Write-Host "Staging APK assets into $assetsDir ..."
Copy-Item -LiteralPath $engineUldpak -Destination (Join-Path $assetsDir 'engine.uldpak') -Force
Copy-Item -LiteralPath $gameJsonPath -Destination (Join-Path $assetsDir 'game.json') -Force

if (-not $gameJson.maps -or $gameJson.maps.Count -eq 0) {
    throw "game.json 'maps' array is empty"
}
foreach ($mapId in $gameJson.maps) {
    $mapSrc = Join-Path $projectAbs "maps\$mapId.uldmap"
    if (-not (Test-Path -LiteralPath $mapSrc -PathType Container)) {
        throw "game.json references '$mapId' but $mapSrc does not exist"
    }
    $mapDst = Join-Path $assetsMapsDir "$mapId.uldmap"
    & $uldumPack pack $mapSrc $mapDst
    if ($LASTEXITCODE -ne 0) { throw "uldum_pack failed for '$mapId'" }
}

# ── Gradle ───────────────────────────────────────────────────────────────
$gradlew = Join-Path $Android 'gradlew.bat'
if (-not (Test-Path -LiteralPath $gradlew)) {
    throw @"
$gradlew not found. Bootstrap the Gradle Wrapper once (see docs\platform.md):
  Option A: open "$Android" in Android Studio (auto-syncs and generates gradlew).
  Option B: cd platforms\android; gradle wrapper --gradle-version 8.10
"@
}

# Flavor-aware Gradle task names: assembleGameDebug / assembleGameRelease.
$gradleTask  = if ($Release) { 'assembleGameRelease' } else { 'assembleGameDebug' }
$variantDir  = if ($Release) { 'game/release' }       else { 'game/debug' }
$apkName     = if ($Release) { 'app-game-release.apk' } else { 'app-game-debug.apk' }
$debugSuffix = if ($Release) { '' }                    else { '-debug' }

Write-Host ''
Write-Host "Building APK: '$gameName' ($variant)"
Write-Host "  applicationId: $packageId"
Write-Host "  app label:     $appName"
Write-Host "  version:       $versionName"
Write-Host ''

Push-Location $Android
try {
    & $gradlew $gradleTask `
        "-PulGameProjectDir=$projectAbs" `
        "-PulApplicationId=$packageId" `
        "-PulAppName=$appName" `
        "-PulVersionName=$versionName"
    if ($LASTEXITCODE -ne 0) {
        throw "Gradle build failed (exit $LASTEXITCODE). Check output above."
    }
} finally {
    Pop-Location
}

# ── Copy APK into dist\ ──────────────────────────────────────────────────
$exeName = $gameName -replace '\s', ''
$distDir = Join-Path $Root 'dist'
New-Item -ItemType Directory -Path $distDir -Force | Out-Null
$apkSrc = Join-Path $Android "app\build\outputs\apk\$variantDir\$apkName"
if (-not (Test-Path -LiteralPath $apkSrc)) {
    throw "Expected APK not found at $apkSrc. Gradle output may have moved."
}
$apkDst = Join-Path $distDir "$exeName$debugSuffix.apk"
Copy-Item -LiteralPath $apkSrc -Destination $apkDst -Force

Write-Host ''
Write-Host "Built:   $apkDst"
Write-Host "Install: adb install -r `"$apkDst`""
Write-Host "Launch:  adb shell am start -n $packageId/clan.midnight.uldum.MainActivity"
