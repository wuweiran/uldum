# Uldum Engine — Build Targets & Game Projects

`uldum_dev` and `uldum_game` share one `class Engine`. Each binary
links a concrete `App` implementation (`DevApp` or `SampleGameApp`)
picked via the `ULDUM_APP_CLASS` macro CMake sets per target.
`ULDUM_DEV_UI` and `ULDUM_SHELL_UI` are compile-time switches for the
*contents* of each binary (dev console vs Shell UI) — per-build
feature toggles, not the architectural seam. The full integration
model is in [`docs/engine-model.md`](engine-model.md).

## Two kinds of things in this repo

This repo contains an **engine** and one **game project** (`sample_game/`). They are different concepts with different build flows.

- **Engine** — the runtime underneath products. Lives in `src/`, `engine/`, `third_party/`, `platforms/`. Has no identity (no name, no icon, no shipped maps). Built into a handful of executables/libraries that developers use.
- **Game project** — a product. Has a name, an icon, a package ID, a lobby, a list of maps it ships. Built into a distributable `.exe` / `.apk`. `sample_game/` is the only one in this repo; real first-party games live in their own repos or folders.

The boundary matters: the engine never contains game content, and a game project never contains engine code. Build scripts reflect the split — engine scripts write to `build/bin/`, game scripts write to `dist/`.

## Engine build targets

Four executables + one packaging tool, all built from engine source.

| Target | Audience | Purpose |
|--------|----------|---------|
| `uldum_dev` | Engine devs | Run any map from `maps/`; debug overlay, dev console, hot-reload |
| `uldum_editor` | Map authors | ImGui terrain editor; opens `.uldmap` folders or archives |
| `uldum_worker` | Multiplayer | One process per active game session. Headless authoritative simulation (no window, no renderer, no audio) |
| `uldum_server` | Multiplayer | Orchestrator. HTTP API for game backends, spawns / reaps `uldum_worker` processes, dispatches webhooks on session end |
| `uldum_pack` | Build pipeline | Pack/unpack/list `.uldpak` and `.uldmap` archives |
| `uldum_game` | End users | Shipped product runtime. Parameterized by a game project (name, icon, bundled maps, package ID) |

`uldum_game` is the only engine target parameterized by a game project. The rest are pure engine.

### `uldum_dev` — developer runtime

- Reads every `.uldmap/` folder under `maps/`, packs them into `build/bin/maps/` at build time.
- Auto-loads `maps/test_map.uldmap` unless `--map <path>` is given.
- In-process local server for single-player; `--host` / `--connect` for multiplayer.
- Debug overlay, dev console, Lua hot-reload.
- Never ships to end users.

### `uldum_editor` — map editor

- Opens `.uldmap` folders (author mode, read/write) or `.uldmap` archives (packed mode, read-only).
- Terrain sculpt, paint, cliff/ramp, water, pathing, object placement, Lua script editing.
- No gameplay simulation, no network.
- Windows only.

### `uldum_worker` — per-session game server

- No window, no renderer, no audio.
- Authoritative simulation at 32 Hz, runs map Lua, accepts ENet connections.
- Reads `--map <path>` and `--port <n>` from CLI.
- One process per active game session. Spawned by `uldum_server` (orchestrator) in production, or runnable standalone for LAN / dev.

### `uldum_server` — orchestrator

- HTTP API that game backends call into to request a session: `POST /sessions`.
- Spawns `uldum_worker` processes on demand, picks UDP ports from a configured range, issues per-player session tokens.
- On game-end, POSTs result JSON to the game backend's configured webhook URL.
- See [network.md](network.md#production-deployment-topology) for the full architecture.

### `uldum_pack` — packaging tool

- `uldum_pack pack <dir> <output> [--encrypt --key <secret>]`
- `uldum_pack unpack <input> <dir> [--key <secret>]`
- `uldum_pack list <input>`
- Used by the build pipeline to produce `engine.uldpak` and `.uldmap` archives.

### `uldum_game` — shipped product runtime

What end users run. Parameterized by a game project at build time:

- Product name / window title → `game.json.name` + `game.json.window.title`
- Exe name / Android applicationId → per game project (Windows: `<Name>.exe`; Android: `game.json.android.package_id`)
- Bundled assets → `engine.uldpak` + the maps listed in `game.json.maps` + the project's `shell/` (RML/RCSS) + branding
- No dev console, no debug overlay, no map switching

## Game project folder layout

A game project is a self-contained folder. The engine builds against it; the engine doesn't care where it lives on disk. Per-platform settings (Windows exe name, Android `applicationId`) are all config fields in `game.json` — there's no `windows/` or `android/` subfolder; folders only exist where there's actual content to hold.

This repo's `sample_game/` is the reference:

```
sample_game/
  game.json                  product config — name, window, maps[], default_map, android{}
  branding/
    icon.png                 1024×1024, becomes .ico for Windows and Android launcher icons
  maps/                      only the maps that ship with this product
  keystore.properties        Android release-signing secrets (gitignored)
  keystore.properties.example  committed template
```

`shell/` holds the RML + RCSS for the menus, lobby, settings, and results screens. The game project's `App` (e.g. `SampleGameApp`) loads these and binds their buttons via `engine.shell()`.

### `game.json` fields

```json
{
    "name": "Uldum Sample",
    "version": "0.1.0",
    "window": {
        "title": "Uldum Sample",
        "width": 1920,
        "height": 1080
    },
    "default_port": 7777,
    "maps": ["simple_map"],
    "default_map": "maps/simple_map.uldmap",
    "android": {
        "package_id": "com.m1knight.uldum-sample",
        "app_name": "Uldum Sample"
    }
}
```

- `maps` — list of map IDs. Each resolves to `<project>/maps/<id>.uldmap/`. Build script packs only these into the dist.
- `default_map` — exe-relative path to the initial map at runtime. Relative to the shipped `.exe`'s working directory (always `maps/<id>.uldmap`).
- `android.package_id` — the `com.m1knight.<name>` published identifier.
- `android.app_name` — the app label shown on the Android launcher / task switcher.

## Scripts

All scripts live in `scripts/`. Engine scripts build the engine. Game scripts build a game project.

### Engine scripts

All build / test / asset-util scripts are PowerShell (`.ps1`). The helper at `scripts/_lib/vcvars.ps1` imports the MSVC x64 environment (via `vcvarsall.bat x64`) into the current PowerShell session; each build script dot-sources it.

| Script | Builds | Packs |
|---|---|---|
| `build.ps1` | all engine targets | `engine.uldpak`, every map in `maps/` |
| `build_dev.ps1` | `uldum_dev` only | `engine.uldpak`, every map in `maps/` |
| `build_editor.ps1` | `uldum_editor` only | `engine.uldpak` |
| `build_server.ps1` | server-side targets (`uldum_worker`, `uldum_server`) | `engine.uldpak`, every map in `maps/` |
| `convert_skybox.py` | — (asset util) | EXR → KTX2 cubemap faces |
| `png_to_ktx2.ps1` | — (asset util) | PNG → KTX2 |

Output: `build/bin/`. Engine scripts never write to `dist/` and never touch a game project.

### Game scripts

| Script | Builds |
|---|---|
| `build_game.ps1 [<project-dir>]` | `uldum_game` parameterized by the project. Defaults to `sample_game/`. Output: `dist/<GameName>/<GameName>.exe` + `engine.uldpak` + `maps/` + `game.json` + icon. |
| `build_android_game.ps1 [<project-dir>]` | Game-flavor APK from a game project. Gradle build with injected `applicationId`, app label, icon, asset source. Output: `dist/<GameName>.apk`. Defaults to `sample_game/`. For engine-dev Android iteration use Android Studio's **dev** build variant — no script, Gradle stages engine maps automatically. |

Game scripts always take (or default to) a game project directory. They never build the engine in isolation — they orchestrate the engine build alongside game-specific assets.

## Current state

- Engine build (`scripts\build.ps1`) produces `uldum_dev`, `uldum_editor`, `uldum_worker`, `uldum_server`, `uldum_pack` + `engine.uldpak` + every `maps/*.uldmap/` packed into `build/bin/maps/`. It does not produce `uldum_game` (that's per-project, via `build_game.ps1`).
- `sample_game/` ships `game.json`, `branding/icon.png` + committed `icon.ico` + committed Android launcher icons (`branding/android/mipmap-*/ic_launcher.png`), `maps/simple_map.uldmap/` (flat 32×32 grass), `shell/` with RML + RCSS, `game.cmake`, and `src/sample_game_app.{h,cpp}`.
- Parameterized Windows build (`scripts\build_game.ps1 [<project>]`) — per-project CMake build tree at `build/games/<project>/`; reads `game.json` via CMake's `file(READ ... JSON)`; exe renamed from `game.json.name` (whitespace stripped); icon embedded from `<project>/branding/icon.ico`; only maps listed in `game.json.maps` are packed. Output: `dist/<GameName>-<config>/<GameName>-<config>.exe` + `engine.uldpak` + `maps/` + `game.json` + `shell/`.
- Parameterized Android game build (`scripts\build_android_game.ps1 [<project>]`) — PowerShell extracts `android.package_id` / `android.app_name` / `version` from `game.json` and passes them to Gradle as `-P` properties. `applicationId`, `versionName`, and `android:label` (manifest placeholder) are per-project. Launcher icons come from `<project>/branding/android/` via Gradle `sourceSets.res.srcDirs`. APK assets pre-staged by PowerShell into `src/game/assets/` (engine.uldpak + game.json + uldum_pack-packed maps).
- Android dev flavor — second Gradle product flavor. Built via Android Studio's Run button (or `gradlew assembleDevDebug`). Fixed `applicationId = clan.midnight.uldum_dev`, bundles every `maps/*.uldmap/` from the engine repo, hardcoded default map = `test_map`. Gradle's `stageDevAssets` task packs maps + copies `engine.uldpak` into `src/dev/assets/`. Different applicationId → installs side-by-side with game-flavor APKs.
- Both Android flavors depend on `scripts\build.ps1` having run (needs `build/bin/uldum_pack.exe` and `build/bin/engine.uldpak` — the Android NDK doesn't ship glslc, so shaders and engine.uldpak are desktop-built and bundled in).
- File-logged runtime diagnostics: every log line is also written to `run.log` next to the exe, flushed on each write.

## Platform matrix

| Target | Windows | Android |
|--------|---------|---------|
| `uldum_dev` | .exe | .apk (dev flavor) |
| `uldum_editor` | .exe | — |
| `uldum_worker` | .exe | — |
| `uldum_server` | .exe | — |
| `uldum_pack` | .exe | — |
| `uldum_game` | .exe | .apk |
