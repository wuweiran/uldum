# Uldum Engine — Model & Art Format Specification

## Overview

All models use **glTF 2.0** (`.gltf` or `.glb`). The engine extracts geometry, skeleton, and animation data via cgltf. Standalone textures are **KTX2 + Basis Universal** (authored from PNG via `basisu` — see [packaging.md](packaging.md)). Audio is **OGG Opus** (music) or **WAV** (SFX).

## Coordinate System

- **Game coordinates**: X = right, Y = forward, Z = up
- **glTF uses Y-up**: the engine applies a -90° X rotation at render time for non-native models
- Models authored in Z-up (Blender default) should export with "Y-up" glTF convention — the engine handles the conversion

## Model Structure

A glTF model can contain:

| Component | glTF Field | Required | Notes |
|-----------|-----------|----------|-------|
| Meshes | `meshes[]` | Yes | Triangle primitives only. Each primitive becomes its own draw with its own material (static models). |
| Skeleton | `skins[0]` | No | First skin used. Up to 128 bones |
| Animations | `animations[]` | No | Matched to engine states by name |
| Materials | `materials[]` | No | Per-primitive on both static and skinned models (texture + baseColorFactor + alphaMode + doubleSided). Each primitive draws with its own material. |

## Supported subset of glTF 2.0

The loader is tuned for skeletal-animated unit models. Authors hitting an unsupported feature get a `[WARN ] [Asset]` log line at load time so the limit doesn't go unnoticed.

### Supported

- Geometry: `TRIANGLES` primitives only. Indexed and non-indexed both work.
- Node transforms: translation / rotation / scale (or a node `matrix`), composed up the hierarchy. Static meshes bake the node's world transform into their vertices; skinned meshes are placed by the skeleton. (See [Scale → Node transforms](#node-transforms-sizing--placing-a-model).)
- Vertex attributes: `POSITION` (required), `NORMAL`, `TEXCOORD_0`, `JOINTS_0`, `WEIGHTS_0`.
- Skin: first `skins[0]` only. Up to 128 bones. 4 bone influences per vertex.
- Bone-parented un-skinned meshes (e.g., a weapon node parented to a hand bone) are auto-converted to skinned with 100% weight on the parent bone.
- Animations: `translation`, `rotation`, `scale` channels. Played as linear regardless of the sampler's authored interpolation.
- Textures: PNG / JPEG / BMP / TGA / HDR (via stb_image), KTX2 / Basis Universal (via the engine's transcoder). Embedded in buffer-views or referenced by external URI. KTX2 detection works via the `KHR_texture_basisu` extension's mime type, the KTX2 magic prefix, or a `.ktx2` URI extension.
- **Materials (static models, per-primitive):**
  - `pbrMetallicRoughness.baseColorTexture` → bound as the primitive's diffuse texture.
  - `pbrMetallicRoughness.baseColorFactor` → per-instance multiplier in the fragment shader. Modulates the diffuse texture, or supplies a flat color for color-only primitives.
  - `alphaMode`: `OPAQUE` and `MASK` (alpha-test discard with `alphaCutoff`) both fully supported, including correctly shaped shadows for masked geometry. `BLEND` degrades to `OPAQUE` with a warning (no sorted transparent pass yet).
  - `doubleSided` → flips the cull mode per draw (no-cull for double-sided primitives, back-cull otherwise). Both pass and shadow honor this.
- **Materials (skinned models):** per-primitive, same as static — each submesh binds its own diffuse texture and `baseColorFactor`. Color-only materials (factor, no texture) render against a white default. `alphaMode` / `doubleSided` are read but the skinned pass draws Opaque + back-cull regardless (per-submesh alpha/cull not yet honored on skinned).

### Not supported

These are silently ignored unless a warning is noted:

- Primitive types other than `TRIANGLES` (warning emitted, primitive skipped).
- Additional UV sets (`TEXCOORD_1+`), additional joint/weight sets — warnings emitted; only the first set is used.
- Vertex colors (`COLOR_0`), `TANGENT` attribute.
- Morph targets and morph-target animation channels.
- Animation sampler interpolation modes `STEP` and `CUBICSPLINE` (warning emitted; played as linear).
- Animations on un-skinned models (warning emitted; channels dropped).
- Skins beyond `skins[0]` (warning emitted).
- PBR shading: `roughness`, `metallic`, normal map, occlusion, emissive, all `KHR_materials_*` extensions, and `KHR_materials_pbrSpecularGlossiness` (legacy). The fragment shader is Lambert + ambient + shadow + point lights; PBR is deferred work.
- `alphaMode: BLEND` (true transparency with depth-sorted draws). Degrades to `OPAQUE` with a per-material warning.
- `KHR_draco_mesh_compression`, `KHR_lights_punctual`, camera nodes, scene roots — the loader walks all nodes regardless of which scene they're in.

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

## Attachment Points

Effects and projectiles attach to a unit at **named bones** in the glTF
skeleton — the engine looks up the bone by name and reads its animated
world-space transform each frame, so the attached visual follows the pose. Add
these as extra bones in the rig (named exactly), parented to the relevant body
part. If a requested name doesn't exist, the engine falls back to the model
origin (no crash).

| Name | Location | Typical use |
|------|----------|-------------|
| `origin` | Model origin (feet / base) | Ground effects, birth |
| `chest` | Center of torso | Hit effects, buffs |
| `overhead` | Above the head | Status icons, heal glow |
| `right_hand` | Right hand | Weapon trails, attack effects |
| `left_hand` | Left hand | Shield / off-hand effects |

These are conventions, not hard requirements — a map may reference any bone
name. See [effects.md](effects.md) for the full attachment + effect API.

## Animation Clips

Clips are glTF `animations[]` entries. The engine matches clips to gameplay states by the animation's `name` field (case-sensitive):

| Clip Name | Engine State | When Active | Looping |
|-----------|-------------|-------------|---------|
| `idle` | Standing still, no active orders | Default state | Yes |
| `walk` | Moving — Move order, AttackMove, or chasing an attack target | Movement or combat chase | Yes |
| `attack` | Attacking — plays during WindUp + Backswing phases, scaled to `attack_cooldown` | Combat system | No (restarts each swing) |
| `spell` | Casting an ability — plays during CastPoint + Backswing phases | Ability cast system | No |
| `hit` | Hit by a normal attack while otherwise idle — brief recoil/flinch (WC3 "Stand Hit") | Idle widgets, normal-attack hits only | No (one-shot) |
| `death` | Unit has died | Death system | No (holds last frame) |

### Behavior

- **Missing clips**: If a model doesn't have a clip for a state, the bind pose is used (no crash)
- **State transitions**: 0.15s crossfade blend between states
- **Playback**: Animations run at render framerate (not simulation tick rate). The engine reads simulation state each frame to determine which clip to play
- **Attack timing**: The `attack` clip is uniformly scaled to fit `attack_cooldown`. The `animation.dmg_pt` fraction (from `units.json`) defines where the visual hit lands in the clip. The engine uses `combat.dmg_time` and `combat.backsw_time` (seconds) for gameplay timing
- **Spell timing**: The `spell` clip uses two-phase scaling. The `animation.cast_pt` fraction defines where the visual cast fires. The ability's `cast_time` and `backsw_time` (seconds) control gameplay timing
- **Hit flinch**: `hit` plays only on an otherwise-idle widget struck by a **normal attack** — spells, damage-over-time and splash don't flinch. Walking, attacking, casting and dying always outrank it, so a busy unit never flinches (no jitter, no throttle). Authoring it is optional; with no `hit` clip the widget simply doesn't recoil. Best for destructibles (crate/tree wobble) and idle units. Plays once for the clip's own length, then returns to `idle` — a new hit retriggers it.
- **Flying units**: There is **no separate `fly` state** (this matches WC3). A flyer is a gameplay/movement property of the unit, not an animation tag — the model just plays `walk` while it moves through the air, so author the **wing-flap loop as the `walk` clip** (and a hover/glide as `idle`). The other clips (`attack`, `spell`, `death`) work the same as for ground units.

### Item animations

Items are widgets too, so they share the same name-matched clip system — but only three states are meaningful for a ground item (the other clips are ignored). Carried items are hidden (no animation while in a slot), so all three only ever play on the ground:

| Clip Name | When Active | Looping |
|-----------|-------------|---------|
| `idle` | The item is lying on the ground | Yes |
| `birth` | Plays once when the item is dropped / created on the ground (materialize / drop-in) | No |
| `death` | Plays once when the item is **destroyed on the ground** — a consumed `powerup`, a `charged` item spending its last charge on the ground, or Lua `RemoveItem`. Then the item is removed. | No (holds last frame) |

- **`idle` is the WC3 "Stand" loop** — spinning powerups/runes, bobbing potions, a glowing tome. Author it as a looping transform baked into the model. This is the whole of item liveliness; most items need only `idle`.
- **Pickup into an inventory is instant** — no `death` clip plays when a unit *collects* an item (matches WC3). `death` is for an item leaving the world *on the ground*, not for one going into a slot.
- **Preplaced items** (authored in the map) come up already in `idle` — they do **not** replay `birth` on map load. Only items created/dropped at runtime birth.
- All three are **optional**: with no clip for a state the bind pose is used (a static potion model just sits there — no crash, no motion).
- Items are not skeletal fighters — `walk` / `attack` / `spell` / `hit` clips on an item model are ignored.


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

## Textures

- **Recommended format**: KTX2 + Basis Universal — one format, every target, every mount. The [Basis Universal](https://github.com/BinomialLLC/basis_universal) transcoder library is linked into `uldum_asset`; it parses the KTX2 container and transcodes to RGBA8 for GPU upload (BC7/ASTC paths are a later optimization).
- **Unit textures**: referenced by glTF material. KTX2 can be embedded in the `.glb` via the `KHR_texture_basisu` extension, embedded as a raw image buffer-view, or sit as an external file next to the model. The loader detects KTX2 by mime type, magic bytes, or `.ktx2` extension and routes through the transcoder; everything else goes through stb_image (PNG / JPEG / BMP / TGA / HDR).
- **Terrain textures**: 4-layer splatmap; each layer points to a KTX2 in `tileset.json`.

### Texture Pipeline

| Stage | Format | Tool |
|-------|--------|------|
| Authoring | PNG / TGA / source format of choice | Any image editor |
| Import step (author-side, outside the engine) | PNG → KTX2 | `basisu` / `scripts/png_to_ktx2.ps1` |
| In-tree / shipping | KTX2 + Basis Universal | — (runtime reads KTX2 directly) |

See [packaging.md](packaging.md) and [editor.md](editor.md) for the full authoring workflow.

## Audio

- **Music / voice**: OGG Opus (streaming)
- **SFX**: WAV (no decompression latency) or OGG Opus
- **3D positional**: Per-unit sound sources with distance attenuation

## Unit Type Model Reference

In `units.json`, the `model` field points to the glTF file:

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

### Node transforms (sizing / placing a model)

The loader respects glTF **node transforms** (translation / rotation / scale, or a node `matrix`), composed up the hierarchy — standard glTF behavior. For static meshes the node's world transform is **baked into the vertices at load**; for skinned meshes the skeleton owns placement (vertices stay in bind-pose space).

Practical upshot for authoring (e.g. in Blender):

- **You do NOT need to Apply Transforms.** Object-level scale/rotation/position you set in Blender is exported as node TRS and the engine honors it. (Applying transforms also works — it just bakes the same result into the mesh instead.)
- To resize a too-small/too-large model, set the object scale in Blender and re-export; no code or per-type field needed. (Per-instance `transform.scale` in the unit/projectile type def still stacks on top if you want runtime variation.)
- The bake uses the inverse-transpose for normals, so **non-uniform** node scale won't skew lighting.
