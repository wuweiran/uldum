# Effect System Design

## Overview

The effect system renders visual effects attached to units, positions, or abilities. Maps define all effect content — the engine provides particle rendering and model effect rendering. Lua scripts create and control effects without knowing the underlying implementation (particles vs model).

## Effect Types

### Particle Effects

Defined by emitter parameters: count, speed, lifetime, size, color, gravity, texture, emission rate. The engine renders them as textured billboards.

### Model Effects

A glTF model used as a visual effect (spinning rune circle, floating orb, weapon glow). The engine loads the model and plays its default animation (first clip). No special animation name required.

## Effect Definition

Maps register effects by name. Each effect is either particle-based or model-based:

```lua
-- Particle effect
DefineEffect("fire_burst", {
    type = "particle",
    count = 15,
    speed = 120,
    life = 0.5,
    size = 8,
    gravity = -100,
    texture = "textures/fire_particle.png",  -- optional, solid color if omitted
    start_color = { r = 1, g = 0.6, b = 0.1, a = 1 },
    end_color = { r = 1, g = 0.2, b = 0, a = 0 },
})

-- Continuous emitter
DefineEffect("aura_glow", {
    type = "particle",
    emit_rate = 10,
    speed = 30,
    life = 0.8,
    size = 5,
    gravity = 40,
    start_color = { r = 0.4, g = 0.6, b = 1, a = 0.6 },
    end_color = { r = 0.2, g = 0.3, b = 0.8, a = 0 },
})

-- Model effect
DefineEffect("holy_light_beam", {
    type = "model",
    model = "effects/holy_beam.glb",
    scale = 1.0,
})
```

Effects can also be defined in JSON files and loaded by the map.

## Lua API

The API is unified — same functions for particle and model effects:

```lua
-- Fire-and-forget at a position
PlayEffect("fire_burst", x, y)

-- Fire-and-forget on a unit (at attachment point)
PlayEffectOnUnit("fire_burst", unit)
PlayEffectOnUnit("fire_burst", unit, "right_hand")

-- Persistent effect (returns handle for later removal)
local fx = CreateEffect("aura_glow", unit)
local fx = CreateEffect("aura_glow", unit, "overhead")
DestroyEffect(fx)

-- Persistent effect at a position
local fx = CreateEffectAtPoint("campfire", x, y)
DestroyEffect(fx)
```

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

The engine does not define any effects. It provides:

- **Particle renderer**: takes emitter parameters, renders textured alpha-blended billboards
- **Model effect renderer**: loads a glTF model, plays its default animation, renders with the mesh pipeline
- **Attachment point system**: looks up named bones in unit skeletons, provides world-space transforms
- **Effect lifecycle**: create, update (follow unit/bone), destroy with optional fade

## Map Responsibilities

Maps define all visual effect content:

- Register effects via Lua `DefineEffect` or JSON files
- Provide particle textures and effect model files in `shared/assets/`
- Wire effects to gameplay via Lua triggers (on_damage, on_death, on_cast, etc.)
- Define which attachment points to use per effect
