# Uldum Engine

A unit-centric game engine inspired by Warcraft III, built with modern C++23. Two interchangeable GPU backends — Vulkan 1.3 (Windows + Android) and OpenGL ES 3.1+ (Android fallback for non-Vulkan-1.3 devices).

## Features (planned)

- **Vulkan 1.3 / OpenGL ES rendering** — GPU-driven rendering with bindless textures (Vulkan) or sampler-array fallback (GLES); shared shader sources, separate per-backend stages
- **Skeletal animation** — GPU skinning, animation state machine, glTF models
- **Particle effects** — CPU-driven billboard particles, named effect system
- **Unit-centric gameplay** — ECS internally, unit facade for scripting
- **Lua 5.4 scripting** — trigger/event system, per-scene scripts with `require()`, persistent save data
- **Map system** — heightmap terrain, object placement, asset overrides
- **Audio** — 3D positional SFX, music streaming, ambient loops (miniaudio)
- **Server-authoritative multiplayer** — with single player via in-process local server
- **Terrain editor** — in-engine ImGui-based editor (sculpt, paint, cliff, ramp, pathing)
- **Cross-platform** — Windows and Android

## Current Status

Phase 26 (App architecture revamp) — `uldum_dev` refactored onto the new `Engine + App` model and migrated to a public surface; remaining sub-steps (sample_game conversion + Shell facade) are in progress.

**What works:**

- **Cross-platform GPU.** Vulkan 1.3 (dynamic rendering, sync2, bindless) and OpenGL ES 3.1+ behind a single command-list API. Shared assets and shaders; one backend picked at startup.
- **Asset pipeline.** glTF 2.0 models (`KHR_texture_basisu` supported), KTX2 + Basis Universal textures, OGG audio, JSON configs. Per-primitive materials on static meshes (`baseColorTexture` × `baseColorFactor`, `alphaMode=MASK`, `doubleSided`).
- **GPU-driven static rendering.** Mega VB/IB, bindless textures, instance SSBO, multi-draw indirect partitioned by pipeline class × cull mode. 4× MSAA, frustum culling, PCF shadow maps with correctly silhouetted alpha-masked casters.
- **Terrain.** Heightmap + cliffs + ramps, splatmap with 4 layers, SDF-based transitions with noise, sampler2DArray for layer textures, shallow + deep water with Gerstner waves and reflection.
- **Skeletal animation.** GPU skinning, animation state machine driven by simulation, crossfade blending, skinned shadow casting.
- **Effects + particles.** CPU-driven billboards with burst + continuous emitters, procedural shapes, glow-particle-seeded point lights, unit-attached effects with bone targeting.
- **ECS simulation.** Monotonic typed handles (Unit / Item / Destructable / Projectile), paged sparse-set storage, data-driven types loaded from map JSON, deterministic 32 Hz tick. WC3-style facing + scale + alliances.
- **Gameplay primitives.** Tile-grid A* + steering, attack-move + auto-acquire, abilities with typed innate/item/applied ownership, items as ability bundles, projectiles, cliff-aware fog of war with shared vision.
- **Lua 5.4 scripting.** Sandboxed sol2 VM, WC3-style triggers backed by typed stack-local event frames, timers, regions, scene switching mid-session (Lua VM swap), camera scripting, save-data channel.
- **Audio.** miniaudio with 3D positional SFX, music streaming, ambient loops, per-channel volume mixing.
- **Server-authoritative networking.** ENet UDP, fog-filtered snapshots, host barrier on scene swap, reconnect-with-state-restore. Local single-player runs the same server in-process.
- **Production server topology.** `uldum_worker` (one process per session, game-agnostic, Linux + Windows) + `uldum_server` orchestrator (HTTP API, per-player tokens, webhook dispatch).
- **Shell UI** (game builds) — RmlUi 6 driven by project-supplied `.rml` / `.rcss`; networked lobby with host-authoritative slot table.
- **HUD** (all builds) — custom retained-mode tree of atoms + composites (action_bar, command_bar, minimap, joystick, inventory, mobile nearby-item pickup), MSDF text, world overlays, drag-cast.
- **In-engine terrain editor.** Sculpt / paint / cliff / ramp / pathing / object placement, source-folder or packed-map workflow.
- **Build targets.** `uldum_dev`, `uldum_game` (per-project), `uldum_worker`, `uldum_server`, `uldum_editor`, `uldum_pack`. Windows + Android (dev APK and game-flavor APK).

## Prerequisites

Install these before building:

1. **Visual Studio 2022 or later** (Community edition is free)
   - During installation, select the **"Desktop development with C++"** workload
   - Download: https://visualstudio.microsoft.com/

2. **Vulkan SDK 1.3+**
   - Download and install from https://vulkan.lunarg.com/sdk/home
   - Verify: `vulkaninfo`

3. **CMake 3.28+**
   - Usually included with Visual Studio
   - Verify: `cmake --version`

4. **GPU with Vulkan 1.3 support**

## Build (Windows)

Using the build script (auto-detects Visual Studio, uses Ninja for parallel builds):

```powershell
scripts\build.ps1
```

First build fetches third-party dependencies via CMake FetchContent (takes ~1 min). Subsequent builds are incremental.

> If PowerShell blocks the script with an execution-policy error, either run it as `powershell -ExecutionPolicy Bypass -File scripts\build.ps1`, or allow local scripts once via `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`.

Output: `build/bin/uldum_dev.exe` (developer runtime), `build/bin/uldum_editor.exe` (map editor). See [docs/build-targets.md](docs/build-targets.md) for details.

## Run

```cmd
cd build\bin
uldum_dev.exe
```

`uldum_dev` boots into an ImGui dev console that lists every `.uldmap` in `build/bin/maps/` — pick a map, choose Offline / Host / Client, and the session starts. The same dev console flow runs on Android via the dev-flavor APK (Phase 16d-i). To skip the picker for CLI-driven iteration, pass `--map <path>` to auto-start a session on that map.

**Multiplayer (LAN):**
```cmd
:: Terminal 1 — host
uldum_dev.exe --host --map maps/test_map.uldmap

:: Terminal 2 — join
uldum_dev.exe --connect 127.0.0.1
```
Default port is 7777. Override with `--port <n>`. Without `--host` or `--connect`, the engine runs in single-player mode.

In host mode, the simulation waits for all expected players (from manifest) before starting. Lua scripts call `EndGame(winner, stats_json)` to end the session.

**Controls (RTS preset):**
- **Left click** — select unit
- **Left drag** — box select
- **Right click ground** — move order
- **Right click enemy** — attack order
- **A + left click** — attack-move (ground) or force-attack (unit)
- **S** — stop, **H** — hold position
- Ability hotkeys come from the map's `abilities.json` `"hotkey"` field — the action_bar surfaces them automatically
- **Ctrl+0-9** — assign control group, **0-9** — recall
- **F1-F3** — select hero 1-3
- **Arrow keys** — pan camera
- **Scroll wheel** — zoom

## Troubleshooting

- **"Failed to create Vulkan instance"** — Vulkan SDK not installed, or GPU drivers too old. Run `vulkaninfo` to check.
- **Build hangs during configure** — FetchContent is downloading dependencies. Wait for it to finish.
- **build.ps1 says "No Visual Studio installation found"** — Install VS with the C++ workload.
- **CMake errors about missing packages** — Delete the `build/` directory and rebuild.

## Project Structure

```
src/
├── core/           Types, logging, math, allocators
├── platform/       Window, input, filesystem (Win32 / Android)
├── rhi/            GPU abstraction (vulkan/ + gles/ backends)
├── asset/          Resource manager, format loaders (glTF, KTX2, JSON)
├── render/         Pipelines, camera, terrain, animation, particles, effects, world overlays
├── audio/          3D positional audio, music, ambient (miniaudio)
├── script/         Lua 5.4 VM, engine API bindings, triggers
├── simulation/     ECS, units, abilities, pathfinding, combat, projectiles
├── input/          Command system, selection, picker, RTS / action presets, hotkey bindings
├── hud/            Custom retained-mode in-game UI (atoms, composites, world overlays, text tags)
├── shell/          RmlUi-backed around-game UI (menus, lobby, loading, results) — game builds only
├── network/        GameServer, NetworkManager, ENet transport
├── map/            Map format, terrain data, scene loader
├── editor/         Map editor executable — terrain tools, cliff/ramp editing
├── tools/          Build-time CLI utilities (uldum_pack, uldum_gen_overlays)
├── server/         Headless server-side binaries (uldum_worker, future uldum_server orchestrator)
└── app/            Client / editor entry points (uldum_dev, uldum_game, android_main)
```

## Documentation

- [docs/design.md](docs/design.md) — Full technical design and phase roadmap
- [docs/gameplay-model.md](docs/gameplay-model.md) — Game object hierarchy, components, unit/item/destructable model
- [docs/ability-system.md](docs/ability-system.md) — Ability system, slots, hotkeys, scripting
- [docs/items.md](docs/items.md) — Item system: types, inventory, charges/level fields, pickup/use lifecycle
- [docs/input-system.md](docs/input-system.md) — Input system, commands, ability slots, Lua hooks
- [docs/scripting.md](docs/scripting.md) — Lua scripting API, triggers, timers, events
- [docs/map-system.md](docs/map-system.md) — Map format, engine vs map boundary, asset split
- [docs/terrain.md](docs/terrain.md) — Terrain system, heightmap, cliffs, ramps, pathing
- [docs/audio.md](docs/audio.md) — Audio system design
- [docs/effects.md](docs/effects.md) — Particle and effect system
- [docs/model-format.md](docs/model-format.md) — Model, animation, texture specifications
- [docs/network.md](docs/network.md) — Network protocol, transport, state sync, interpolation
- [docs/coordinates.md](docs/coordinates.md) — Coordinate system and unit conventions
- [docs/build-targets.md](docs/build-targets.md) — Build targets and packaging
- [docs/packaging.md](docs/packaging.md) — `.uldpak` / `.uldmap` archive format, mounts, texture baking
- [docs/editor.md](docs/editor.md) — Editor modes (normal / source-folder), authoring lifecycle
- [docs/ui.md](docs/ui.md) — UI system: Shell UI (RmlUi) vs HUD (custom, Lua-driven)

## License

TBD
