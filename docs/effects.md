# Effect System Design

## Overview

The effect system renders visual effects attached to units or positions. Maps
define all effect content as named, parameterized **presets**; the engine owns
the **rendering backends** that draw them. The two are separate layers — a preset
picks a *phenomenon* (`type`), and the engine decides how to render it.

An effect's `type` names an orthogonal visual phenomenon (not a particle shape):

| `type` | Backend | What it is |
|---|---|---|
| `spark` | particle | Energetic bright bits — impacts, magic, soft auras. |
| `spray` | particle | A liquid arc — blood or water (the color decides which). |
| `glow`  | **glow system** | Engine-owned light visual: rising volumetric Tyndall light shafts today (WC3 level-up look); the home for future light effects like persistent "hero glow". No texture knob — the look is procedural. |

Particle **shape** (the soft orb / teardrop sprite) is an internal engine detail
— authors never pick it. You pick the phenomenon; the engine maps it to a shape
(or to the glow backend). One effect = exactly one `type`; no composition.

## Effect Definition

Effect definitions live in `types/effects.json` inside each `.uldmap`. They're
loaded by both host and client at session start, so the registries stay symmetric
— there's no Lua-side `DefineEffect` API, by design.

```json
{
    "fire_burst": {
        "type": "spark",
        "count": 15, "speed": 120, "life": 0.5, "size": 8, "gravity": -100,
        "color": { "r": 1, "g": 0.6, "b": 0.1, "a": 1 }
    },
    "level_up": {
        "type": "glow",
        "height": 240, "radius": 26, "life": 1.3, "tyndall": 0.65, "intensity": 1.2,
        "color": { "r": 1.0, "g": 0.92, "b": 0.55, "a": 1.0 }
    }
}
```

### Common fields (all types)

| Field | Default | Meaning |
|---|---|---|
| `type` | `spark` | The phenomenon: `spark`, `spray`, or `glow`. Selects the backend + param set below. |

Color: every type takes a single **`color`** (RGBA). For particles (`spark`/
`spray`) the particle spawns at this color and its **alpha fades linearly to 0**
over each particle's `life` — a built-in disappearance, no second color needed.
For `glow` it's the steady shaft/light tint; brightness rises and falls via the
fade envelope (`fade_in`/`fade_out`).

### Particle fields (`spark` / `spray`)

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

### Glow fields (`glow`)

Engine-owned light shaft; no texture/shape knob — the look is procedural. A
single **static** vertical shaft (no motion) with one **`color`**, lit by a
Tyndall scattering envelope: it blooms in, then as it dies the scattering halo
**retracts toward the bright core and weakens** (not a flat alpha fade) — light
scattering away, like a door closing in the dark. Emits one point light while it
plays (reach + brightness derived from `radius` + `intensity`).

| Field | Default | Meaning |
|---|---|---|
| `color` | — | RGBA tint of the shaft + the light it casts |
| `height` | 220 | Shaft height (world units) |
| `radius` | 24 | Beam width (world units); also scales the light's reach |
| `life` | 1.2 | Total fade-in→hold→fade-out duration (seconds) |
| `fade_in` | 0 | Seconds to ramp on (linear). `0` = half of `life` |
| `fade_out` | 0 | Seconds to ramp off (linear). `0` = half of `life` |
| `tyndall` | 0.6 | Volumetric striation strength (`0` = clean shaft) |
| `intensity` | 1.0 | Brightness of the beam and the light it casts |

With `fade_in`/`fade_out` both unset the glow uses a symmetric ramp (half `life`
each). Set them in seconds to control on/off speed independently — e.g.
`"fade_in": 0.1, "fade_out": 0.6` snaps on then drifts away. If the two together
exceed `life` they're scaled to fit (the peak lands where they meet).

(see `src/render/effect.h` for the full structs)

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
