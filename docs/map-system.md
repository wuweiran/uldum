# Uldum Engine — Map System Design

## Overview

A **map** in Uldum is a self-contained gameplay package — equivalent to a WC3 map (`.w3x`). It defines terrain, units, abilities, assets, scripts, and all gameplay logic. The engine provides mechanical infrastructure; the map provides content and rules.

Maps are distributed as `.uldmap` directories (or packed archives in Phase 12).

## Terminology

- **Map**: a gameplay package. Analogous to a WC3 map or a "mod."
- **Scene**: a terrain + placement set within a map. A map contains 1+ scenes. Addresses WC3's limitation of one terrain per map.
- **Tileset**: a set of ground textures used by terrain (grass, dirt, stone, etc.).
- **State**: a depletable/regenerating resource on a unit (HP, mana, energy). Has `current`, `max`, `regen`.
- **Attribute**: a single-value modifier on a unit (strength, agility, intelligence). Does not deplete or regenerate.
- **Classification**: a string-based flag on a unit used for targeting filters (e.g., "ground", "air", "hero", "structure"). Map-defined.

## Engine vs Map Boundary

### Engine provides (C++ mechanics)

| Concept | Details |
|---------|---------|
| Entity categories | Unit, Destructable, Item, Doodad, Projectile — fixed in engine |
| Order system | Move, Attack, Cast, Stop, Patrol, etc. — engine processes these |
| Movement + pathfinding | Preset move types: Ground, Air, Amphibious. Engine handles pathing |
| Classification system | Targeting filter infrastructure. **Map defines the actual flags** (string-based) |
| HP (hit points) | Built-in state: current, max, regen. Engine fires death event at 0 |
| State system | Additional depletable resources (mana, energy, etc.). Map declares which states exist. Engine ticks regen, checks cost for ability casts |
| Attribute system | Single-value modifiers (strength, agility, etc.). All map-defined. Modified by abilities/items/levels |
| Normal attack | Melee/ranged attack flow: dmg_time (fore-swing), backsw_time (recovery), attack cooldown, projectile launch |
| Projectile system | Flight, homing, arc, collision detection, impact event |
| Ability system | ~10 cast flow forms (instant, target_unit, target_point, passive, aura, toggle, channel, etc.). Cooldown, range check, state cost check. Applied abilities (WC3 "buffs") are abilities with duration + auto-remove |
| Collision / spatial queries | `units_in_range()`, `units_in_rect()`, etc. |
| Event system | on_damage, on_death, on_cast, on_attack, etc. Map Lua hooks these |
| Damage event flow | Engine fires `on_damage(source, target, amount, context)`. Map Lua can modify amount (apply its own attack/armor logic). Engine applies final HP change |
| Cooldown ticking | Decrement ability cooldowns each tick |

### Map defines (gameplay data + logic)

| Concept | Details |
|---------|---------|
| Classifications | String-based flags: "ground", "air", "hero", "structure", "mechanical", etc. |
| Attack types | String-based: "normal", "pierce", "siege", "magic", "chaos", etc. |
| Armor types | String-based: "unarmored", "light", "heavy", "fortified", etc. |
| States (beyond HP) | Map declares: mana, energy, rage, etc. with max/regen per unit type |
| Attributes | Map declares: strength, agility, intelligence, etc. |
| Damage logic | Map Lua hooks `on_damage` to apply attack/armor multipliers |
| Unit types | JSON definitions with states, attributes, classifications, abilities |
| Ability types | JSON + Lua. Includes applied abilities (WC3 "buffs": passive, with duration) |
| Item types | JSON definitions |
| Destructable types | JSON definitions |
| UI layout | Map declares layout structure (panels, positions). Engine/UI framework renders |
| Tileset | Ground texture definitions |
| Terrain + placements | Heightmap, tile types, pathing, preplaced objects |
| Scripts | Lua: triggers, damage formulas, custom ability effects, game mode logic |
| Custom assets | Models, textures, audio, icons |

### Engine config (non-gameplay only)

| Contents | Details |
|----------|---------|
| `engine.json` | tick_rate, max_units, vsync, max_draw_calls |
| `shaders/` | Rendering shaders |
| `scripts/` | Core Lua API library (functions exposed to map scripts) |
| `textures/` | Fallback textures only (default white, missing texture) |

No unit types, ability types, or gameplay data in engine config.

## Map Package Structure

```
my_map.uldmap/
├── manifest.json               # metadata, players, teams, game mode
├── tileset.json                # ground texture definitions
│
├── types/                      # all gameplay type definitions
│   ├── unit_types.json
│   ├── ability_types.json      # all abilities: active, passive, auras, applied (WC3 "buffs")
│   ├── item_types.json
│   └── destructable_types.json
│
├── shared/                     # shared across all scenes
│   ├── scripts/
│   │   ├── main.lua            # map entry point
│   │   └── ...
│   └── assets/
│       ├── models/
│       ├── textures/
│       ├── audio/
│       └── icons/              # referenced by ability/item definitions
│
└── scenes/
    ├── scene_01/               # each scene = one terrain + placements
    │   ├── terrain.bin         # heightmap, tile types, pathing
    │   ├── objects.json        # preplaced units, destructables, doodads, regions, cameras
    │   └── scripts/            # scene-specific triggers (optional)
    └── scene_02/
        └── ...
```

A simple melee map has one scene. Complex maps (RPGs, campaigns) use multiple scenes with transitions via Lua API.

## Manifest (`manifest.json`)

```json
{
    "name": "Battlegrounds",
    "author": "MapMaker",
    "description": "A 4-player melee map with varied terrain.",
    "version": "1.0.0",
    "engine_version": "0.1.0",
    "game_mode": "melee",
    "suggested_players": "2-4",
    "loading_screen": {
        "text": "Prepare for battle!",
        "image": "assets/textures/loading.ktx2"
    },
    "players": [
        { "slot": 0, "type": "human", "team": 0, "name": "Player 1", "color": "red" },
        { "slot": 1, "type": "human", "team": 1, "name": "Player 2", "color": "blue" },
        { "slot": 2, "type": "computer", "team": 0, "name": "Computer 1", "color": "teal" },
        { "slot": 3, "type": "computer", "team": 1, "name": "Computer 2", "color": "purple" }
    ],
    "teams": [
        { "id": 0, "name": "Sentinels", "allied": true, "shared_vision": true },
        { "id": 1, "name": "Scourge", "allied": true, "shared_vision": true }
    ],
    "tileset": "tileset.json",
    "start_scene": "scene_01",
    "vital_state": "health",

    "classifications": ["ground", "air", "hero", "structure", "mechanical", "undead", "worker", "summoned"],
    "attack_types": ["normal", "pierce", "siege", "magic", "chaos", "hero"],
    "armor_types": ["unarmored", "light", "medium", "heavy", "fortified", "hero"],
    "states": [
        { "id": "health", "has_max": true, "has_regen": true },
        { "id": "mana", "has_max": true, "has_regen": true }
    ],
    "attributes": ["strength", "agility", "intelligence"],

    "ui_layout": "ui/layout.json"
}
```

The manifest declares all map-specific enumerations: classifications, attack/armor types, states, and attributes. The engine reads these at map load and uses them for validation and targeting filters.

## Tileset (`tileset.json`)

```json
{
    "name": "Ashenvale",
    "layers": [
        { "id": 0, "name": "grass",  "texture": "assets/textures/terrain/grass.ktx2" },
        { "id": 1, "name": "dirt",   "texture": "assets/textures/terrain/dirt.ktx2" },
        { "id": 2, "name": "stone",  "texture": "assets/textures/terrain/stone.ktx2" },
        { "id": 3, "name": "sand",   "texture": "assets/textures/terrain/sand.ktx2" }
    ]
}
```

## Scene: Terrain (`terrain.bin`)

Binary format (see `docs/terrain.md` for full spec):
- Heightmap: per-vertex float array
- Tile types: per-tile u8 (indexes into tileset layers)
- Pathing: per-tile u8 bitfield (walkable, flyable, buildable)

## Scene: Object Placements (`objects.json`)

```json
{
    "units": [
        { "type": "footman", "x": 40.0, "y": 50.0, "facing": 0.0, "owner": 0 },
        { "type": "paladin", "x": 42.0, "y": 50.0, "facing": 0.0, "owner": 0 }
    ],
    "destructables": [
        { "type": "tree_ashenvale", "x": 30.0, "y": 30.0, "facing": 0.0, "variation": 0 }
    ],
    "items": [
        { "type": "potion_healing", "x": 50.0, "y": 50.0 }
    ],
    "doodads": [
        { "model": "models/doodads/flower.gltf", "x": 20.0, "y": 20.0, "z": 0.0, "facing": 0.0, "scale": 1.0 }
    ],
    "regions": [
        { "name": "spawn_area_1", "x": 10.0, "y": 10.0, "width": 20.0, "height": 20.0 }
    ],
    "cameras": [
        { "name": "intro_camera", "x": 64.0, "y": 64.0, "z": 30.0, "pitch": -0.8, "yaw": 0.0 }
    ]
}
```

## Type Definitions

All type definitions live in `types/`. See `docs/gameplay-model.md` for detailed schemas.

Key changes from earlier design:
- **No engine defaults** — maps define all types from scratch
- **Attack/armor types are strings** — map declares valid values in manifest
- **States are map-defined** — HP is built-in; map adds mana, energy, etc.
- **Attributes are map-defined** — strength, agility, etc. are strings
- **No separate buff system** — WC3 "buffs" are just applied abilities (passive, with duration and auto-remove)
- **Damage logic is in Lua** — map hooks `on_damage` event, applies its own multipliers

## Multi-Scene Support

### Why

WC3's one-terrain-per-map limitation forces campaigns into sequential map lists with no shared state. Uldum's scene system allows:
- RPG maps with town → dungeon → boss transitions
- Campaign-style progression within a single map package
- Shared hero state, inventory, quest flags across scenes

### How

```lua
-- In map Lua script:
-- Save state before transition
local hero_data = SaveHeroState(my_hero)

-- Load new scene
LoadScene("scene_02")

-- After scene loads, restore state
local hero = CreateUnit("paladin", player_1, 10, 10)
RestoreHeroState(hero, hero_data)
```

- `LoadScene(name)` unloads current terrain + placements, loads new scene
- Shared scripts and assets remain loaded (they're at package level)
- Global Lua state persists across scene transitions
- Map scripts manage what state to carry over

## Map Loading Flow

1. Engine reads `manifest.json`
2. Engine registers map-defined classifications, attack/armor types, states, attributes
3. Engine loads `tileset.json`
4. Engine loads all type definitions from `types/`
5. Engine loads the start scene:
   a. Load `terrain.bin` → build terrain mesh, set up pathing
   b. Load `objects.json` → create preplaced entities
6. Engine loads `shared/scripts/main.lua` → map initialization runs
7. Game loop begins

## Map Unloading

1. Fire `on_map_unload` event (Lua cleanup)
2. Destroy all entities
3. Unload terrain
4. Unload custom assets
5. Clear type registries
6. Reset Lua state
