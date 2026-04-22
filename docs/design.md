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
├── simulation/     # ECS, units, abilities, pathfinding, collision, AI
├── script/         # Lua 5.4 VM, sol2 bindings, trigger/event system
├── map/            # Map format (FlatBuffers), terrain data, object placement
├── network/        # Server-authoritative: server sim, client prediction, state sync
├── audio/          # miniaudio: 3D positional, SFX, music streaming
├── asset/          # Resource manager, glTF/KTX2/OGG loaders, async loading
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

### Current

- Vulkan 1.3 instance + validation layers (debug)
- Physical device selection (prefer discrete GPU)
- Logical device + graphics/present queue
- Swapchain with per-swapchain-image semaphores
- Per-frame command buffers, fences
- VMA for GPU memory management
- One-shot command buffers for immediate GPU work (texture uploads)
- Dynamic rendering (no render passes)
- Synchronization2 barriers
- Shader compilation: GLSL → SPIR-V at build time via glslc

### Future

- Render graph with automatic barriers and transient resources
- GPU-driven rendering (indirect draw, compute culling, instance buffer)
- Bindless resources via descriptor indexing (VK_EXT_descriptor_indexing)

## 7. Rendering

### Terrain

- Heightmap-based terrain with tile grid
- Splatmap texturing (blend up to 4 textures per tile)
- Pathing overlay (walkable, flyable, buildable)

### Units

- Skeletal animation with GPU skinning
- Animation state machine (clips bound by name: `idle`, `walk`, `attack`, `spell`, `death`)
- Selection circles, health bars (billboarded)

See `docs/model-format.md` for full model, animation, and art pipeline specification.

### Particles

- Compute-based particle system
- Emitter types: point, sphere, cone, mesh surface

### UI

- Custom retained-mode UI for game HUD
- Dear ImGui for editor and debug overlay

## 8. Simulation

See [gameplay-model.md](gameplay-model.md) for the full object model design, including:
- Entity categories (Unit, Building, Hero, Destructable, Item, Doodad, Projectile)
- Component design (Transform, Health, States, Attributes, Movement, Combat, Abilities, etc.)
- Order system (Move, Attack, Cast, Train, Build, Patrol, etc.)
- Ability system (data-driven definitions, cooldowns, targeting, passive auras, buffs as abilities)
- Generic state system (HP built-in, mana/energy/etc. map-defined)
- Generic attribute system (strength/agility/etc. all map-defined)
- String-based classifications, attack types, armor types (map-defined)
- Type definition system (JSON-driven, map-only — no engine defaults)
- ECS implementation (sparse sets, systems, World struct, unit facade API)

See [map-system.md](map-system.md) for the engine vs map boundary and map package format.

### Key Architecture Points

- ECS internally (sparse set per component type, cache-friendly iteration)
- Unit-centric facade API externally (free functions operating on entity IDs)
- Data-driven types: unit/ability definitions loaded from JSON
- Fixed timestep simulation at 32 real-time ticks/sec (31.25ms per tick), always constant regardless of game speed
- Game speed multiplier scales how much game time passes per tick (1.0x = 31.25ms game time/tick, 2.0x = 62.5ms game time/tick, 0 = paused)
- In-game clock tracks game-time elapsed (affected by speed), used by scripts and timers
- Tick count per real second never changes — game speed only affects game_dt, not CPU load
- Camera, audio, and editor update at real frame rate (unaffected by game speed)
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
├── textures/           # Map-specific KTX2 textures
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
    └── engine.json
```

### Override Mechanism

1. Engine loads base definitions from `engine/config/`
2. Map loads, its `overrides/` merge on top (add or replace entries by ID)
3. Map scripts can further modify at runtime via Lua API

## 11. Networking

Server-authoritative with client-side interpolation. See [network.md](network.md) for full protocol design.

- **Server** runs the authoritative simulation at 32 Hz, broadcasts state every tick
- **Clients** send commands to server, receive state snapshots, interpolate for smooth visuals
- **No client-side prediction** — acceptable for RTS (small command delay is expected)
- **Single player** runs the same server in-process (direct function calls, no sockets)
- **Transport** abstracted behind an interface; Phase 13b uses ENet (reliable/unreliable UDP)

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
| KTX-Software | 4.3+ | KTX2 + Basis Universal textures | Apache 2.0 |
| stb_vorbis | latest | OGG Vorbis decoding | MIT/Public Domain |
| FlatBuffers | latest | Binary serialization | Apache 2.0 |
| nlohmann/json | 3.11 | JSON parsing | MIT |

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
- **Core components**: Transform, Health, Movement, Combat, Ability, Vision, Owner, etc.
- **Typed handles**: Unit, Destructable, Item — distinct types wrapping Handle base
- **Unit facade**: create_unit, get_health, is_hero, is_building, issue_order, etc.
- **Unit type definitions**: data-driven types loaded from JSON via TypeRegistry
- **Systems**: health regen, cooldown ticking, ability duration — stubs for complex systems

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

**5e — Shadow mapping**
- Light-space depth pass (render scene from directional light's perspective)
- Shadow map texture (depth-only framebuffer, typically 2048x2048)
- Shadow sampling in main fragment shader (compare fragment depth vs shadow map)
- PCF (percentage-closer filtering) for soft shadow edges
- Verify: directional shadows cast by units onto terrain

### Phase 6 — Map System

Maps are self-contained gameplay packages. The engine provides mechanics; maps provide all content and rules. See `docs/map-system.md` for full design.

- **Map package**: `.uldmap` directory with manifest, types, scenes, scripts, assets
- **Manifest**: metadata, player slots, teams, map-defined enumerations (classifications, attack/armor types, states, attributes)
- **Terrain loading**: heightmap, tile types, pathing from binary format
- **Object placement**: preplaced units, destructables, items, doodads, regions, cameras
- **Type loading**: unit/ability/item/destructable types from map's `types/` directory (no engine defaults)
- **Scene system**: multiple terrains per map, transitions via Lua API
- **Tileset**: map-defined ground texture set
- **Map loading/unloading**: full lifecycle with cleanup
- **Test map**: move current test data (unit_types.json, destructable_types.json, item_types.json, test units) into a proper `.uldmap` package. Engine init creates no gameplay content — map system handles everything

### Phase 7 — Gameplay Systems

Requires terrain data from Phase 6. Completes the simulation systems left incomplete in Phase 4.

**7a — Pathfinding + movement**
- Pathfinding subsystem: A* on tile grid for single-unit pathing. Current implementation produces tile-center waypoints. Future: flowfield pathfinding for group movement (modern RTS standard — replaces per-unit A* when many units share a destination)
- MovementSystem: consumes pathfinding, moves units along paths each tick. Steering, turn rate, speed
- Terrain height placement: update position.z from heightmap each tick
- Terrain slope tilt: renderer aligns model to terrain normal (visual only, no sim data)
- Verify: issue a Move order, unit walks to destination on terrain

**7b — Collision + spatial queries**
- Spatial grid for unit-unit proximity queries
- `units_in_range()`, `units_in_rect()` query API
- Unit-unit collision avoidance (separation steering)
- Verify: units don't stack on top of each other

**7c — Combat + projectiles**
- CombatSystem: target acquisition, range checking, attack timing (dmg_time fore-swing, backsw_time recovery)
- Normal attack flow: melee hit, ranged projectile launch
- ProjectileSystem: flight, homing, hit detection, impact event
- Damage event: engine fires `on_damage` with raw damage + context, map Lua can modify (stubbed until Phase 8)
- Death event: engine fires `on_death` when HP reaches 0, entity cleanup
- Verify: unit attacks another, HP decreases, target dies

**7d — Abilities**
- AbilitySystem: cast flow (validate → cost check → animate → fire effect)
- Ability forms: instant, target_unit, target_point, passive, toggle, channel
- Aura scanning: apply/remove abilities to nearby units
- Applied ability duration ticking + removal
- Modifier stacking and recalculation (base + all active modifiers = effective value)
- Verify: cast Holy Light (heals target), Devotion Aura (nearby units gain armor)

### Phase 8 — Scripting (Lua 5.4)

Now the unit model, renderer, map system, and gameplay systems are stable — Lua binds against a tested API. See `docs/scripting.md` for full design.

- **Lua VM**: Lua 5.4 + sol2, sandboxed per map (no os/io/filesystem access)
- **common.lua**: engine API declarations (`engine/scripts/common.lua` — the "common.j" equivalent)
- **Unified event system**: `RegisterEvent(event_name, fn)` — one system for everything (abilities, combat, lifecycle)
- **Timer system**: `CreateTimer(delay, repeating, fn)` — for periodic effects, delayed actions
- **Engine API bindings**: unit CRUD, orders, abilities, damage, spatial queries, hero, player, regions

### Phase 9 — Animation & Particles

Visual feedback for gameplay systems. Validates that rendering, simulation, and scripting work together.

- **Skeletal animation**:
  - Extend vertex format with bone indices/weights (SkinnedVertex, 64 bytes)
  - Extract skeleton and animation data from glTF (cgltf: skins, joints, animations)
  - GPU skinning via vertex shader + bone SSBO (per-entity, descriptor set 2)
  - Separate skinned mesh pipeline (coexists with existing non-skinned pipeline)
  - Procedural test skeleton for validation without real models
- **Animation state machine**:
  - CPU-side, runs at render framerate (reads simulation state, drives playback)
  - States: Idle, Walk, Attack, Death — derived from Combat, Movement, DeadState components
  - Crossfade blending between states
  - Per-bone local TRS interpolation → hierarchy walk → final skinning matrices
- **Particle system** (CPU-driven):
  - Emitter types: point, sphere, cone
  - Per-particle: position, velocity, color, lifetime, size
  - Billboard quad generation on CPU, uploaded to dynamic vertex buffer
  - Alpha-blended pipeline (depth test on, depth write off)
  - Spawn on events: attack hit, death, projectile trail
- **Render graph refactor** (optional): automatic barriers, transient resources

### Phase 10 — Editor (Terrain Editor v1)

Needs render + map working first.

- **ImGui integration**: ImGui rendering via Vulkan backend
- **Heightmap sculpting**: raise, lower, smooth, flatten brushes
- **Texture painting**: splatmap editing with brush
- **Pathing marking**: walkable, flyable, buildable tile flags
- **Object placement**: place, move, delete units and doodads
- **Save/Load**: serialize edited map to .uldmap package

### Phase 11 — Audio

Independent module, can be developed in parallel with other phases.

- **miniaudio integration**: device init, playback
- **3D positional audio**: per-unit sound, distance attenuation, listener tracking
- **Music streaming**: background music with crossfade
- **SFX system**: fire-and-forget sound effects, pooled voices

### Phase 12 — Input System

Player input → command → simulation pipeline. Engine provides built-in input presets;
maps configure keybinds and UI layout. Mobile UI deferred until Android builds work.

**Phase 12a — Command System & Selection**
- `GameCommand` struct: the universal unit of player intent (Move, AttackMove, Attack, Cast, Stop, Hold, Patrol)
- All commands flow through the command system — Lua `IssueOrder`, input presets, and (later) network all produce `GameCommand`s
- Engine-managed per-player selection state (selected units, subgroup)
- Selection helpers: screen-to-world raycast, unit picking, box select region

**Phase 12b — RTS Input Preset (Desktop)**
- Left click: select unit / click ability target
- Box drag: multi-select
- Right click: smart order (move ground, attack enemy, follow ally)
- Ability hotkeys (map-configurable)
- Control groups (Ctrl+1..9 assign, 1..9 recall)
- Camera: edge pan, middle-mouse drag, scroll zoom

**Phase 12c — Map Input Configuration**
- Commands vs ability slots: built-in commands (attack, move, stop, hold) are separate from ability slots
- Ability slot system: 16-slot array per unit, Lua API for runtime slot management
- `InputBindings`: action-to-key mapping with JSON overrides from manifest `"input"` section
- Preset factory: engine creates preset from manifest `"input_preset"` field
- Ability `"hotkey"` and `"hidden"` fields in ability definitions
- Lua events: `on_order` (cancellable), `on_select`
- Lua selection API: `GetSelectedUnits`, `SelectUnit`, `ClearSelection`, etc.

*Phase 12d (Action/RPG Input Preset) merged into Phase 16b (Mobile UI).*

### Phase 13 — Networking

Server-authoritative model with client-side interpolation. See [network.md](network.md).

**Phase 13a — Local Server Refactor** ✓
- GameServer class owns Simulation + ScriptEngine
- Single player runs server in-process (direct function calls, no sockets)
- Clean client/server separation — foundation for all networking
- Fog of war: per-player visibility, cliff LOS, shared vision, visual smoothing

**Phase 13b — Multiplayer**
- Transport abstraction + ENet implementation (reliable/unreliable UDP)
- Binary protocol: commands (client → server), state snapshots (server → client)
- Server broadcasts entity state every tick (32 Hz), fog-filtered per player
- Client interpolation: buffer two snapshots, render between them
- Host mode (`--host`) and connect mode (`--connect <ip>`)
- No client-side prediction — RTS command delay is acceptable

**Phase 13c — Session Management** ✓
- `--map <path>` CLI arg (replaces hardcoded map path)
- Server waits for expected player count before ticking simulation
- `S_START` broadcast: all clients begin at the same moment
- `EndGame(winner, stats_json)` Lua function → fires `on_game_end` event → broadcasts `S_END`
- No lobby UI — that's a product-level concern (Phase 16)

**Phase 13d — Reconnect** ✓
- Disconnected player's state kept alive during configurable timeout (`manifest.reconnect.timeout`)
- Reconnecting client receives full state snapshot to catch up
- Optional game pause on disconnect (`manifest.reconnect.pause`)
- Timeout expiry fires `on_player_dropped` Lua event (map decides what happens to units)
- Client detects server disconnect and transitions to Results

### Phase 14 — Rendering Improvements

GPU-driven rendering, culling, anti-aliasing, and water.

**Phase 14a — Instance Buffer + Indirect Draw**
- Per-entity transforms in a GPU SSBO (instance buffer), indexed by instance ID
- Replace per-entity `vkCmdDrawIndexed` with `vkCmdDrawIndexedIndirect`
- One indirect draw per unique mesh+material combination
- Foundation for all subsequent GPU-driven work

**Phase 14b — Mesh Merging + Bindless Descriptors + Frustum Culling**
- Mega vertex/index buffer: all static meshes share one VB+IB with draw offsets
- Bindless texture array (`VK_EXT_descriptor_indexing`), per-instance material index
- Single multi-draw-indirect call for all static geometry (main pass + shadow pass)
- CPU frustum culling: bounding sphere vs 6 frustum planes (Gribb/Hartmann extraction from VP matrix)
- Entities fully outside camera view skipped before instance data upload

**Phase 14c — MSAA**
- Multisample anti-aliasing (4x) for all geometry pipelines
- Multisampled color + depth render targets, resolve to swapchain
- Core Vulkan feature — works on all desktop and mobile GPUs

**Phase 14d — Tileset Textures + Terrain Blending + Water**
- Load real diffuse textures from tileset JSON (map-provided PNGs)
- Terrain type as `sampler2DArray`, per-vertex type index (up to 16 layers)
- Blend-mask transitions between terrain types (engine presets: flat, noisy, blades, rocky, cracked)
- Water as terrain layer types: shallow (transparent two-pass) and deep (opaque one-pass)
- Water visuals: animated UV, tinted color, configurable opacity/wave_speed per tileset

**Phase 14e — Lighting, Skybox & Day/Night**
- Dynamic lighting: sun direction, sun color, ambient color as uniforms (currently hardcoded in shaders)
- Skybox: cube or hemisphere rendered behind everything, per-tileset sky texture or procedural gradient
- Emissive particles: effects like holy light, fire, magic skip scene lighting and glow at full brightness
- Lua API for map-controlled atmosphere: `SetSunDirection`, `SetSunColor`, `SetAmbientColor`, `SetSkyColor`
- Maps can drive day/night cycle with timers, set fixed time of day, or trigger dramatic lighting changes

### Phase 15 — Packaging & Distribution

Build targets, cross-platform packaging, and asset baking.

#### Phase 15a — Build Targets
- **`uldum_game`**: shipped product executable (Release build, no debug UI, no editor)
- **`game.json`**: product configuration (product name, default map, window title, resolution)
- **`uldum_server`**: headless dedicated server (no renderer, no audio, no window — simulation + networking only)
- Build scripts: `build_game.ps1`, `build_server.ps1`

#### Phase 15b — PAK Archive
- Pack engine assets and map assets into single .pak archive files (engine.pak, map.pak)
- Runtime: AssetManager loads from PAK instead of loose files
- Encryption for map PAK (protect Lua scripts/gameplay logic)

#### Phase 15c — KTX2 Textures
- KTX2 + Basis Universal is the **only** runtime texture format, everywhere (engine, maps, all targets)
- Authoring workflow: map makers convert PNG → KTX2 with `toktx` *before* dropping files into a map folder
- Engine never bakes. `uldum_pack pack` remains pure archive; no format transforms anywhere in the build
- Editor source-folder / normal modes still exist (different save semantics), but format-wise they're identical — both require KTX2
- See [editor.md](editor.md) for editor modes, [packaging.md](packaging.md) for runtime texture loading and author workflow

#### Phase 15d — Platform Packaging
- **Windows**: `build_game.ps1 <project>` produces `dist/<GameName>/` with exe + `engine.uldpak` + listed maps + `game.json`. Installer deferred.
- **Android**: debug APK that installs and renders the test map
  - Android NDK + Gradle wrapper at `android/`, engine as `libuldum_game.so`
  - `src/platform/android/` — GameActivity, Vulkan surface, lifecycle
  - APK-bundled assets via `AAssetManager` (new mount kind)
  - Touch input and mobile UI are Phase 16d, not here
- **Linux** and **iOS** deferred. Release signing / AAB / Play Console deferred.

### Phase 16 — UI System

Two systems: **Shell UI** (RmlUi, menus / game-room / settings / results) and **HUD** (custom, Lua-driven, in-game widgets). Concepts, rationale, authoring model, and primitives: see [ui.md](ui.md).

**Phase 16a — Shell UI framework (RmlUi integration)**
- RmlUi as a CMake `FetchContent` dep.
- `Rml::RenderInterface` on top of our Vulkan RHI (mesh upload, texture binding, scissor, transform). Per-frame re-tessellation first; switch to persistent geometry only if frame time demands.
- `Rml::SystemInterface` bridging time + input from `platform/windows` + `platform/android`.
- Single `Rml::Context` owned by the engine; `App` loads / shows / hides RML documents.
- Smoke test: trivial RML doc renders on Windows + Android.
- `ImGui` stays for dev tools — no conflict.

**Phase 16b — sample_game shell screens + game lobby**
- Skeleton-only (flat colors, default font, no polish): main menu → game lobby → loading → in-session → results → menu, plus a settings overlay.
- RML + RCSS under `sample_game/shell/`, Lua button callbacks; hot-reload, no rebuild.
- Game lobby lives in both Shell UI and dev UI: per-team slot table, host is authoritative, host's Start commits. Manifest slots carry `{team, color, type?, name?}`; `"type": "computer"` is a map-locked AI. See `docs/network.md` for the lobby protocol and `Simulation::get_player_name` / Lua `GetPlayerName` for name exposure.
- Dev APK's Android map picker = same Shell pipeline, auto-populated from bundled test maps.

**Phase 16c — HUD system (custom, Lua-driven)**
- Engine primitives: world-space billboards, screen-space panels, bars, icons, text labels, minimap.
- Lua API: `DefineHudBar`, `ShowSelectionCircle`, `AttachIconToUnit`, etc. Values bound to sim state.
- Custom instanced batch renderer — depth-aware for world-space.
- Completely separate from RmlUi.

**Phase 16d — Screen transitions + mobile polish**
- Screen state machine; fades via RCSS transitions.
- GameActivity touch → RmlUi `pointerdown` / `pointerup`.
- RCSS sizing rules for mobile tap targets.
- IME plumbing for text-entry fields — deferred until needed.

## 16. Deferred / Future Work

Topics scoped out of current phases — revisit when the time comes.

- **Multi-lobby server** — today's `uldum_server` hosts one game per process. Multi-tenant support (lobby directory, browse / create / join) is deferred. Workaround: multiple server processes on different ports.
- **LAN game discovery** — WC3-style auto-populated list of local hosts via UDP broadcast, so clients don't have to type an IP.
- Controller / gamepad input.
- CJK / RTL text shaping (HarfBuzz).
- Rich custom shader decorators (game-project art concern).
- UI designer tool — authors edit RML / RCSS directly.
