# Uldum Engine

A unit-centric game engine inspired by Warcraft III, built from scratch with modern C++ and Vulkan.

## Project Overview

Uldum is a game engine for map-based, unit-centric games with Lua scripting, multiplayer support, and an in-engine terrain editor. Think Warcraft III's engine rebuilt with modern technology and zero legacy burden.

## Tech Stack

- **Language**: C++23 (MSVC on Windows, Clang/NDK on Android)
- **Build**: CMake
- **Rendering**: Vulkan 1.3 (rasterization, render graph, GPU-driven, bindless)
- **Scripting**: Lua 5.4 with sol2 bindings
- **Networking**: Server-authoritative with ENet (UDP). Single player uses in-process local server.
- **Audio**: miniaudio
- **Platforms**: Windows, Android

## Architecture

### Modules (all under `src/`)

| Module | Purpose | Dependencies |
|--------|---------|--------------|
| `core` | Types, math, logging, allocators, profiling | None |
| `platform` | Window, input, filesystem (Win32 / GameActivity) | core |
| `rhi` | Vulkan 1.3 abstraction (device, swapchain, commands) | core, platform |
| `render` | Render graph, materials, terrain, animation, particles | core, rhi, asset |
| `simulation` | ECS, units, abilities, pathfinding, collision, AI | core, script |
| `script` | Lua 5.4 VM, engine API bindings, trigger system | core |
| `map` | Map format, terrain data, object placement, overrides | core, asset |
| `network` | Server-authoritative sim, client prediction, state sync | core, simulation |
| `audio` | 3D positional audio, SFX, music streaming | core, asset |
| `asset` | Resource manager, format loaders (glTF, KTX2, OGG) | core |
| `editor` | In-engine terrain editor (ImGui) | core, render, simulation, map |
| `app` | Entry point, main loop, game state machine | All |

### Asset Split

- **Engine assets** (`engine/`): shipped with engine, read-only. Base unit/ability types, shaders, default textures, core Lua scripts.
- **Map assets** (`*.uldmap/`): per-map, modifiable. Terrain, preplaced objects, map scripts, custom models/textures. Maps can override engine definitions.

### Key Design Decisions

- **Server-authoritative networking** (not lockstep). No fixed-point math. Regular floating point.
- **Custom platform layer** — no SDL. Win32 on Windows, GameActivity on Android.
- **One format per asset type**: glTF (models + skeletal animation), KTX2 + Basis Universal (textures — authored externally via `toktx`), OGG (audio), GLSL (shaders), JSON (config), FlatBuffers (binary serialization). See `docs/model-format.md` for full art spec.
- **Hybrid ECS + unit-centric API**: ECS internally for performance, unit facade for scripting.
- **Deterministic fixed timestep** for simulation (e.g. 16 ticks/sec), decoupled from render.

## Conventions

- Use `#pragma once` for include guards.
- Use C++23 features: `std::expected`, `std::format`, `std::span`, structured bindings, etc.
- Namespace: `uldum::` for all engine code, sub-namespaces per module (e.g. `uldum::rhi`, `uldum::core`).
- Error handling: `std::expected` for fallible functions, assertions for invariants. No exceptions.
- Naming: `snake_case` for functions/variables/files, `PascalCase` for types/classes, `UPPER_CASE` for constants/macros.
- Prefer value semantics. Use `std::unique_ptr` for ownership, raw pointers for non-owning references.
- No `using namespace std;` in headers.

## Third-Party Libraries

VMA, shaderc, sol2, Lua 5.4, ENet, miniaudio, Dear ImGui, Tracy, glm, cgltf, stb_image, stb_vorbis, FlatBuffers, nlohmann/json.

## Build

Requires Vulkan SDK installed and MSVC (Visual Studio Build Tools).

**Windows (via build script):**
```powershell
scripts\build.ps1
```
This imports the MSVC x64 environment (via `vcvarsall.bat x64`) into the PowerShell session, then runs CMake configure + build.

**Or manually from a Developer Command Prompt:**
```cmd
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Output binary: `build/bin/uldum.exe`
