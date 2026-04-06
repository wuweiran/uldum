# Uldum Engine

A unit-centric game engine inspired by Warcraft III, built with modern C++23 and Vulkan 1.3.

## Features (planned)

- **Vulkan 1.3 rendering** — render graph, GPU-driven rendering, bindless resources
- **Skeletal animation** — GPU skinning, animation state machine, glTF models
- **Particle effects** — CPU-driven billboard particles, named effect system
- **Unit-centric gameplay** — ECS internally, unit facade for scripting
- **Lua 5.4 scripting** — trigger/event system, sandboxed map scripts
- **Map system** — heightmap terrain, object placement, asset overrides
- **Server-authoritative multiplayer** — with single player via in-process local server
- **Terrain editor** — in-engine ImGui-based editor (sculpt, paint, pathing, objects)
- **Cross-platform** — Windows and Android

## Current Status

Phase 10 complete. Terrain editor.

**What works:**
- Win32 window + Vulkan 1.3 rendering (dynamic rendering, synchronization2)
- VMA for GPU memory, GLSL shaders compiled to SPIR-V at build time
- Asset pipeline: PNG textures (stb_image), glTF models with skeleton + animation (cgltf), JSON configs (nlohmann/json)
- Handle-based AssetManager with path caching + absolute path loading
- ECS simulation: sparse set storage, generational typed handles (Unit, Destructable, Item)
- Map system: `.uldmap` packages with manifest, types, scenes, scripts, assets
  - Manifest: metadata, players, teams, alliances, map-defined enumerations
  - Type loading from map's `types/` directory (no engine gameplay data)
  - Scene: terrain + object placements (units, destructables, items, regions, cameras)
- WC3-scale coordinates (1 tile = 128 game units, melee range 128, speed ~270)
- Generic state system (HP engine-built-in, mana/energy/etc. map-defined)
- Generic attribute system (string-based: armor, attack_type, strength, etc. all map-defined)
- String-based classifications, attack/armor types (map declares valid values in manifest)
- Alliance system: per-player-pair, asymmetric (allied, passive), loaded from manifest teams
- A* pathfinding on tile grid with 8-directional movement
- Movement with committed-side local avoidance (steer around other units)
- Hard collision resolution (prevents unit overlap)
- Spatial grid for efficient proximity queries
- Combat system: attack state machine, auto-acquire within acquire_range, attack-move
- Unified damage pipeline with damage types (map-defined), on_damage event with SetDamageAmount
- Death system with on_death callback, corpse lifecycle
- Projectile system: flight, homing, hit detection, impact damage
- Ability system: passive, aura, instant, target_unit, target_point, toggle, channel
  - Cast order system: IssueOrder(unit, "cast", ability_id, target) with state machine
  - Aura scanning, modifier system, stackable abilities
- Skeletal animation (Phase 9):
  - GPU skinning via vertex shader + per-entity bone SSBO
  - glTF skeleton/animation extraction (joints, weights, inverse bind matrices, keyframes)
  - Animation state machine: idle, walk, attack, spell, death (clips bound by name)
  - Playback speed syncs to gameplay timing (attack cooldown, movement speed)
  - Crossfade blending between states
  - Skinned shadow pass (shadows match animation pose)
  - Procedural 2-bone test skeleton for development without real models
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
  - Attribute API: GetUnitAttribute, SetUnitAttribute
  - Proper Unit/Player usertypes with == operator, nil for invalid handles
  - Context functions with wrong-event warnings
- Textured mesh pipeline with descriptor sets, samplers, and diffuse texture binding
- Terrain splatmap rendering: 4 ground texture layers blended per tile
- Shadow mapping: 2048x2048 depth pass, 3x3 PCF filtering, depth bias
- Directional lighting with world-space normals and shadows
- Game coordinates: X=right, Y=forward, Z=up
- Terrain editor (Phase 10):
  - Separate `uldum_editor.exe` build target with ImGui (Dear ImGui + Vulkan + Win32)
  - Heightmap sculpting: raise, lower, smooth, flatten brushes
  - Texture painting: splatmap with 4 ground layers
  - Cliff level editing: sheer terraces with smoothed diagonal edges
  - Ramp placement: slopes between adjacent cliff levels
  - Water placement: per-tile water height
  - Pathing editing: toggle walkable flag per vertex
  - Tile-based brush with vertex indicator and grid overlay
  - Save/load terrain.bin (binary format)
  - Open Map folder picker, scene switching
  - DPI-aware UI scaling
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

Output: `build/bin/uldum_dev.exe` (developer runtime), `build/bin/uldum_editor.exe` (map editor), `build/bin/uldum_game.exe` (shipped game), `build/bin/uldum_server.exe` (dedicated server). See [docs/build-targets.md](docs/build-targets.md) for details.

## Run

```cmd
cd build\bin
uldum_dev.exe
```

The engine loads the test map from `maps/test_map.uldmap/` automatically. The test scene spawns creep waves attacking two hero units with abilities (cleave, consecration, holy light).

**Controls:**
- **WASD** — move camera on the ground plane
- **Q/E** — lower/raise camera height
- **Escape** — quit

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
├── audio/          3D positional audio (miniaudio) [stub]
├── script/         Lua 5.4 VM, engine API bindings, triggers
├── simulation/     ECS, units, abilities, pathfinding, combat, AI
├── network/        Server-authoritative multiplayer (ENet) [stub]
├── map/            Map format, terrain data, overrides
├── editor/         Map editor executable — terrain tools, object placement [stub]
└── app/            Engine core + entry points (dev, game, server)
```

## Documentation

- [docs/design.md](docs/design.md) — Full technical design and roadmap
- [docs/model-format.md](docs/model-format.md) — Model, animation, texture, and effect specifications

## License

TBD
