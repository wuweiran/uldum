# Uldum Engine — Technical Design

## 1. Vision

A modern game engine for unit-centric, map-based games with scripting and multiplayer — inspired by Warcraft III's architecture, built with zero legacy burden.

## 2. Target Platforms

- Windows (Win32 + Vulkan)
- Android (GameActivity + Vulkan / OpenGL ES)

## 3. Module Architecture

```
uldum/
├── core/             # Containers, math (glm), logging, settings, types
├── platform/         # Custom platform layer
│   ├── windows/      #   Win32 window, input, filesystem
│   └── android/      #   GameActivity, touch input, APK filesystem
├── rhi/              # GPU abstraction with two interchangeable backends
│   ├── vulkan/       #   Vulkan 1.3 (dynamic rendering, sync2, bindless)
│   └── gles/         #   OpenGL ES 3.1+ (deferred state, hand-written GLSL ES)
├── render/           # Renderer, terrain, skeletal anim, particles, effects, world overlays, shadow
├── simulation/       # ECS, units, abilities, pathfinding, collision, projectiles
├── script/           # Lua 5.4 VM, sol2 bindings, trigger/event system
├── map/              # Map format (hand-rolled binary), terrain data, object placement
├── network/          # Server-authoritative: server sim, client snapshots, ENet transport
├── audio/            # miniaudio: 3D positional, SFX, music streaming
├── asset/            # Resource manager, glTF/KTX2/OGG loaders, .uldpak mounts
├── input/            # GameCommand, selection state, RTS / Action input presets
├── input_router/     # Picker + preset dispatch (split so server has no client deps)
├── hud/              # Custom retained-mode HUD (atoms + composites, world overlays, text tags)
├── shell/            # RmlUi-backed shell (menus, lobby, results) — game builds only
├── i18n/             # Locale manager, string-pool lookup
├── editor/           # In-engine terrain editor (ImGui): sculpt, paint, cliff, ramp, pathing
├── tools/            # Build-time CLIs: `uldum_pack`, overlay generator
├── server/           # Headless binaries: `uldum_worker` per session, `uldum_server` orchestrator
└── app/              # `class Engine`: entry points, main loop, hosts the active `App`
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

### Android Implementation

- GameActivity (Android Game Development Kit) hosts the engine inside a native activity
- Touch input mapped into the same `InputState` the Win32 path produces
- APK asset access via `AAssetManager`, density-aware `ui_scale`, safe-area insets exposed to the HUD

## 6. RHI Module

Two interchangeable GPU backends behind a single public surface (`CommandList`, pipeline / layout / buffer handles, etc.). The active backend is picked at startup; consumer code never branches on it.

### Vulkan 1.3 backend

- Instance + validation layers (debug)
- Physical device selection (prefer discrete GPU)
- Logical device + graphics/present queue, swapchain with per-image semaphores
- Per-frame command buffers + fences
- VMA for GPU memory management
- One-shot command buffers for immediate GPU work (texture uploads)
- Dynamic rendering (no render passes)
- Synchronization2 barriers
- Bindless textures via `VK_EXT_descriptor_indexing`
- Indirect draw via `vkCmdDrawIndexedIndirect` (multi-draw indirect for GPU-driven static geometry)
- Dynamic state: viewport, scissor, and (opt-in per pipeline) cull mode
- Shader compilation: GLSL → SPIR-V at build time via glslc

### OpenGL ES backend (3.1+)

- EGL context creation on Android
- Framebuffer-as-swapchain, deferred state batches as command-buffer-equivalent
- UBO + sampler bindings as descriptor-equivalent (flat binding scheme: `set * 16 + binding`)
- Program + state cache as pipeline-equivalent
- Bindless replaced by a fixed-size `sampler2DArray` keyed by per-instance material index
- Indirect draw via `glDrawElementsIndirect` (one call per group; no multi-draw)
- Hand-written GLSL ES siblings of every Vulkan shader live at `src/render/shaders/gles/` and are packaged into `engine.uldpak`

### Deferred

- Render graph with automatic barriers and transient resources
- Compute-driven culling (per-instance visibility on GPU)
- Mesh shaders / pipeline cache persistence

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

- CPU-driven billboard particle system; updated on the main thread, uploaded to a dynamic vertex buffer each frame
- Burst emitters and continuous emitters (`emit_rate` field on the effect def); position spawn pattern is either spread-cone (`spread`) or horizontal ring (`radius`)
- Procedural shapes selected by ID in the fragment shader (soft circle, spark, blood splatter, glow beam, water droplet) — no texture authoring required for the common cases
- Glow particles seed dynamic point lights into the scene

### UI

- **Shell UI** (`src/shell/`, game builds only) — RmlUi 6 with a custom Vulkan / file / system interface set. Game projects ship their own `shell/` directory with `.rml` + `.rcss` + fonts; the App drives screen transitions through `engine.shell().load_document(...)` / `.bind(...)`.
- **HUD** (`src/hud/`, all builds) — custom retained-mode tree of atoms (panel, label, image, bar, button) and composites (action_bar, command_bar, minimap, joystick, inventory). 2D quad batcher feeds the render graph; SDF text for crisp scaling. World-anchored overlays (health bars, name labels, text tags) ride the same batcher with world-to-screen projection.
- **Dev console / editor** — Dear ImGui (Vulkan + GLES backends both wired), used for the in-engine map editor and the dev runtime's map picker / session controls.

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
    ├── units.json     # Override/add unit types
    └── abilities.json  # Override/add abilities
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

Versions are pinned in `third_party/CMakeLists.txt` via `FetchContent`. `latest` here means tracking the default branch rather than a tagged release.

| Library | Pin | Purpose | License |
|---|---|---|---|
| glm | 1.0.3 | Math (rendering) | MIT |
| nlohmann/json | v3.12.0 | JSON parsing | MIT |
| stb | latest | `stb_image` (PNG/JPEG/...), `stb_vorbis` (OGG decode) | MIT / Public Domain |
| cgltf | v1.15 | glTF 2.0 loading | MIT |
| Lua | v5.4.8 | Scripting runtime | MIT |
| sol2 | latest | C++ ↔ Lua binding (main branch — 3.3.0 has a reference-optional bug newer Clang trips on) | MIT |
| Dear ImGui | v1.92.8 | Editor / debug UI | MIT |
| miniaudio | 0.11.25 | Audio playback | MIT / Public Domain |
| VMA | v3.3.0 | Vulkan memory allocator | MIT |
| ENet | v1.3.18 | UDP networking | MIT |
| cpp-httplib | v0.46.0 | HTTP for `uldum_server` (orchestrator API) + dev CLI client | MIT |
| Basis Universal | latest | KTX2 transcoder (runtime) + encoder `basisu` CLI (author-time) | Apache 2.0 |
| FreeType | VER-2-14-3 | Font rasterization for HUD MSDF + RmlUi | FTL / GPLv2 |
| RmlUi | 6.2 | Shell UI framework (game builds only) | MIT |

Build-time dependencies (not part of the engine binary): `glslc` from the Vulkan SDK for SPIR-V compilation. No use of shaderc, FlatBuffers, KTX-Software, or Tracy yet (Tracy is listed under Deferred work below).

## 15. Build Phases

### Phase 1 — Minimal Running Engine

Core loop, Win32 window, Vulkan init, clear screen to a color, input to close window. Proves the foundation works.

### Phase 2 — Full Structure with Stubs

All modules created with interfaces defined, CMake targets wired up, stub implementations. Compiles but most features return early.

### Phase 3 — Core & Asset Foundation ✓

Prerequisite layer that all gameplay and rendering modules depend on.

- **Core enhancements**: custom allocators (linear, pool), ResourcePool
- **Asset manager**: handle-based resource loading with path caching
  - glTF 2.0 model loading (cgltf)
  - PNG texture loading (stb_image)
  - JSON config loading (nlohmann/json)
- **Third-party deps**: glm, stb, cgltf, nlohmann/json via CMake FetchContent

### Phase 4 — Simulation (ECS + Unit Model) ✓

ECS foundation and data-driven unit creation.

- **ECS core**: paged sparse-set component storage and monotonic typed entity ids
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
- **Test map**: move current test data (units.json, destructables.json, items.json, test units) into a proper `.uldmap` package. Engine init creates no gameplay content — map system handles everything

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
- **`uldum_worker`** (initially built as `uldum_server`, renamed in Phase 24): headless per-session game server (no renderer, no audio, no window — simulation + networking only)
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

Custom retained-mode UI stack, separate from RmlUi. Available in both dev and game builds. One input preset per map (RTS or RPG), with per-platform (desktop/mobile) adaptation driven by the engine. HUD owns screen-space widgets and world-anchored overlays (healthbars, floating damage text).

- **16c-i — UI foundation.** 2D quad batcher + font pipeline + retained node tree + input routing. Cross-platform (desktop + mobile) from day one. Delivers one interactive button on screen; no game integration. HUD-stack specifics (renderer, font pipeline, text stack) in [ui.md](ui.md).
- **16c-ii — Node catalog (atoms + composites).** Atom nodes (panel, label, image, bar, button) + v1 composites (action_bar, command_bar, minimap, joystick). Catalog and schema pinned in [ui.md](ui.md). Deferred composites tracked separately: unit_panel, status_strip, resource_bar, dialog, chat_box.
- **16c-iii — World UI.** Screen-pixel-sized elements anchored to world positions or entities, projected each frame, rendered on top of the 3D scene (no occlusion). Covers multi-bar overlays above units, hover-triggered name labels, and WC3-style text tags (Lua-created, optionally animated with velocity / lifespan / fadepoint). Selection circles (ground decals) tracked separately — either as a follow-up sub-phase or deferred.
- **16c-iv — `hud.json` loader + RTS desktop preset.** Parser for the `hud.json` schema from [ui.md](ui.md). Port today's hardcoded HUD to a declarative `hud.json`; `test_map` declares `"preset": "rts"`. RTS preset behavior matrix lives in [input-system.md](input-system.md).
- **16c-v — Lua API + server/client sync.** Single authoritative Lua VM on the server. Bindings follow the WC3-style flat naming in [ui.md](ui.md): `GetNode`, `SetNodeVisible`, `SetLabelText`, `SetBarFill`, `CreateNode`, `DestroyNode`, `TriggerRegisterNodeEvent`, `CreateTextTag` / `SetTextTag*` / `DestroyTextTag`, etc. Networking: atom node state changes broadcast as `S_HUD_UPDATE` deltas; transient targeted / broadcast events use `S_HUD_EVENT`; client atom input forwarded as `C_NODE_EVENT`. Composite actions (action_bar slot press → ability cast) ride existing game-command protocols, no new messages.
- **16c-vi — RTS mobile variant.** Touch affordances, safe-area handling, mobile-tuned sizes. Android `uldum` auto-boots into it.
- **16c-vii — Action preset (desktop).** `action_rpg` input preset + `action_bar` bound to a hero actor. `action_test.uldmap` is the dev test bed.
- **16c-viii — Action preset (mobile).** `joystick` composite active; touch action_bar. Swipe-look deferred — minimap pan covers camera for v1.

**Phase 16d — Android dev parity + Shell polish**

- **16d-i — Android dev console.** Map picker + ImGui dev console on Android, reusing `uldum_dev`'s code paths. Boots into the picker instead of auto-starting on a hardcoded map.
- **16d-ii — Shell screen transitions.** State machine for Menu → Lobby → Loading → Playing → Results, with fades / slides via RCSS `transition` / `animation`.
- **16d-iii — Shell mobile input.** GameActivity touch → `Rml::Context::ProcessMouseButton*`.
- **16d-iv — Mobile tap-target sizing.** RCSS rules for minimum touch sizes + scaled fonts when host is mobile.
- IME plumbing — deferred until needed.

### Phase 17 — Items

First gameplay primitive beyond combat / abilities. Engine stays policy-light: an item is a typed entity bundling icon + model + a list of abilities granted to the carrier, plus two free integer fields (`charges`, `level`) the engine renders but never interprets. WC3-style consumption / merge / drop-on-death live in map Lua, on top of `EVENT_ITEM_*` events that expose `GetTriggerItem()`. Full design in [docs/items.md](items.md).

- Item type schema (`items.json`); kind derived from `abilities[0].form`.
- `Inventory` component + `Item` entity; smart right-click `PickUp` order; `Drop` from Lua / HUD.
- `inventory` HUD composite — slot row, icon, charges (bottom-right) and level (top-left) badges shown only when > 0.
- Lua bindings: `CreateItem`, `RemoveItem`, `GiveItem`, `UnitDropItemFromSlot`, `GetItem{Charges,Level,TypeId}`, `SetItem{Charges,Level}`, `GetTriggerItem`, item events.
- Network: rides existing entity-delta sync; no new message kinds.

### Phase 18 — Scripting enrichment

Thin Lua-binding categories that together turn the engine into a genuine map-authoring platform. Bundled because each is small on its own and they compose — a typical map uses regions to detect the player reaching a story beat, fires a camera pan, opens a confirmation popup composed from atom nodes, then loads the next scene.

- **Regions.** Rect / circle zones registered by Lua. Trigger events fire on enter / leave. Lua: `CreateRegion`, `AddRegionRect`, `AddRegionCircle`, `TriggerRegisterEnterRegion`, `IsUnitInRegion`, `GetUnitsInRegion`. The single biggest authoring primitive WC3 had outside of triggers themselves — without it, "step into the cave" / "kill zone" / "spawn-on-approach" all need hand-rolled distance polling.
- **Scene switching.** A map ships several scenes (terrain + placements + per-scene `main.lua`) and Lua can swap between them mid-session. Lua: `LoadScene(name)`. The engine tears down entities + reloads the new scene's placements, then `ScriptEngine` resets per-scene state (triggers, timers, event contexts) and runs the new `main()`. Map-level state (type registry, environment, network connection) survives. The Lua VM is restarted on each swap; cross-scene data rides the save channel (`SaveData` / `LoadData`). Pairs naturally with regions for "walk into portal → swap scene".
- **Node-event + composite UI bindings.** Round out the HUD scripting surface so map authors can compose dialogs, popups, and tutorials from atom nodes (panel + label + button) declared in `hud.json` and triggered through the existing event system. New bindings: `ShowNode` / `HideNode`, `MinimapSetVisible`, `JoystickSetVisible`, `TriggerRegisterNodeEvent(trig, node, EVENT_BUTTON_PRESSED)`. Combined with templates the result is what a "dialog composite" would have shipped — without locking dialog shape into the engine.
- **Game pause + single-player query.** `PauseGame()` / `UnpauseGame()` flip a script-owned flag the App reads each frame to gate sim ticks (independent of the network's reconnect-pause). `IsSinglePlayer()` reports whether the session is offline. Together they let dialogs and cutscenes freeze gameplay locally without breaking MP.
- **Camera scripting.** Programmatic pan / zoom / shake / lock-to-unit. Lua: `PanCamera`, `SetCameraPosition`, `SetCameraZoom`, `SetCameraLockUnit`, `CameraShake`. Tiny API but elevates production quality of small games disproportionately — boss reveals, screen-shake on big hits, scripted intros.

### Phase 19 — Editor expansion

Authoring efficiency becomes the bottleneck once 17 + 18 land — the existing terrain editor doesn't know about units, items, destructables, or regions. This phase fills that out:

- Fine-grained pathing grid — split the pathing tile from the terrain cell so pathing resolution is a fraction of a terrain tile (WC3 ships ¼-cell pathing). Prerequisite for destructables and small footprints to feel right.
- Unit / item / destructable / doodad placement on the terrain canvas. Selecting a placed object exposes the few toggles that matter — unit facing + owner, destructable variation — and any object can be dragged to a new position.
- Region tool — draw rect / circle, name it, persist into the scene.
- Asset management — import authoring-friendly source files into the engine's runtime formats. First target: PNG → KTX2 (Basis-Universal compressed), with options exposed for cases like HUD icons that need zstd disabled. Imports need a clear destination: the editor exposes the map package's directory tree (e.g. `textures/`, `models/`, `audio/`) and the import dialog lets the author pick the target sub-path so it's obvious where the converted file lands.

By this point items + regions + scene-driven UI + camera have produced concrete authoring pain points the editor can address with knowledge of what the engine actually supports. Triggers stay in Lua scripts (no in-editor trigger graph) — the script-side `TriggerRegister*` surface is the authoring interface there.

### Phase 20 — Gameplay Enrichment

The engine's gameplay surface is thin compared to what map authors hitting WC3 expectations will want. Phase 20 expands it. Detailed designs live in the linked docs; this list is the work breakdown.

- Ability forms expansion — split modifier passive into `passive_modifier` + `passive_flag`, refcounted flags, `aura` broadcasts buffs, ability-namespace attribute modifiers. See [ability-system.md](ability-system.md).
- Events — `EVENT_UNIT_DYING`, `EVENT_UNIT_ISSUED_ORDER`, `EVENT_UNIT_ABILITY_CHANNEL`, `EVENT_UNIT_ABILITY_ENDCAST` and global variants.
- Status flags — first-class unit states (stunned / silenced / muted / disarmed / rooted / invulnerable / magic_immune / untargetable / paused).
- Animation control — `SetUnitAnimation` / `QueueUnitAnimation` Lua bindings.
- Effect attachment slots — named points (`head`, `weapon`, `origin`).
- Fog-of-war scripting — Lua bindings (`UnitShareVision`, `CreateFogModifier`, `MakeUnitVisible`, `IsVisibleToPlayer`).

### Phase 21 — Projectile

Promotes projectiles to first-class agents with a scriptable lifecycle. Unifies auto-attack arrows and ability projectiles under one API.

- Two-stage lifecycle (create at source, then emit) with per-projectile triggers.
- Homing and skillshot paths.
- Damage as a first-class payload; engine auto-routes auto-attack hits.
- Death animation window before teardown.
- MP sync under the standard per-peer visibility filter.

### Phase 22 — I18n

Full internationalization across rendering, content, and UI. Today the engine is English-default; complex scripts need shaping work and map authors have no way to localize strings.

- Glyph rendering — CJK and other complex scripts via HarfBuzz integration in the HUD text stack. Font fallback chain (Latin → CJK face for codepoints outside the primary font).
- Map content i18n — translatable strings in unit type defs, ability defs, dialogs, text tags, scene scripts. Maps ship per-locale string tables (`strings/en/xxx.json`, `strings/zh/xxx.json`, …) and reference entries by key; engine resolves at load time based on player locale.
- Shell + HUD i18n — RmlUi and HUD authors reference string keys rather than literal text; engine looks up against the active locale.
- RTL shaping — Hebrew / Arabic visual ordering. Same HarfBuzz integration covers it.

### Phase 23 — Headless Worker

Make the per-session game server a clean, game-agnostic, headless binary that builds and runs on Linux as well as Windows — the prerequisite shape for any future cloud-deployable session host. One process still hosts one game session; multi-session orchestration lands in Phase 24 as a separate `uldum_server` binary on top. *(Built and shipped as `uldum_server`; renamed to `uldum_worker` in Phase 24 once the orchestrator takes the `uldum_server` slot.)*

- Generic worker — strip `game.json` reads; operator supplies maps via `--map`.
- Map fingerprinting — SHA-256 over every `.lua` file in the map, lexicographic path order. Mismatch on `C_JOIN` is a hard reject.
- Library split for headless builds — `uldum_hud` becomes pure data with a render-side `HudRenderer` in `uldum_render`; `uldum_input` becomes the tiny bindings lib with picker / presets moved to `uldum_input_router`. Worker's transitive link graph no longer pulls Vulkan / rhi / vma / freetype / msdfgen.
- Linux build target — worker is headless; CMake + a few `#ifdef _WIN32` cleanups.

### Phase 24 — Server Orchestration + Reconnect

Production-deployment shape: `uldum_server` (orchestrator) spawns `uldum_worker` processes per session, driven by an HTTP API game backends call into. Architecture in [network.md](network.md#production-deployment-topology).

- Two binaries — `uldum_server` (orchestrator), `uldum_worker` (one process per session).
- HTTP API for game backends — `POST /sessions` creates a session and returns connection info + per-player tokens.
- Map allowlist auto-discovered from `<cwd>/maps/`.
- Auth-on-join — worker validates per-player tokens at `C_JOIN` (standalone mode still accepts all).
- Reconnect-after-blip — disconnected slots held open for N seconds; same token rejoins the same slot.
- `GAME_SESSION` global — game backend's `initial_data` JSON exposed to map Lua before `main()` runs.
- Webhook dispatch — worker writes end-of-session result to stdout; orchestrator POSTs it to the game backend's `webhook_url`.
- `uldum_dev --server <url>` — dev client doubles as a game-backend stand-in for local testing.
- Engine stays cloud-neutral — caller auth is the deployment's job (Azure API Management / App Gateway / nginx / etc. terminate cloud-native auth in front of the orchestrator).

### Phase 25 — OpenGL ES RHI

Only ~50% of in-market Android phones support Vulkan 1.3 (most missing devices lack dynamic rendering or synchronization-2, both of which the current RHI uses). The unsupported half overlaps heavily with the lower-end, casual-game-paying audience. Phase 25 adds a second RHI backend targeting OpenGL ES 3.2 so the engine reaches the rest of the Android market. Quality is "fallback" — features that don't translate cleanly (bindless, indirect draw, shadow cascades when those land) gate to the Vulkan backend.

- RHI abstraction — extract the implicit interface in `vulkan_rhi.h` into an abstract `Rhi` base. `VulkanRhi` and `GlesRhi` are concrete implementations selected at startup based on device capability.
- GLES 3.2 backend — context creation (EGL on Android), framebuffer-as-swapchain, deferred state batches as command-buffer-equivalent, UBO + sampler bindings as descriptor-equivalent, program + state cache as pipeline-equivalent.
- Shader pipeline — author once in SPIR-V (current `.spv` flow); transpile to GLSL ES at build time via SPIRV-Cross, ship both compiled SPIR-V and GLSL ES sources inside `engine.uldpak`.
- Bindless texture array → texture-2D-array — GLES has no real bindless. The static mesh / instance pipeline collapses material indexing into a single 2D-array sampler with per-instance layer index.
- Indirect draw → instanced draw loop — GLES has glDrawElementsIndirect but limited; fall back to one `glDrawElementsInstancedBaseInstance`-equivalent per draw group.
- Per-feature gating — UI's "graphics" settings expose only what the active backend supports. Vulkan-only features list lives next to the RHI selector.

### Phase 26 — App architecture revamp

Today's `uldum_game` and `uldum_dev` are conditional-compilation flavors of one binary, with menu / lobby / settings logic embedded in the engine runtime's click dispatcher. That doesn't scale — a real game can't customize UX without forking the engine. Phase 26 establishes the model every Uldum-based product uses: the engine is a library of services exposed as `class Engine`, and a product is an `App` implementation that uses them. The dev console becomes one such app, proving the architecture by being its first consumer. After this Phase, "dev" and "game" are no longer build flavors — they're different `App` implementations linked into the same engine binary. The target model and runtime/build details are in `docs/engine-model.md`.

- Refactor `uldum_dev` into the new architecture first — rename `class App` → `class Engine`, extract the menu / lobby / pause flow into a `DevApp : App` implementation, retire the `ULDUM_DEV_UI` / `ULDUM_SHELL_UI` flavors. This validates the architecture before any other consumer. **(Done — landed alongside this doc rewrite. `DevApp` reaches `Engine` through its public surface only, no friend access.)**
- Define the `App` interface + factory and the `Shell` facade `App` implementations use to drive RmlUi.
- Add `shell/bindings.json` for declarative button actions (navigation, quit) so trivial cases don't need C++.
- Parameterize desktop CMake + Android Gradle by `ULDUM_GAME_PROJECT_DIR` so the game project's C++ sources and assets get folded in at build time. Binary name derives from `game.json`.
- Mirror the model on the worker side (`class Worker`).
- Convert `sample_game/` to the new model (the reference `SampleGameApp`) and validate the full flow end-to-end on Windows + Android. After this, the existing dispatcher in `engine.cpp` is gone.
- Documentation pass — update `docs/build-targets.md` to drop the "dev vs game flavor" framing.

## 16. Deferred / Future Work

Two tiers by whether they block shipping a real production game; grouped by domain inside each tier.

### Production blockers

**Platform reach**

- **iOS client** — currently Windows + Android. Needs Metal RHI backend (or MoltenVK shim) and an iOS platform layer to mirror `android_platform.cpp`.

### Quality & convenience (non-blocking)

**Networking**

- **Transport encryption** — DTLS-over-UDP, QUIC, or noise-on-ENet. Closes on-path packet snooping + injection. Server-authoritative game logic already neutralizes most cheating; vendor-SDK HTTPS already protects login. Add when threat model justifies it: real-money / regulated data, competitive title where griefing is existential, GDPR / regulatory requirement on EU launch.
- **TCP fallback (WebSocket-over-TLS)** — secondary transport for the ~5-10% of sessions on restricted networks (corporate firewalls, locked-down public Wi-Fi) where UDP gets dropped. Server listens on both; same wire protocol; client falls back when UDP fails to establish or deliver. Pulls TLS in transitively, which closes the encryption gap as a side effect.
- **Protocol version handshake** — client + server exchange a wire-protocol version at `C_JOIN`; mismatch is a clean rejection. Useful once shipped clients are out-of-sync with running servers (App Store auto-update windows, mixed live versions); not useful pre-launch when client + server are rebuilt together every run.
- **LAN game discovery** — WC3-style UDP broadcast so local clients don't have to type an IP.

**Tooling & authoring**

- **UI designer tool** — authors edit RML / RCSS directly until then.
- Controller / gamepad input.

**Rendering quality**

- **PBR material model** — replace per-shader Lambert + ad-hoc specular with a shared metallic/roughness BRDF. Required for glTF `pbrMetallicRoughness` to render as authored. Triggered by the first imported metallic-roughness asset.
- **HDR + tonemap** — float color target, ACES / Khronos PBR Neutral on present. Without it, emissives clip and bright-sun + dark-shadow can't both be exposed.
- **Tangent space (MikkTSpace)** — add `tangent: vec4` to vertex format; read glTF `TANGENT` when present, generate via MikkTSpace otherwise. `mesh.frag` samples no normal map yet (terrain does, meshes don't), so add mesh normal-map sampling first. Triggered by the first model with a hand-authored normal map.
- **Shadow cascades** — replace the fixed world-space shadow box with view-frustum cascades for uniform shadow resolution regardless of map size. Fitting the single map to the camera view was tried and reverted: lost the free soft edges, and the box boundary flickered on zoom.
- **Rendering audit pass** — sweep for other "works because nothing's stressed it" shortcuts: ambient uniform, post-process pipeline, SSAO, anisotropic filtering. Promote individual findings out as they bite.
- Rich custom shader decorators (game-project art concern).

**Startup & UX**

- **Boot splash** — `Engine::init()` runs synchronously before the first frame, so launch shows a black/white window until it finishes. Present a splash during that gap. Must be engine-drawn (the UI layer isn't up yet — it's the thing being covered), customized only by data: the game ships a splash image; dev gets a fallback with no image.
- **`AppState::Splash`** — default-initial, App-drawn welcome state entered after init (game shows a branded splash + auto-advances; dev jumps to `Menu`). Independent of the boot splash above.
