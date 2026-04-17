# Uldum Engine — Build Targets

## Overview

The Uldum engine is not a product — it is the runtime underneath products. A shipped game is a map package with custom UI, scripts, and assets, configured via `game.json`. The engine is invisible to end users.

Four build targets serve different audiences:

| Target | Audience | Purpose |
|--------|----------|---------|
| `uldum_dev` | Developers | Test any map, debug overlay, dev console |
| `uldum_editor` | Map makers | Terrain sculpt, object placement, save/load |
| `uldum_game` | End users | Shipped game, loads one map, no debug UI |
| `uldum_server` | Multiplayer | Headless authoritative simulation |

## Targets

### `uldum_dev` — Developer Runtime

What developers use day-to-day. Loads maps, runs full gameplay, shows debug info.

- Loads `maps/test_map.uldmap` by default (or any map via command line)
- In-process local server for single player (no network latency)
- Debug overlay: FPS, entity count, pathing grid, spatial grid visualization
- Dev console (toggle with ~): execute Lua, inspect state, switch maps
- Hot-reload Lua scripts on file change
- Free camera (not locked to map camera restrictions)
- Full gameplay: simulation, combat, abilities, scripts, AI

**This is what the current `uldum.exe` is.** Phase 10+ adds debug overlay and dev console.

### `uldum_editor` — Map Editor

Desktop-only tool for creating and editing `.uldmap` packages.

- ImGui-based UI: toolbar, property inspector, terrain tools, object palette
- Terrain editing: sculpt (raise/lower/smooth/flatten), paint textures, set cliff levels, place water, mark pathing
- Object editing: place/move/delete units, destructables, items, regions, cameras
- Type editing: edit unit_types.json, ability_types.json in the UI
- Save/Load: read and write all map files (terrain.bin, objects.json, manifest.json)
- Test play: launch `uldum_dev` with current map as a separate process
- No gameplay simulation (no combat, no AI, no scripts running)
- No network

### `uldum_game` — Shipped Game

What end users run. A specific game product.

- Reads `game.json` for configuration (map path, window settings, title)
- Loads exactly one map — the game's main map
- Shows the map's custom UI (Lua-driven menus, HUD, etc.)
- Connects to server (remote for multiplayer, in-process for single player)
- No dev console, no debug overlay, no map switching
- Release build: no validation layers, optimized

A publisher ships:
```
MyGame/
├── mygame.exe            (uldum_game, renamed)
├── game.json             { "map": "maps/main.uldmap", "title": "My Game" }
├── maps/main.uldmap/     game content
└── engine/               shaders, engine scripts, default assets
```

### `uldum_server` — Dedicated Server

Headless multiplayer server.

- No window, no renderer, no audio
- Loads map, runs authoritative simulation at 32 Hz
- Accepts client connections via ENet
- Executes map Lua scripts (game logic, triggers, AI)
- Reads server config from `game.json` or command-line args
- Minimal platform layer (filesystem + networking only)

## Library Architecture

```
Shared libraries (all targets may link):
  uldum_core          Types, math, logging, allocators
  uldum_asset         Resource loading (glTF, KTX2, JSON)
  uldum_map           Map format, terrain data, type definitions
  uldum_simulation    ECS, units, pathfinding, combat, abilities
  uldum_script        Lua VM, engine API bindings, triggers
  uldum_network       ENet client/server, state sync
  uldum_rhi           Vulkan 1.3 abstraction
  uldum_render        Pipelines, terrain mesh, animation, particles, effects
  uldum_audio         3D positional audio (miniaudio)

Platform libraries (one per OS):
  uldum_platform_windows    Win32 window, input, filesystem
  uldum_platform_android    GameActivity, touch input, APK filesystem
  uldum_platform_headless   No window, minimal filesystem (for server)

Editor executable (not a library — only the editor target uses this code):
  uldum_editor      ImGui panels, terrain tools, object tools, gizmos
```

## Link Matrix

```
                    core asset map  sim  script net  rhi  render audio platform       imgui
uldum_dev            *    *    *    *     *     *    *     *      *   windows
uldum_editor         *    *    *    *                *     *          windows          *
uldum_game           *    *    *    *     *     *    *     *      *   windows/android
uldum_server         *    *    *    *     *     *                     headless
```

## Source Layout

```
src/
├── core/              Platform-independent
├── asset/             Platform-independent
├── map/               Platform-independent
├── simulation/        Platform-independent
├── script/            Platform-independent
├── network/           Platform-independent
├── rhi/               Vulkan 1.3 (Windows + Android)
├── render/            Platform-independent (uses rhi/)
├── audio/             Platform-independent (miniaudio)
│
├── platform/
│   ├── platform.h         Abstract interface
│   ├── windows/           Win32 implementation
│   ├── android/           GameActivity (future)
│   └── headless/          Server stub (future)
│
├── editor/            Editor executable (ImGui tools + entry point)
│   ├── main.cpp           uldum_editor entry point
│   └── editor.h/cpp       Editor core, terrain tools, object tools
│
└── app/               Game engine + entry points
    ├── engine.h/cpp       Shared engine core (init, tick, render, shutdown)
    ├── dev_main.cpp       uldum_dev entry point
    ├── game_main.cpp      uldum_game entry point (stub)
    └── server_main.cpp    uldum_server entry point (stub)
```

## `game.json` — Game Configuration

```json
{
    "title": "My RTS Game",
    "version": "1.0.0",
    "map": "maps/main.uldmap",
    "window": {
        "width": 1920,
        "height": 1080,
        "fullscreen": false
    },
    "server": {
        "tick_rate": 32,
        "max_players": 8,
        "port": 7777
    }
}
```

Read by `uldum_game` at startup. `uldum_dev` ignores it (loads maps via command line or default). `uldum_server` reads map path and server config from it.

## Platform Matrix

| Target | Windows | Android | Linux (future) |
|--------|---------|---------|----------------|
| `uldum_dev` | .exe | - | - |
| `uldum_editor` | .exe | - | - |
| `uldum_game` | .exe | .apk | .elf |
| `uldum_server` | .exe | - | .elf |

## Phased Rollout

| Phase | Targets Available |
|-------|-------------------|
| Phase 9 (current) | `uldum_dev` only |
| Phase 10 (Editor) | + `uldum_editor` |
| Phase 12 (Network) | + `uldum_server` |
| Phase 13 (Packaging) | + `uldum_game`, Android, game.json |
