# Uldum Engine — Technical Design

## 1. Vision

A modern game engine for unit-centric, map-based games with scripting and multiplayer — inspired by Warcraft III's architecture, built with zero legacy burden.

## 2. Target Platforms

- Windows (Win32 + Vulkan)
- Android (GameActivity + Vulkan)

## 3. Module Architecture

```
uldum/
├── core/           # Allocators, containers, math (glm), logging, profiling (Tracy)
├── platform/       # Custom platform layer
│   ├── windows/    #   Win32 window, input, filesystem
│   └── android/    #   GameActivity, touch input, APK filesystem
├── rhi/            # Thin Vulkan 1.3 abstraction
│   └── vulkan/     #   Instance, device, swapchain, command buffers, sync
├── render/         # Render graph, materials, terrain, skeletal anim, particles, UI
├── simulation/     # ECS, units, abilities, buffs, pathfinding, collision, AI
├── script/         # Lua 5.4 VM, sol2 bindings, trigger/event system
├── map/            # Map format (FlatBuffers), terrain data, object placement
├── network/        # Server-authoritative: server sim, client prediction, state sync
├── audio/          # miniaudio: 3D positional, SFX, music streaming
├── asset/          # Resource manager, glTF/PNG/OGG loaders, async loading
├── editor/         # In-engine terrain editor (ImGui): sculpt, paint, place, pathing
└── app/            # Entry point, main loop, game state machine
```

## 4. Core Module

Provides foundational utilities used by all other modules.

- **Types**: Fixed-width aliases (`u8`, `u16`, `u32`, `u64`, `i8`, `i16`, `i32`, `i64`, `f32`, `f64`)
- **Logging**: Level-based logging (trace, debug, info, warn, error) using `std::format`
- **Math**: glm for rendering math. Standard float for all gameplay (no fixed-point).
- **Memory**: Custom allocators (linear, pool) for hot paths — future work
- **Profiling**: Tracy integration — future work

## 5. Platform Module

Abstracts OS-specific window management, input, and filesystem.

### Interface

```cpp
namespace uldum::platform {

struct Config {
    const char* title;
    u32 width;
    u32 height;
};

class Platform {
public:
    virtual ~Platform() = default;
    virtual bool init(const Config& config) = 0;
    virtual void shutdown() = 0;
    virtual bool poll_events() = 0;  // false = quit requested
    virtual void* native_window_handle() const = 0;
    virtual void* native_instance_handle() const = 0;
    static std::unique_ptr<Platform> create();
};

}
```

### Windows Implementation

- `CreateWindowExW` for window creation
- `WndProc` message pump for input and lifecycle events
- Returns `HWND` and `HINSTANCE` for Vulkan surface creation

### Android Implementation (future)

- GameActivity from Android Game Development Kit
- Touch input mapping
- APK asset access via AAssetManager

## 6. RHI Module (Vulkan 1.3)

Thin abstraction over Vulkan. Not a generic RHI — Vulkan-specific, but organized cleanly.

### Phase 1 (minimal)

- Vulkan instance + validation layers (debug)
- Physical device selection (prefer discrete GPU)
- Logical device + graphics/present queue
- Swapchain (FIFO present mode)
- Per-frame command buffer, semaphores, fences
- Frame loop: acquire → record (clear) → submit → present

### Future

- Render graph with automatic barriers and transient resources
- GPU-driven rendering (indirect draw, compute culling)
- Bindless resources via descriptor indexing
- VMA integration for memory management
- Shader compilation pipeline (shaderc)

## 7. Rendering

### Terrain

- Heightmap-based terrain with tile grid
- Splatmap texturing (blend up to 4 textures per tile)
- Pathing overlay (walkable, flyable, buildable)

### Units

- Skeletal animation with GPU skinning
- Animation state machine
- Selection circles, health bars (billboarded)

### Particles

- Compute-based particle system
- Emitter types: point, sphere, cone, mesh surface

### UI

- Custom retained-mode UI for game HUD
- Dear ImGui for editor and debug overlay

## 8. Simulation

See [gameplay-model.md](gameplay-model.md) for the full object model design, including:
- Entity categories (Unit, Building, Hero, Destructable, Item, Doodad, Projectile)
- Complete component design (Transform, Health, Movement, Combat, Abilities, Buffs, etc.)
- Order system (Move, Attack, Cast, Train, Build, Patrol, etc.)
- Ability system (data-driven definitions, cooldowns, targeting, passive auras)
- Buff system (stat modifiers, periodic effects, duration, stacking)
- Type definition system (JSON-driven, map-overridable)
- ECS implementation (sparse sets, systems, World struct, unit facade API)

### Key Architecture Points

- ECS internally (sparse set per component type, cache-friendly iteration)
- Unit-centric facade API externally (free functions operating on entity IDs)
- Data-driven types: unit/ability/buff definitions loaded from JSON
- Fixed timestep simulation (16 ticks/sec = 62.5ms per tick)
- Render interpolates between sim states for smooth visuals

## 9. Scripting (Lua 5.4)

### Trigger System (WC3-style)

```lua
-- Event → Condition → Action
CreateTrigger("on_unit_death", {
    event = EVENT_UNIT_DIES,
    condition = function(unit)
        return GetUnitType(unit) == "hero"
    end,
    action = function(unit)
        local killer = GetKillingUnit()
        AddHeroExperience(killer, 200)
        DisplayMessage("A hero has fallen!")
    end
})
```

### Sandboxing

- Map scripts run in sandboxed Lua states
- No filesystem access, no os.execute
- Engine API is the only interface to the outside world

## 10. Map System

### Map Package Structure

```
my_map.uldmap/
├── manifest.json       # Metadata, player slots, teams, game mode
├── terrain.bin         # Heightmap, splatmap, pathing grid
├── objects.json        # Preplaced units, doodads, regions, cameras
├── scripts/
│   ├── main.lua        # Map entry point
│   └── ...             # Additional modules
├── models/             # Map-specific glTF models
├── textures/           # Map-specific PNG textures
├── audio/              # Map-specific OGG sounds
└── overrides/
    ├── unit_types.json     # Override/add unit types
    └── ability_types.json  # Override/add abilities
```

### Engine Assets

```
engine/
├── shaders/            # GLSL shaders
├── models/             # Shared base models
├── textures/           # Default terrain tileset, UI textures, icons
├── scripts/            # Core Lua API library
├── audio/              # UI sounds
└── config/
    ├── unit_types.json
    ├── ability_types.json
    ├── buff_types.json
    └── engine.json
```

### Override Mechanism

1. Engine loads base definitions from `engine/config/`
2. Map loads, its `overrides/` merge on top (add or replace entries by ID)
3. Map scripts can further modify at runtime via Lua API

## 11. Networking

### Server-Authoritative Model

- **Server** runs the authoritative simulation
- **Clients** send player commands (move, attack, cast ability)
- **Server** validates commands, advances simulation, broadcasts state deltas
- **Clients** receive state, interpolate/predict for smooth visuals

### Single Player

- Same server simulation runs in-process (no sockets)
- Client calls server functions directly via local interface
- Identical game logic, zero network overhead

### Transport

- ENet for reliable/unreliable UDP channels
- Reliable: commands, chat, game state critical updates
- Unreliable: position interpolation hints, cosmetic state

### Lobby

- Host-based model (one player hosts, or dedicated server)
- Map selection, player slots, team assignment
- Ready check → synchronized game start

## 12. Audio

- miniaudio for cross-platform playback
- 3D positional audio (units, effects) with distance attenuation
- Streaming for music tracks
- Buffered playback for short SFX
- Per-channel volume control (master, music, SFX, voice)

## 13. Editor (Terrain Editor v1)

In-engine tool using Dear ImGui. Activated via editor mode toggle.

### Features

- **Heightmap sculpting**: raise, lower, smooth, flatten brushes
- **Texture painting**: paint terrain textures with brush (splatmap editing)
- **Pathing marking**: mark tiles as walkable, flyable, buildable, unbuildable
- **Object placement**: place/move/delete units, doodads, regions, cameras
- **Save/Load**: serialize to map package format

### Future (beyond v1)

- Full World Editor (separate application)
- Trigger editor (visual scripting)
- Ability editor
- Import/export tools

## 14. Third-Party Libraries

| Library | Version | Purpose | License |
|---------|---------|---------|---------|
| VMA | latest | Vulkan memory management | MIT |
| shaderc | latest | GLSL → SPIR-V compilation | Apache 2.0 |
| sol2 | 3.x | C++ ↔ Lua binding | MIT |
| Lua | 5.4 | Scripting runtime | MIT |
| ENet | 1.3.x | UDP networking | MIT |
| miniaudio | latest | Audio playback | MIT/Public Domain |
| Dear ImGui | latest | Editor/debug UI | MIT |
| Tracy | latest | Profiling | BSD |
| glm | latest | Math (rendering) | MIT |
| cgltf | latest | glTF 2.0 loading | MIT |
| stb_image | latest | PNG/JPEG loading | MIT/Public Domain |
| stb_vorbis | latest | OGG Vorbis decoding | MIT/Public Domain |
| FlatBuffers | latest | Binary serialization | Apache 2.0 |
| rapidjson | latest | JSON parsing | MIT |

## 15. Build Phases

### Phase 1 — Minimal Running Engine

Core loop, Win32 window, Vulkan init, clear screen to a color, input to close window. Proves the foundation works.

### Phase 2 — Full Structure with Stubs

All modules created with interfaces defined, CMake targets wired up, stub implementations. Compiles but most features return early.

### Phase 3 — Core & Asset Foundation ✓

Prerequisite layer that all gameplay and rendering modules depend on.

- **Core enhancements**: custom allocators (linear, pool), generational handle system, ResourcePool
- **Asset manager**: handle-based resource loading with path caching
  - glTF 2.0 model loading (cgltf)
  - PNG texture loading (stb_image)
  - JSON config loading (nlohmann/json)
- **Third-party deps**: glm, stb, cgltf, nlohmann/json via CMake FetchContent

### Phase 4 — Simulation (ECS + Unit Model) ✓

ECS foundation and data-driven unit creation.

- **ECS core**: sparse set component storage, handle allocator, generational typed handles
- **Core components**: Transform, Health, Movement, Combat, Ability, Buff, Vision, Owner, etc.
- **Typed handles**: Unit, Destructable, Item — distinct types wrapping Handle base
- **Unit facade**: create_unit, get_health, is_hero, is_building, issue_order, etc.
- **Unit type definitions**: data-driven types loaded from JSON via TypeRegistry
- **Systems**: health regen, cooldown ticking, buff duration — stubs for complex systems

### Phase 5 — Render Pipeline

Visual feedback — draw entities and terrain. Built progressively in stages.

**5a — Triangle on screen**
- VMA integration for GPU memory management
- Shader pipeline: GLSL → SPIR-V (pre-compiled or shaderc)
- Graphics pipeline: vertex layout, rasterizer state, depth
- Vertex buffer with hardcoded triangle, draw call in frame loop
- Verify: colored triangle rendered on screen

**5b — 3D mesh rendering + camera**
- Camera system: top-down with tilt, zoom, pan (WC3-style), MVP matrices
- GPU mesh upload: vertex/index buffers from asset-loaded glTF data
- 3D projection and basic per-vertex coloring
- Verify: test_triangle.gltf rendered in 3D, camera can move

**5c — Terrain + lighting**
- Heightmap terrain mesh generation (procedural or from data)
- Basic directional lighting (Blinn-Phong or similar)
- Depth buffer and proper 3D sorting
- Verify: 3D terrain surface with shading

**5d — Textures + materials**
- Texture GPU upload from asset-loaded PNG data
- Sampler and descriptor set management
- Material system: shader + texture bindings
- Terrain splatmap texturing (blend up to 4 textures per tile)
- Verify: textured terrain

**5e — Animation + particles (can defer)**
- Skeletal animation: GPU skinning, animation state machine
- Compute-based particle system
- Render graph refactor: automatic barriers, transient resources, pass ordering

### Phase 6 — Map System

Ties simulation + render together into loadable, playable content.

- **Map format**: FlatBuffers serialization for terrain and object data
- **Terrain data**: heightmap, splatmap, pathing grid — loaded into both simulation and render
- **Object placement**: preplaced units, doodads, regions, cameras
- **Override system**: map-level unit/ability type overrides merging on top of engine defaults
- **Map loading/unloading**: full lifecycle with cleanup

### Phase 7 — Gameplay Systems

Requires terrain data from Phase 6. Completes the simulation systems left incomplete in Phase 4.

- **Pathfinding**: A* on tile grid, flow field for group movement
- **Collision**: spatial grid for unit-unit and unit-terrain queries
- **Order execution**: MovementSystem processes Move/Patrol, CombatSystem processes Attack, AbilitySystem processes Cast
- **Combat logic**: target acquisition, range checking, damage calculation, attack animation timing
- **Ability execution**: cast flow (validate → animate → fire effect), projectile spawning, aura scanning
- **Buff application**: stat modifier stacking/recalculation, periodic effects, dispel

### Phase 8 — Scripting (Lua 5.4)

Now the unit model, renderer, map system, and gameplay systems are stable — Lua binds against a tested API.

- **Lua VM**: Lua 5.4 integration, sandboxed per map
- **sol2 bindings**: expose unit facade, trigger API, game state queries to Lua
- **Trigger system**: event → condition → action (WC3-style)
- **Engine API**: CreateUnit, DamageTarget, SetUnitPosition, DisplayMessage, etc.

### Phase 9 — Editor (Terrain Editor v1)

Needs render + map working first.

- **ImGui integration**: ImGui rendering via Vulkan backend
- **Heightmap sculpting**: raise, lower, smooth, flatten brushes
- **Texture painting**: splatmap editing with brush
- **Pathing marking**: walkable, flyable, buildable tile flags
- **Object placement**: place, move, delete units and doodads
- **Save/Load**: serialize edited map to .uldmap package

### Phase 10 — Audio

Independent module, can be developed in parallel with other phases.

- **miniaudio integration**: device init, playback
- **3D positional audio**: per-unit sound, distance attenuation, listener tracking
- **Music streaming**: background music with crossfade
- **SFX system**: fire-and-forget sound effects, pooled voices

### Phase 11 — Networking

Last because it's the most complex and needs a working local game loop.

- **ENet integration**: reliable/unreliable UDP channels
- **Local server**: in-process server for single player (no sockets, direct calls)
- **Remote server**: networked server with state delta broadcasting
- **Client prediction**: local simulation with server reconciliation
- **Lobby system**: host/join, map selection, player slots, team assignment, ready check
- **Desync detection**: periodic state checksums for debugging

### Phase 12 — Packaging & Distribution

Cross-platform build output and asset protection.

- **PAK archive format**: pack engine assets and map assets into single archive files (engine.pak, map.pak)
- **Encryption**: encrypt map PAK files to protect gameplay logic from reverse engineering; optionally encrypt engine assets too
- **Build output**: self-contained distribution folder per platform (exe + engine.pak + any platform-specific files)
- **Windows packaging**: output folder or installer
- **Android packaging**: APK/AAB with assets bundled
- **Asset baking pipeline**: optional offline step — pre-compile shaders to SPIR-V, compress textures to GPU formats (KTX2), bake JSON configs to binary if needed
