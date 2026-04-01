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

Phase 5b complete. The engine renders 3D objects from ECS entities with a movable camera.

**What works:**
- Win32 window + Vulkan 1.3 rendering (dynamic rendering, synchronization2)
- VMA for GPU memory, GLSL shaders compiled to SPIR-V at build time
- Asset pipeline: PNG textures (stb_image), glTF models (cgltf), JSON configs (nlohmann/json)
- Handle-based AssetManager with path caching
- ECS simulation: sparse set storage, generational typed handles (Unit, Destructable, Item)
- Data-driven unit types loaded from JSON (TypeRegistry)
- 3D mesh pipeline with MVP push constants, camera with WASD+QE movement
- Renderer iterates ECS World (Transform + Renderable) and draws entities
- Game coordinates: X=right, Y=forward, Z=up
- Build output is self-contained (engine assets copied to output directory)

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
