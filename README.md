# Uldum Engine

A unit-centric game engine inspired by Warcraft III, built with modern C++23 and Vulkan 1.3.

## Features (planned)

- **Vulkan 1.3 rendering** — render graph, GPU-driven rendering, bindless resources
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

Phase 15a complete. Build targets.

**What works:**
- Win32 window + Vulkan 1.3 rendering (dynamic rendering, synchronization2)
- VMA for GPU memory, GLSL shaders compiled to SPIR-V at build time
- Asset pipeline: PNG textures (stb_image), glTF models with skeleton + animation (cgltf), JSON configs (nlohmann/json)
- Handle-based AssetManager with path caching + absolute path loading
- Real glTF model loading: mesh merging, embedded texture extraction, per-model material
- ECS simulation: sparse set storage, generational typed handles (Unit, Destructable, Item)
- Map system: `.uldmap` packages with manifest, types, scenes, scripts, assets
  - Manifest: metadata, players, teams, alliances, map-defined enumerations
  - Type loading from map's `types/` directory (no engine gameplay data)
  - Scene: terrain + object placements (units, destructables, items, regions, cameras)
- WC3-scale coordinates (1 tile = 128 game units, melee range 128, speed ~270)
- WC3 facing convention (0 = +X, 90 = +Y, degrees in JSON/Lua, radians internally)
- Generic state system (HP engine-built-in, mana/energy/etc. map-defined)
- Generic attribute system (string-based: armor, attack_type, strength, etc. all map-defined)
- String-based classifications, attack/armor types (map declares valid values in manifest)
- Alliance system: per-player-pair, asymmetric (allied, passive, shared_vision), loaded from manifest teams
- Fog of war: per-player tile-based visibility with three modes (none, explored, unexplored)
  - Cliff-based line-of-sight blocking (high ground sees low, cliffs block vision)
  - Shared vision via alliance flags (per-team configurable)
  - Visual smoothing: temporal fade (fast reveal, slow fade), feathered vision edges, bilinear filtering
  - Per-frame GPU fog texture, terrain shader darkening, enemy unit/shadow culling
  - Lua API: reveal, unexplore, visibility queries
- A* pathfinding on tile grid with 8-directional movement
- Movement with committed-side local avoidance (steer around other units)
- Per-unit collision radius (configurable per type in JSON)
- Hard collision resolution (prevents unit overlap)
- Spatial grid for efficient proximity queries
- Combat system: attack state machine, auto-acquire within acquire_range, attack-move
  - Attack facing tolerance (~10 degrees, like WC3)
  - Backswing completes even when target dies (cancelable by player orders)
  - Continuous attack: no idle gap between swings against same target
- Unified damage pipeline with damage types (map-defined), on_damage event with SetDamageAmount
- Death system with on_death callback, corpse lifecycle
- Projectile system: flight, homing, hit detection, impact damage
- Ability system: passive, aura, instant, target_unit, target_point, toggle, channel
  - Cast order system: IssueOrder(unit, "cast", ability_id, target) with state machine
  - Aura scanning, modifier system, stackable abilities
- Skeletal animation (Phase 9):
  - GPU skinning via vertex shader + per-entity bone SSBO
  - glTF skeleton/animation extraction (joints, weights, inverse bind matrices, keyframes)
  - Rest pose support for correct bind pose rendering
  - Animation state machine: idle, walk, attack, spell, death, birth (clips bound by name)
  - Playback speed syncs to gameplay timing (attack cooldown, movement speed)
  - Crossfade blending between all state transitions
  - Skinned shadow pass (shadows match animation pose)
- Effect system (Phase 9):
  - Named effects: engine defaults + map-defined (Lua or JSON)
  - Lifecycle: CreateEffect (persistent) / PlayEffect (fire-and-forget) / DestroyEffect
  - Unit-attached effects that follow the unit
  - CPU-driven billboard particles with alpha blending
  - Growable descriptor pool (auto-allocates new pools on demand)
- Lua 5.4 scripting with sol2 bindings
  - Sandboxed VM (no filesystem access)
  - Trigger system with 3 priority levels (High, Normal, Low)
  - Timer system: CreateTimer with repeating/one-shot support
  - Full engine API: unit CRUD, orders, abilities, damage, spatial queries, hero, player
  - Alliance API: SetAlliance, IsPlayerAlly, IsPlayerEnemy
  - Effect API: DefineEffect, CreateEffect, PlayEffect, DestroyEffect
  - Audio API: PlaySound, PlayMusic, StopMusic, PlayAmbientLoop, SetVolume
  - Attribute API: GetUnitAttribute, SetUnitAttribute
  - Proper Unit/Player usertypes with == operator, nil for invalid handles
  - Context functions with wrong-event warnings
- Textured mesh pipeline with descriptor sets, samplers, and diffuse texture binding
- Terrain splatmap rendering: 4 ground texture layers blended per tile
- Shadow mapping: 2048x2048 depth pass, 3x3 PCF filtering, depth bias, normal offset bias
- Directional lighting with world-space normals and shadows
- Render interpolation: smooth 60fps visuals from 32Hz simulation tick
- Game coordinates: X=right, Y=forward, Z=up
- Terrain editor (Phase 10):
  - Separate `uldum_editor.exe` build target with ImGui (Dear ImGui + Vulkan + Win32)
  - Heightmap sculpting: raise, lower, smooth, flatten brushes
  - Texture painting: splatmap with 4 ground layers
  - Cliff level editing: sheer terraces with smoothed diagonal edges (midpoint cutting)
  - Cliff constraint enforcement: max 1 level difference between adjacent vertices
  - Per-edge ramp-aware cliff smoothing (no gaps at ramp/cliff boundaries)
  - Diagonal cliff case (saddle points) with proper wall geometry
  - Ramp placement: slopes between adjacent cliff levels (auto-validated)
  - Water placement: per-tile water as splatmap layer
  - Pathing editing: toggle walkable flag per vertex
  - WC3-style brush sizing (1 = single vertex)
  - Tile-based brush with vertex indicator and grid overlay
  - Save/load terrain.bin (binary format)
  - Open Map folder picker, scene switching
  - DPI-aware UI scaling
- Audio system (Phase 11):
  - miniaudio high-level engine (ma_engine) with 3D spatialization
  - 5 volume channels: Master, SFX, Music, Ambient, Voice (sound groups)
  - SFX: 3D positioned and 2D fire-and-forget sounds, auto-cleanup
  - Music: streaming playback with looping and crossfade between tracks
  - Ambient: 3D looping sounds with fade-out
  - Listener updated from camera position each frame
  - Animation-driven sounds: attack, death, birth sounds defined per unit type in JSON
  - Sound callback system: simulation fires events, audio engine plays them (no coupling)
  - Path resolution: map shared/assets first, then engine root
  - Lua API: PlaySound, PlaySoundAtPoint, PlaySound2D, PlayMusic, StopMusic, PlayAmbientLoop, StopAmbientLoop, SetVolume
- Input system (Phase 12):
  - Command system: GameCommand struct, ownership validation, Lua on_order filter
  - Selection state: click, box drag, shift-toggle, control groups (Ctrl+0-9), hero select (F1-F3)
  - Picker: ray-cylinder intersection for unit picking, screen-to-world terrain raycast
  - RTS input preset: WC3-style controls (select, smart order, attack-move, ability hotkeys)
  - InputBindings: configurable action-to-key mapping with JSON overrides from manifest
  - Preset factory: map manifest selects input preset (`"input_preset": "rts"`)
  - Ability hotkeys: per-ability `"hotkey"` field, RTS preset scans selected unit's abilities
  - Ability `"hidden"` field for system-level passives (no UI slot)
  - Commands vs ability slots design: built-in commands (attack, move, stop, hold) separate from ability slots
  - Lua selection API: GetSelectedUnits, SelectUnit, ClearSelection, IsUnitSelected
  - Lua events: on_order (cancellable via CancelOrder), on_select
  - Platform: key_letter[26] array for arbitrary A-Z key detection
- Networking — local server (Phase 13a):
  - GameServer class: owns Simulation + ScriptEngine (server-side state)
  - Two-phase init: simulation before map load, game logic after
  - Zero-overhead local play: tick() is a direct function call, no threading or IPC
  - Clean client/server boundary: Engine holds GameServer + client modules (renderer, audio, input)
- Networking — multiplayer (Phase 13b):
  - Transport abstraction (ENet UDP, swappable for QUIC/TCP later)
  - Binary protocol: commands (client → server), state snapshots (server → client)
  - Server broadcasts fog-filtered state at 32 Hz, client interpolates between snapshots
  - Host mode (`--host`) and connect mode (`--connect <ip>`)
- Session management (Phase 13c):
  - `--map <path>` CLI arg for map selection
  - Server waits for expected players before starting simulation
  - Synchronized start: `S_START` broadcast when all players connected
  - Lua `EndGame(winner, stats_json)` → `on_game_end` event → `S_END` broadcast
- Reconnect (Phase 13d):
  - Disconnected player's state kept alive during configurable timeout
  - Reconnecting client receives full state snapshot to catch up
  - Optional game pause on disconnect (configurable per map)
  - Timeout fires `on_player_dropped` Lua event, client disconnect transitions to Results
- GPU-driven rendering (Phase 14a/b):
  - Instance SSBO + indirect draw for all static meshes
  - Mega vertex/index buffer — all static meshes share one VB+IB
  - Bindless texture array (VK_EXT_descriptor_indexing) — per-instance material index
  - Single multi-draw-indirect call for all static geometry
  - CPU frustum culling: bounding sphere vs camera frustum, skips off-screen entities
  - 4x MSAA anti-aliasing (multisampled color + depth, resolve to swapchain)
- Water rendering (Phase 14d/e):
  - Tileset-driven terrain textures (sampler2DArray, 256x256)
  - SDF curve-based terrain type transitions with noise perturbation
  - Terrain normal maps (UNORM format)
  - Shallow water: transparent surface, riverbed visible, Fresnel edge glow
  - Deep water: opaque with Gerstner wave normals + noise-driven shade
  - Smooth shallow/deep transition via `deep_blend`
  - Shore shrink for shallow water edges
  - Cubemap environment reflection on water surface
  - Splash particles on units walking through shallow water (fog-aware)
  - Pathfinding: ground units walk on shallow water, blocked by deep water
- Skybox and environment lighting (Phase 14f):
  - Map-defined cubemap skybox (6 PNG faces, optional)
  - EXR-to-cubemap converter script (`scripts/convert_skybox.py`)
  - Environment config in manifest.json (sun direction/color, ambient, fog)
  - Environment UBO replacing hardcoded lighting in all shaders
  - Dynamic point light system (8 lights, quadratic falloff)
  - Glow particle effects cast point lights on surrounding surfaces
  - Procedural particle shapes: soft circle, spark, blood splatter, glow beam, water droplet
  - Lua API: SetSunDirection, AddPointLight
- Build targets (Phase 15a):
  - `uldum_dev` — development runtime (debug, auto-starts map)
  - `uldum_game` — shipped product (reads game.json config, no debug)
  - `uldum_server` — headless dedicated server (no renderer/audio/window)
  - `uldum_editor` — in-engine terrain editor (ImGui)
  - `game.json` — product configuration (name, default map, port, resolution)
  - Build scripts: `build.bat`, `build_all.bat`, `build_game.bat`, `build_server.bat`, `build_editor.bat`
  - Test scripts: `test_multiplayer.bat`, `test_server.bat`
- Ninja build system for parallel compilation

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

```cmd
scripts\build.bat
```

First build fetches third-party dependencies via CMake FetchContent (takes ~1 min). Subsequent builds are incremental.

Output: `build/bin/uldum_dev.exe` (developer runtime), `build/bin/uldum_editor.exe` (map editor). See [docs/build-targets.md](docs/build-targets.md) for details.

## Run

```cmd
cd build\bin
uldum_dev.exe
```

The engine loads the test map from `maps/test_map.uldmap/` automatically. The test scene spawns units with real glTF models, skeletal animation, combat, and abilities.

**Multiplayer (LAN):**
```cmd
:: Terminal 1 — host
uldum_dev.exe --host

:: Terminal 2 — join
uldum_dev.exe --connect 127.0.0.1
```
Default port is 7777. Override with `--port <n>`. Specify map with `--map <path>` (default: `maps/test_map.uldmap`). Without `--host` or `--connect`, the engine runs in single-player mode (same as before).

In host mode, the simulation waits for all expected players (from manifest) before starting. Lua scripts call `EndGame(winner, stats_json)` to end the session.

**Controls (RTS preset):**
- **Left click** — select unit
- **Left drag** — box select
- **Right click ground** — move order
- **Right click enemy** — attack order
- **A + left click** — attack-move (ground) or force-attack (unit)
- **S** — stop, **H** — hold position
- **T** — Holy Light (test map paladin ability hotkey)
- **Ctrl+0-9** — assign control group, **0-9** — recall
- **F1-F3** — select hero 1-3
- **Arrow keys** — pan camera
- **Scroll wheel** — zoom

## Troubleshooting

- **"Failed to create Vulkan instance"** — Vulkan SDK not installed, or GPU drivers too old. Run `vulkaninfo` to check.
- **Build hangs during configure** — FetchContent is downloading dependencies. Wait for it to finish.
- **build.bat says "No Visual Studio installation found"** — Install VS with the C++ workload.
- **CMake errors about missing packages** — Delete the `build/` directory and rebuild.

## Project Structure

```
src/
├── core/           Types, logging, math, allocators
├── platform/       Window, input, filesystem (Win32 / Android)
├── rhi/            Vulkan 1.3 abstraction
├── asset/          Resource manager, format loaders (glTF, PNG, JSON)
├── render/         Pipelines, camera, terrain, animation, particles, effects
├── audio/          3D positional audio, music, ambient (miniaudio)
├── script/         Lua 5.4 VM, engine API bindings, triggers
├── simulation/     ECS, units, abilities, pathfinding, combat, AI
├── network/        GameServer, NetworkManager (ENet transport pending)
├── map/            Map format, terrain data, overrides
├── editor/         Map editor executable — terrain tools, cliff/ramp editing
└── app/            Engine core + entry points (dev, editor)
```

## Documentation

- [docs/design.md](docs/design.md) — Full technical design and phase roadmap
- [docs/gameplay-model.md](docs/gameplay-model.md) — Game object hierarchy, components, unit/item/destructable model
- [docs/ability-system.md](docs/ability-system.md) — Ability system, slots, hotkeys, scripting
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

## License

TBD
