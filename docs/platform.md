# Platform Distribution

Per-platform build and distribution notes. Lives alongside [design.md](design.md) (phase roadmap) and [packaging.md](packaging.md) (archive format).

## Windows

Desktop development platform. Ships as a self-contained folder.

**Prerequisites:**
- Visual Studio 2022+ with the "Desktop development with C++" workload
- Vulkan SDK 1.3+
- CMake 3.28+

**Build:**
```powershell
scripts\build.ps1
```

**Output:** `build\bin\` — contains `uldum_dev.exe`, `uldum_game.exe`, `uldum_server.exe`, `uldum_editor.exe`, `uldum_pack.exe`, `basisu.exe`, plus `engine.uldpak` and `maps\*.uldmap` packs. The root-level `game.json` used to sit here; it's now part of `sample_game/` (see [build-targets.md](build-targets.md)).

Distribution zip (e.g., for Steam, itch.io) will be produced by `scripts\build_game.ps1 <project>` → `dist\<GameName>\` once the game-project pipeline is wired up. See the TODO list in [build-targets.md](build-targets.md).

## Android

Target platform for mobile. Ships as APK (debug) or AAB (Play Store, deferred).

### What you install (one-time)

Android Studio is not required, but it's fine if you already have it — it bundles exactly the same JDK, SDK, NDK, and build-tools we'd install otherwise, and the build script detects its default install path automatically.

**If you have Android Studio already:**

1. Through Android Studio's SDK Manager (or its CLI `sdkmanager`), install:
   - **Android SDK Platform 36** (`platforms;android-36`)
   - **Android SDK Build-Tools 36.0.0** (`build-tools;36.0.0`)
   - **NDK (Side by side) 29.0.14206865** (`ndk;29.0.14206865`)
2. `JAVA_HOME` points at a JDK 17+ (Android Studio ships one; Temurin/OpenJDK work too).
3. `scripts\build_android.ps1` auto-detects the SDK at `%LOCALAPPDATA%\Android\Sdk` (Studio's default). No other env vars needed.

**Studio-free install:**

1. **JDK 17+** — [Temurin](https://adoptium.net/) (Windows zip) or equivalent. Set `JAVA_HOME`.
2. **Android Command-Line Tools** — download the `cmdline-tools` zip from https://developer.android.com/studio (scroll to "Command line tools only"). Extract so `sdkmanager` ends up at `<SDK>/cmdline-tools/latest/bin/sdkmanager`.
3. Use `sdkmanager` to install the rest:
   ```cmd
   sdkmanager --install "platform-tools" "platforms;android-36" "build-tools;36.0.0" "ndk;29.0.14206865"
   ```
4. Set `ANDROID_HOME` to the SDK root (persisted or per-shell):
   ```cmd
   set ANDROID_HOME=C:\path\to\AndroidSdk
   ```
   `ANDROID_NDK_ROOT` is optional — AGP auto-discovers the NDK under `%ANDROID_HOME%\ndk\`.

Total footprint: ~3 GB. Compare to Android Studio's full install at ~8 GB with nothing extra for us.

### What's in the repo

Per-platform distribution artifacts live under `platforms/<os>/`. The Android project:

```
platforms/android/
  build.gradle.kts              Root Gradle config — plugin versions
  settings.gradle.kts           Included modules
  gradle.properties             JVM args and AGP flags
  gradlew / gradlew.bat         Gradle Wrapper launcher (committed)
  gradle/wrapper/               Wrapper metadata + jar (committed)
  app/
    build.gradle.kts            AGP config: SDK versions, NDK toolchain, signing, externalNativeBuild
    src/main/
      AndroidManifest.xml       Activity declaration, permissions
      java/com/uldum/game/      GameActivity subclass (Kotlin, <100 lines)
      cpp/CMakeLists.txt        Wrapper; delegates to our root CMakeLists via add_subdirectory
      assets/                   Populated at build time with engine.uldpak + .uldmap files
```

Android Studio users can open `platforms/android/` as a Gradle project; Studio drives the same `gradlew` commands behind the scenes. The Android C++ platform-abstraction layer lives under `src/platform/android/` (parallel to `src/platform/windows/`), kept separate from the Gradle scaffolding.

`platforms/windows/` is empty today — gains content only when Windows needs installer config, app manifest, icon, resource file, Store packaging, etc.

### First-time setup: bootstrap the Gradle wrapper

The Gradle Wrapper (`gradlew`, `gradlew.bat`, `gradle/wrapper/gradle-wrapper.jar`) should be committed to the repo. Once present, `scripts\build_android.ps1` is self-sufficient and future developers on a fresh clone skip this step entirely.

If a fresh checkout is missing the wrapper, bootstrap it once:

**Option A — Android Studio (easiest, recommended):**

1. Open Android Studio → **File** → **Open** → select `platforms\android\` (the directory containing `settings.gradle.kts`, **not** the repo root).
2. Studio detects the Gradle project, downloads the Gradle version pinned in `gradle/wrapper/gradle-wrapper.properties` (8.10.2), and on first sync generates `gradlew`, `gradlew.bat`, and the wrapper jar for you.
3. Wait for the initial sync to finish ("Gradle sync finished" in the status bar). Studio can be closed.
4. `git add platforms/android/gradlew platforms/android/gradlew.bat platforms/android/gradle/wrapper/gradle-wrapper.jar` and commit so nobody else has to repeat this.

**Option B — Studio-free (needs a system `gradle` on PATH):**

```cmd
cd platforms\android
gradle wrapper --gradle-version 8.10
```

Install Gradle once via [Chocolatey](https://chocolatey.org/) (`choco install gradle`), Scoop (`scoop install gradle`), or a manual zip extract. After this command, the system Gradle is no longer needed — the generated wrapper runs Gradle itself going forward.

### Build

```powershell
scripts\build_android.ps1
```

What it does:
1. Verifies `ANDROID_SDK_ROOT`, `ANDROID_NDK_ROOT`, `JAVA_HOME` are set
2. `cd platforms\android` then `gradlew.bat assembleDebug` (the Gradle Wrapper is vendored as `.bat` — not our code)
3. AGP invokes CMake with the Android NDK toolchain, building `libuldum_game.so` for `arm64-v8a` (and `x86_64` for emulator)
4. AGP packages: assets + dex + manifest + native libs → zipalign → debug-signed APK
5. Output: `android/app/build/outputs/apk/debug/app-debug.apk`

### Install & run

```cmd
adb install -r android\app\build\outputs\apk\debug\app-debug.apk
adb shell am start -n com.uldum.game/.MainActivity
```

Logcat: `adb logcat -s Uldum:*`

### Locked-in design decisions

- **Native activity:** `GameActivity` from AndroidX Games Activity Library (actively maintained, handles lifecycle + input better than legacy `NativeActivity`)
- **Min SDK:** 33 (Android 13). The engine uses Vulkan 1.3 core features (dynamic rendering, synchronization2, maintenance4); the NDK's `libvulkan.so` stub doesn't export 1.3 entry points as linkable symbols below API 33. Broader device coverage would require volk-style dynamic function loading with KHR-extension fallback — not planned for now.
- **Target / compile SDK:** 36 (Android 16) · **Build-Tools:** 36.0.0 · **NDK:** 29.0.14206865
- **ABIs:** `arm64-v8a` (devices) and `x86_64` (emulator). No armeabi-v7a.
- **Gradle:** Wrapper-pinned per repo, no separate install
- **AGP:** 8.x series

### What's deferred

- Play Store AAB packaging, release signing keystore workflow
- iOS (requires MoltenVK and App Store submission — out of scope)
- Linux desktop (headless server would be first Linux target when we need it)

