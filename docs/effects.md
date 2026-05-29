# Effect System Design

## Overview

The effect system renders visual effects attached to units or positions. Maps define all effect content — the engine provides particle rendering. Effects are currently particle-only; model-based effects are not yet implemented.

## Effect Definition

Effect definitions live in `types/effects.json` inside each `.uldmap` (and engine-shipped effects in `engine/types/effects.json`). They're loaded by both host and client at session start, so the registries stay symmetric — there's no Lua-side `DefineEffect` API, by design.

```json
{
    "fire_burst": {
        "count": 15, "speed": 120, "life": 0.5, "size": 8, "gravity": -100,
        "texture": "spark",
        "start_color": { "r": 1, "g": 0.6, "b": 0.1, "a": 1 },
        "end_color":   { "r": 1, "g": 0.2, "b": 0,   "a": 0 }
    },
    "aura_glow": {
        "count": 1, "emit_rate": 10, "speed": 30, "life": 0.8, "size": 5,
        "gravity": 40,
        "start_color": { "r": 0.4, "g": 0.6, "b": 1, "a": 0.6 },
        "end_color":   { "r": 0.2, "g": 0.3, "b": 0.8, "a": 0 }
    }
}
```

Fields (see `src/render/effect.h` for the full struct):

| Field | Default | Meaning |
|---|---|---|
| `count` | 10 | Particles per burst (or per continuous tick if `emit_rate > 0`) |
| `emit_rate` | 0 | Particles per second; `0` = one-shot burst |
| `speed` | 100 | Initial particle speed |
| `life` | 0.5 | Seconds each particle lives |
| `size` | 8 | World units |
| `gravity` | -200 | Per-second downward acceleration |
| `spread` | 1.0 | `0` = straight up, `1` = full sphere |
| `radius` | 0 | `> 0` = ring emitter, particles arranged on a horizontal circle |
| `texture` | (default soft circle) | Built-in name: `spark`, `blood`, `glow`, `droplet` |
| `start_color`, `end_color` | yellow → red | RGBA at birth and at death |

## Lua API

```lua
-- Fire-and-forget at a world position. `opts.players` (optional) limits
-- visibility to one player or a list of players; omit for broadcast.
PlayEffect("fire_burst", x, y, z, opts?)

-- Fire-and-forget on a unit, optionally at a named attachment point.
PlayEffectOnUnit("fire_burst", unit)
PlayEffectOnUnit("fire_burst", unit, "right_hand")

-- Persistent effect at a world position. Returns handle (0 on failure).
local fx = CreateEffect("aura_glow", x, y, z, opts?)
DestroyEffect(fx)

-- Persistent effect on a unit. Follows the unit's transform each frame.
local fx = CreateEffectOnUnit("aura_glow", unit)
local fx = CreateEffectOnUnit("aura_glow", unit, "overhead")
DestroyEffect(fx)
```

Engine-shipped effects available without map registration: `hit_spark`, `death_burst`, `heal_glow`, `spell_cast`, `blood_splat`, `aura_glow`.

## Attachment Points

Unit models define attachment points as named bones in the glTF skeleton. The engine looks up these bones by name and reads their world-space transform each frame.

### Standard Attachment Points

| Name | Location | Typical Use |
|------|----------|-------------|
| `origin` | Model origin (feet) | Ground effects, birth effects |
| `chest` | Center of torso | Hit effects, buff visuals |
| `overhead` | Above head | Status indicators, heal effects |
| `right_hand` | Right hand | Weapon trails, attack effects |
| `left_hand` | Left hand | Shield effects, off-hand |

Maps may use any bone name — these are conventions, not engine requirements. If the requested attachment point doesn't exist, the effect falls back to the model origin.

### Creating Attachment Points in Blender

1. Select the armature, enter Edit Mode
2. Add a new bone at the desired position
3. Parent it to the appropriate body part bone (e.g., hand bone)
4. Name it with a standard name (e.g., `right_hand`, `chest`, `overhead`)
5. Export as glTF — the bone becomes a skeleton joint

The engine reads the bone's animated world-space transform each frame, so attached effects follow the unit's animation pose.

## Engine Responsibilities

The engine ships a small set of common effects (`hit_spark`, `death_burst`, etc., listed above). Maps can override them or add new ones via their own `types/effects.json`. The engine provides:

- **Particle renderer**: takes emitter parameters, renders textured alpha-blended billboards
- **Attachment point system**: looks up named bones in unit skeletons, provides world-space transforms each frame
- **Effect lifecycle**: create, update (follow unit/bone), destroy

## Map Responsibilities

Maps define their gameplay-specific visual content:

- Add or override effect definitions in `types/effects.json`
- Wire effects to gameplay via Lua triggers (`EVENT_GLOBAL_DAMAGE`, `EVENT_GLOBAL_DEATH`, `EVENT_GLOBAL_ABILITY_EFFECT`, etc.)
- Choose attachment points per effect call
