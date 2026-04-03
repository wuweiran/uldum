# Uldum Engine — Model & Art Format Specification

## Overview

All models use **glTF 2.0** (`.gltf` or `.glb`). The engine extracts geometry, skeleton, and animation data via cgltf. Textures are **PNG**. Audio is **OGG Opus** (music) or **WAV** (SFX).

## Coordinate System

- **Game coordinates**: X = right, Y = forward, Z = up
- **glTF uses Y-up**: the engine applies a -90° X rotation at render time for non-native models
- Models authored in Z-up (Blender default) should export with "Y-up" glTF convention — the engine handles the conversion

## Model Structure

A glTF model can contain:

| Component | glTF Field | Required | Notes |
|-----------|-----------|----------|-------|
| Meshes | `meshes[]` | Yes | Triangle primitives only |
| Skeleton | `skins[0]` | No | First skin used. Up to 128 bones |
| Animations | `animations[]` | No | Matched to engine states by name |
| Materials | `materials[]` | Not yet | Engine uses placeholder textures currently |

## Vertex Format

### Non-Skinned (32 bytes)

| Attribute | Type | glTF Accessor |
|-----------|------|---------------|
| Position | vec3 | `POSITION` |
| Normal | vec3 | `NORMAL` |
| Texcoord | vec2 | `TEXCOORD_0` |

### Skinned (64 bytes)

| Attribute | Type | glTF Accessor |
|-----------|------|---------------|
| Position | vec3 | `POSITION` |
| Normal | vec3 | `NORMAL` |
| Texcoord | vec2 | `TEXCOORD_0` |
| Bone Indices | uvec4 | `JOINTS_0` |
| Bone Weights | vec4 | `WEIGHTS_0` |

A mesh is treated as skinned if the model has a `skins[]` entry AND the mesh primitive has `JOINTS_0` + `WEIGHTS_0` attributes.

## Skeleton

- Defined by `skins[0].joints[]` — array of node references
- Each joint's parent is found by walking up the glTF node tree
- `skins[0].inverseBindMatrices` provides the inverse bind pose for each joint
- **Maximum 128 bones** per skeleton (SSBO-based GPU skinning)
- 4 bone influences per vertex

## Animation Clips

Clips are glTF `animations[]` entries. The engine matches clips to gameplay states by the animation's `name` field (case-sensitive):

| Clip Name | Engine State | When Active | Looping |
|-----------|-------------|-------------|---------|
| `idle` | Standing still, no active orders | Default state | Yes |
| `walk` | Moving — Move order, AttackMove, or chasing an attack target | Movement or combat chase | Yes |
| `attack` | Attacking — full attack cycle (turn, wind up, swing, cooldown) | Combat system | No (restarts each swing) |
| `spell` | Casting an ability — during cast point and backswing | Ability cast system | No |
| `death` | Unit has died | Death system | No (holds last frame) |

### Behavior

- **Missing clips**: If a model doesn't have a clip for a state, the bind pose is used (no crash)
- **State transitions**: 0.15s crossfade blend between states
- **Playback**: Animations run at render framerate (not simulation tick rate). The engine reads simulation state each frame to determine which clip to play
- **Attack timing**: The `attack` clip should be authored to match the unit's `cast_point` + `backswing` timing from `unit_types.json`. The clip plays during WindUp and Backswing phases

### glTF Example

```json
{
    "animations": [
        {
            "name": "idle",
            "channels": [
                { "target": { "node": 1, "path": "rotation" }, "sampler": 0 }
            ],
            "samplers": [
                { "input": 0, "output": 1, "interpolation": "LINEAR" }
            ]
        },
        { "name": "walk", ... },
        { "name": "attack", ... },
        { "name": "death", ... }
    ]
}
```

## Effects

Effects are named presets for visual feedback (particles, future: model attachments, sprites). The engine provides defaults, maps can define or override.

### Effect Definition

Effects can be defined in three ways:

**1. Engine defaults** — built-in effects always available:

| Name | Description |
|------|-------------|
| `hit_spark` | Orange sparks on attack hit |
| `death_burst` | Red burst on unit death |
| `heal_glow` | Green upward particles on healing |
| `spell_cast` | Blue burst on ability cast |
| `blood_splat` | Dark red on heavy damage |
| `aura_glow` | Continuous blue glow (persistent) |

**2. Map-defined in Lua** — maps can define custom effects:

```lua
DefineEffect("holy_nova", {
    count = 25, speed = 200, life = 0.8, size = 12,
    start_color = {1, 1, 0.5, 1},
    end_color = {1, 0.8, 0, 0}
})
```

**3. Map-defined in JSON** — `effects.json` alongside type definitions:

```json
{
    "holy_nova": {
        "count": 25, "speed": 200, "life": 0.8, "size": 12,
        "start_color": { "r": 1, "g": 1, "b": 0.5, "a": 1 },
        "end_color": { "r": 1, "g": 0.8, "b": 0, "a": 0 }
    }
}
```

### Effect Lifecycle

Effects have two modes:

**Fire-and-forget** — plays once, auto-destroys:
```lua
PlayEffect("hit_spark", x, y, z)
PlayEffectOnUnit("death_burst", unit)
```

**Persistent** — stays until explicitly destroyed (auras, buffs, environmental):
```lua
local fx = CreateEffectOnUnit("aura_glow", unit)
-- Later:
DestroyEffect(fx)
```

Persistent effects attached to a unit automatically follow the unit's position.

### Model-Attached Effects (Future)

Defined in a sidecar JSON file alongside the glTF model (e.g. `footman.effects.json`). Effects are attached to bones and triggered by animation events:

```json
{
    "effects": [
        {
            "name": "weapon_trail",
            "bone": "weapon_r",
            "effect": "hit_spark",
            "trigger": { "animation": "attack", "start": 0.2, "end": 0.4 }
        }
    ]
}
```

## Textures

- **Format**: PNG (development), KTX2 with BC7/ASTC (shipping — Phase 13)
- **Unit textures**: Referenced by glTF material (not yet implemented — engine uses placeholder colors)
- **Terrain textures**: 4-layer splatmap, defined in `tileset.json`

### Texture Pipeline (Future)

| Stage | Format | Tool |
|-------|--------|------|
| Authoring | PNG / TGA | Any image editor |
| Development | PNG | Loaded via stb_image |
| Shipping | KTX2 (BC7 desktop, ASTC mobile) | Asset baking pipeline (Phase 13) |

## Audio

- **Music / voice**: OGG Opus (streaming)
- **SFX**: WAV (no decompression latency) or OGG Opus
- **3D positional**: Per-unit sound sources with distance attenuation (Phase 11)

## Unit Type Model Reference

In `unit_types.json`, the `model` field points to the glTF file:

```json
{
    "footman": {
        "model": "models/units/footman.gltf",
        ...
    }
}
```

The engine searches for the model relative to the map's asset directory first, then falls back to the engine's asset directory. If the model fails to load, a procedural placeholder (box with 2-bone skeleton) is used.

## Scale

All units use WC3-style scale:

| Measurement | Typical Value |
|------------|---------------|
| Tile size | 128 game units |
| Unit collision radius | 20 game units |
| Unit height | ~64 game units |
| Melee attack range | 128 game units |
| Ranged attack range | 500-800 game units |
| Map size | 8192 x 8192 (64x64 tiles) |

Models should be authored at this scale, or use `transform.scale` in the unit type definition to adjust.
