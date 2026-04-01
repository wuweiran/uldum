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

Phase 6 complete. The engine loads self-contained map packages with types, terrain, and preplaced objects.

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
- Textured mesh pipeline with descriptor sets, samplers, and diffuse texture binding
- Terrain splatmap rendering: 4 ground texture layers blended per tile via RGBA splatmap
- Shadow mapping: 2048x2048 depth pass from light perspective, 3x3 PCF filtering, depth bias
- Directional lighting with world-space normals and shadows
- Game coordinates: X=right, Y=forward, Z=up (documented in docs/coordinates.md)
- Build output is self-contained (engine + maps copied to output directory)

## Requirements

- **C++23 compiler** — MSVC 19.50+ (Visual Studio 2026) or Clang 18+
- **Vulkan SDK** — 1.3.x ([LunarG](https://vulkan.lunarg.com/))
- **CMake** — 3.28+

## Build (Windows)

From a **Developer Command Prompt** or using the build script:

```cmd
scripts\build.bat
```

Or manually:

```cmd
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Output: `build/bin/uldum.exe`

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
