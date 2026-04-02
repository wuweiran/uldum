# Uldum Engine

A unit-centric game engine inspired by Warcraft III, built with modern C++23 and Vulkan 1.3.

## Features (planned)

- **Vulkan 1.3 rendering** — render graph, GPU-driven rendering, bindless resources
- **Unit-centric gameplay** — ECS internally, unit facade for scripting
- **Lua 5.4 scripting** — trigger/event system, sandboxed map scripts
- **Map system** — heightmap terrain, object placement, asset overrides
- **Server-authoritative multiplayer** — with single player via in-process local server
- **Terrain editor** — in-engine ImGui-based editor (sculpt, paint, pathing, objects)
- **Cross-platform** — Windows and Android

## Current Status

Phase 7d complete. Ability system with passives, auras, modifiers, and cast flow infrastructure.

**What works:**
- Win32 window + Vulkan 1.3 rendering (dynamic rendering, synchronization2)
- VMA for GPU memory, GLSL shaders compiled to SPIR-V at build time
- Asset pipeline: PNG textures (stb_image), glTF models (cgltf), JSON configs (nlohmann/json)
- Handle-based AssetManager with path caching + absolute path loading
- ECS simulation: sparse set storage, generational typed handles (Unit, Destructable, Item)
- Map system: `.uldmap` packages with manifest, types, scenes, scripts, assets
  - Manifest: metadata, players, teams, map-defined enumerations
  - Type loading from map's `types/` directory (no engine gameplay data)
  - Scene: terrain + object placements (units, destructables, items, regions, cameras)
  - Units placed on terrain surface automatically
- Generic state system (HP engine-built-in, mana/energy/etc. map-defined)
- Generic attribute system (string-based: armor, attack_type, strength, etc. all map-defined)
- String-based classifications, attack/armor types (map declares valid values in manifest)
- A* pathfinding on tile grid with 8-directional movement
- MovementSystem: path following, steering, turn rate, terrain height placement
- Terrain slope tilt: entities visually align to terrain surface normal
- Move orders from map placement data (objects.json `move_to` field)
- Spatial grid for efficient proximity queries (units_in_range, units_in_rect, nearest_unit)
- UnitFilter for query filtering (owner, enemy_of, classifications, alive_only, etc.)
- Unit-unit collision avoidance (separation steering)
- Buildings block pathing tiles (units pathfind around them)
- Combat system: attack state machine (chase, turn, wind-up, fire, backswing, cooldown)
- Damage + death: HP decreases on hit, entity destroyed at 0 HP
- Projectile system: flight, homing, hit detection, impact damage
- Visual feedback: scale pulse on attack, units shrink as HP decreases, corpse turns dark gray
- Ability system: AbilityDef (JSON templates) + Ability instances on units
  - Forms: passive, aura, instant, target_unit, target_point, toggle, channel
  - AddAbility, RemoveAbility, ApplyPassiveAbility API
  - Aura scanning at uniform 0.25s interval, applies passive buffs to nearby allies
  - Modifier system: abilities apply attribute modifiers, recalculated on add/remove
  - Stackable flag on ability defs (items stack, buffs refresh)
  - Cast flow skeleton for active abilities (validation, cost, timing — effects via Lua in Phase 8)
- Textured mesh pipeline with descriptor sets, samplers, and diffuse texture binding
- Terrain splatmap rendering: 4 ground texture layers blended per tile via RGBA splatmap
- Shadow mapping: 2048x2048 depth pass from light perspective, 3x3 PCF filtering, depth bias
- Directional lighting with world-space normals and shadows
- Game coordinates: X=right, Y=forward, Z=up (documented in docs/coordinates.md)
- Build output is self-contained (engine + maps copied to output directory)

## Prerequisites

Install these before building:

1. **Visual Studio 2022 or later** (Community edition is free)
   - During installation, select the **"Desktop development with C++"** workload
   - This provides the MSVC compiler, Windows SDK, and CMake integration
   - Download: https://visualstudio.microsoft.com/

2. **Vulkan SDK 1.3.x**
   - Download and install from https://vulkan.lunarg.com/sdk/home
   - The installer sets `VULKAN_SDK` environment variable automatically
   - Verify: open a terminal and run `vulkaninfo` — it should print your GPU info

3. **CMake 3.28+**
   - Usually included with Visual Studio, but can be installed separately
   - Download: https://cmake.org/download/
   - Verify: `cmake --version`

4. **GPU with Vulkan 1.3 support**
   - Most GPUs from 2018+ support Vulkan 1.3
   - Update your GPU drivers to the latest version

## Build (Windows)

Using the build script (auto-detects Visual Studio installation):

```cmd
scripts\build.bat
```

Or manually from a **Developer Command Prompt for VS**:

```cmd
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Output: `build/bin/uldum.exe`

## Run

After building, run the engine from the build output directory:

```cmd
cd build\bin
uldum.exe
```

The engine loads the test map from `maps/test_map.uldmap/` automatically. You should see a terrain with units moving on it.

**Controls:**
- **WASD** — move camera on the ground plane
- **Q/E** — lower/raise camera height
- **Escape** — quit

## Troubleshooting

- **"Failed to create Vulkan instance"** — Vulkan SDK not installed, or GPU drivers too old. Run `vulkaninfo` to check.
- **"No Vulkan-capable GPU found"** — GPU doesn't support Vulkan 1.3. Update drivers.
- **build.bat says "No Visual Studio installation found"** — Install VS with the C++ workload. The script uses `vswhere.exe` to find VS automatically.
- **CMake errors about missing packages** — Delete the `build/` directory and rebuild. Third-party dependencies are fetched via CMake FetchContent on first configure.

## Project Structure

```
src/
├── core/           Types, logging, math, allocators
├── platform/       Window, input, filesystem (Win32 / Android)
├── rhi/            Vulkan 1.3 abstraction
├── asset/          Resource manager, format loaders
├── render/         Render graph, materials, terrain, animation
├── audio/          3D positional audio (miniaudio)
├── script/         Lua 5.4 VM, engine API bindings, triggers
├── simulation/     ECS, units, abilities, pathfinding, AI
├── network/        Server-authoritative multiplayer (ENet)
├── map/            Map format, terrain data, overrides
├── editor/         In-engine terrain editor (ImGui)
└── app/            Entry point, engine main loop
```

See [docs/design.md](docs/design.md) for the full technical design.

## License

TBD
