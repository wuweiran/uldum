@echo off
setlocal

set "ROOT=%~dp0.."
set "ANDROID=%ROOT%\platforms\android"

:: Per-machine proxy / env overrides (see scripts\build.bat for the pattern)
if exist "%~dp0build_local.bat" call "%~dp0build_local.bat"

if not defined JAVA_HOME (
    echo ERROR: JAVA_HOME is not set. Install JDK 17+ ^(Android Studio bundles one,
    echo or https://adoptium.net/^) and set JAVA_HOME to its install directory.
    exit /b 1
)

:: Auto-detect the Android SDK if env vars aren't set. Android Studio installs
:: to %LOCALAPPDATA%\Android\Sdk by default. Gradle honors ANDROID_HOME first,
:: then ANDROID_SDK_ROOT, then local.properties.
if not defined ANDROID_HOME if not defined ANDROID_SDK_ROOT (
    if exist "%LOCALAPPDATA%\Android\Sdk" (
        set "ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk"
        echo [auto-detect] ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk
    ) else (
        echo ERROR: No Android SDK found. Install via Android Studio or the
        echo Command-Line Tools ^(https://developer.android.com/studio^), then set
        echo ANDROID_HOME to the SDK directory.
        exit /b 1
    )
)

if not exist "%ANDROID%\gradlew.bat" (
    echo ERROR: %ANDROID%\gradlew.bat not found. Bootstrap the Gradle Wrapper once:
    echo.
    echo   Option A ^(easiest if you have Android Studio^):
    echo     Open Android Studio ^> File ^> Open ^> select "%ANDROID%"
    echo     Studio auto-syncs and generates gradlew, gradlew.bat, and
    echo     gradle/wrapper/gradle-wrapper.jar. Close Studio when done.
    echo.
    echo   Option B ^(Studio-free — needs system Gradle^):
    echo     cd platforms\android ^&^& gradle wrapper --gradle-version 8.10
    echo.
    echo After the wrapper files exist, this script is self-sufficient.
    echo See docs\platform.md for details.
    exit /b 1
)

pushd "%ANDROID%"
call .\gradlew.bat assembleDebug
set "RC=%ERRORLEVEL%"
popd

if not "%RC%"=="0" (
    echo.
    echo Build failed ^(exit %RC%^). Check Gradle output above.
    exit /b %RC%
)

echo.
echo Built: %ANDROID%\app\build\outputs\apk\debug\app-debug.apk
echo Install:  adb install -r "%ANDROID%\app\build\outputs\apk\debug\app-debug.apk"
