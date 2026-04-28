# Platform Distribution

Per-platform build and distribution notes. Lives alongside [design.md](design.md) (phase roadmap) and [packaging.md](packaging.md) (archive format).

## Windows

Primary desktop development platform. Ships as a self-contained folder under `dist\`.

**Prerequisites:**
- Visual Studio 2022+ with the "Desktop development with C++" workload
- Vulkan SDK 1.3+
- CMake 3.28+

### Engine build (developer iteration)

```powershell
scripts\build.ps1              # all engine targets (Debug)
scripts\build.ps1 -Release     # optimized build
scripts\build_dev.ps1          # single-target helpers
scripts\build_editor.ps1
scripts\build_server.ps1
```

**Output:** `build\bin\` — `uldum_dev.exe`, `uldum_editor.exe`, `uldum_server.exe`, `uldum_pack.exe`, `basisu.exe`, plus `engine.uldpak` and every `maps\*.uldmap\` from the engine repo packed into `build\bin\maps\`. **No `uldum_game.exe`** — that target only appears in per-project game builds (below).

Run `uldum_dev.exe` from `build\bin\` to iterate. Loads `maps/test_map.uldmap` by default; `--map <path>` picks a different one.

### Game build (packaging for shipping)

```powershell
scripts\build_game.ps1                       # sample_game, Debug  -> dist\UldumSample-debug\
scripts\build_game.ps1 -Release              # sample_game, Release -> dist\UldumSample\
scripts\build_game.ps1 ..\my-game -Release   # external game project
```

Produces a self-contained `dist\<GameName>[-debug]\` folder: `<GameName>.exe` (renamed from uldum_game, icon embedded from `<project>/branding/icon.ico`), `engine.uldpak`, only the maps listed in `game.json.maps[]`, and `game.json`. Zip it up for itch.io or point Steam's depot builder at the folder — no Windows installer generation yet (that's post-15d).

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
3. `scripts\build_android_game.ps1` auto-detects the SDK at `%LOCALAPPDATA%\Android\Sdk` (Studio's default). No other env vars needed.

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

The Gradle Wrapper (`gradlew`, `gradlew.bat`, `gradle/wrapper/gradle-wrapper.jar`) should be committed to the repo. Once present, `scripts\build_android_game.ps1` is self-sufficient and future developers on a fresh clone skip this step entirely.

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

### Two flavors: `dev` and `game`

The Android project has two Gradle product flavors. Different purposes, different workflows:

| Flavor | applicationId | Bundled content | Driver |
|---|---|---|---|
| `dev` | `clan.midnight.uldum_dev` | every `maps/*.uldmap/` from the engine repo | **Android Studio Run button** (or `gradlew assembleDevDebug`) |
| `game` | per-project (from `game.json.android.package_id`) | only maps listed in `game.json.maps[]` | `scripts\build_android_game.ps1 [<project>]` |

Different applicationIds → both APKs install side-by-side on one device, no conflict. The engine dev's iteration APK and the shipping game APK coexist.

### Dev flavor — engine iteration (Android Studio)

For debugging engine features on-device (touch input, Vulkan driver quirks, fog-of-war behavior on mobile GPU, performance). No PowerShell wrapper — Studio is the canonical tool.

**First time:**
```powershell
scripts\build.ps1                 # produces build\bin\uldum_pack.exe, needed by Gradle's stageDevAssets
```

**Then in Android Studio:**
1. Open `platforms\android\` (File → Open → select the folder).
2. Let Gradle sync (first time ~2 min, downloads wrapper + AGP).
3. **Build Variants** panel (View → Tool Windows → Build Variants) → pick `devDebug`.
4. Attach device or start an emulator (AVD) — x86_64 for host emulators, arm64-v8a for real devices.
5. Hit **Run** (Shift+F10). Gradle's `stageDevAssets` task packs every engine map + copies `engine.uldpak` into `src/dev/assets/`. APK installs and launches into the ImGui dev-console map picker — pick a map, tap Offline, the session starts. Same flow as desktop `uldum_dev`.

Set breakpoints in `src/app/android_main.cpp` or any engine code — Studio's LLDB attaches and stops on them. Logcat: `adb logcat -s Uldum:*`.

### Game flavor — shipping APK (PowerShell)

For packaging a specific game project (e.g., `sample_game/`) as a shippable APK.

```powershell
scripts\build.ps1                       # engine tools (first time, or after engine changes)
scripts\build_android_game.ps1          # debug APK  -> dist\<GameName>-debug.apk
scripts\build_android_game.ps1 -Release # release APK -> dist\<GameName>.apk (needs keystore.properties)
```

What the script does:
1. Resolves the game project dir (default: `sample_game/`). Reads `game.json`.
2. Pre-stages APK assets in `platforms\android\app\src\game\assets\`:
   - `engine.uldpak` from `build\bin\`
   - `game.json` from the project
   - Each `<project>/maps/<id>.uldmap/` packed with `uldum_pack.exe`
3. Invokes `gradlew assembleGameDebug` (or `assembleGameRelease`) with per-project properties: `applicationId`, `versionName`, app label, project dir (for icon res lookup).
4. Copies the resulting APK to `dist\<GameName>[-debug].apk`.

**Install / launch a game APK:**
```cmd
adb install -r dist\UldumSample-debug.apk
adb shell am start -n com.m1knight.uldum_sample/clan.midnight.uldum.MainActivity
```

The activity class lives in the engine namespace (`clan.midnight.uldum.MainActivity`) regardless of applicationId — that's by design; per-game namespaces would force every game to ship a MainActivity subclass for no reason.

**Release signing:** the release variant reads `<project>/keystore.properties` (gitignored). If absent, `-Release` fails fast with a message showing how to generate a keystore with `keytool`. See [build-targets.md](build-targets.md) for the game-project folder layout.

### Locked-in design decisions

- **Native activity:** `GameActivity` from AndroidX Games Activity Library (actively maintained, handles lifecycle + input better than legacy `NativeActivity`)
- **Min SDK:** 33 (Android 13). The engine uses Vulkan 1.3 core features (dynamic rendering, synchronization2, maintenance4); the NDK's `libvulkan.so` stub doesn't export 1.3 entry points as linkable symbols below API 33. Broader device coverage would require volk-style dynamic function loading with KHR-extension fallback — not planned.
- **Target / compile SDK:** 36 (Android 16) · **Build-Tools:** 36.0.0 · **NDK:** 29.0.14206865
- **ABIs:** `arm64-v8a` (devices) and `x86_64` (emulator). No armeabi-v7a.
- **Namespace fixed at `clan.midnight.uldum`** — only `applicationId` varies per flavor/game. `MainActivity` resolves relative to namespace.
- **Gradle Wrapper** pinned per repo, no separate install. AGP 9.x.

### What's deferred

- Play Store AAB packaging (`bundle` task; Store accepts APK for direct upload but AAB is required for Play Store publishing).
- iOS (requires MoltenVK and App Store submission — out of scope).
- Linux desktop (headless server would be first Linux target when we need it).
- ImGui-based dev lobby on Android — map picker / debug console. Compile-define-gated to dev flavor when it lands.

