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
| Meshes | `meshes[]` | Yes | Triangle primitives only |
| Skeleton | `skins[0]` | No | First skin used. Up to 128 bones |
| Animations | `animations[]` | No | Matched to engine states by name |
| Materials | `materials[]` | No | Only `baseColorFactor` is read, as a 1×1 RGBA fallback when the model has no image data |

## Supported subset of glTF 2.0

The loader is tuned for skeletal-animated unit models. Authors hitting an unsupported feature get a `[WARN ] [Asset]` log line at load time so the limit doesn't go unnoticed.

### Supported

- Geometry: `TRIANGLES` primitives only. Indexed and non-indexed both work.
- Vertex attributes: `POSITION` (required), `NORMAL`, `TEXCOORD_0`, `JOINTS_0`, `WEIGHTS_0`.
- Skin: first `skins[0]` only. Up to 128 bones. 4 bone influences per vertex.
- Bone-parented un-skinned meshes (e.g., a weapon node parented to a hand bone) are auto-converted to skinned with 100% weight on the parent bone.
- Animations: `translation`, `rotation`, `scale` channels. Played as linear regardless of the sampler's authored interpolation.
- Textures: PNG / JPEG / BMP / TGA / HDR (via stb_image), KTX2 / Basis Universal (via the engine's transcoder). Embedded in buffer-views or referenced by external URI. KTX2 detection works via the `KHR_texture_basisu` extension's mime type, the KTX2 magic prefix, or a `.ktx2` URI extension.
- Materials: only `pbrMetallicRoughness.baseColorFactor` is read, used as a 1×1 RGBA fallback texture when the model ships no image data.

### Not supported

These are silently ignored unless a warning is noted:

- Primitive types other than `TRIANGLES` (warning emitted, primitive skipped).
- Additional UV sets (`TEXCOORD_1+`), additional joint/weight sets — warnings emitted; only the first set is used.
- Vertex colors (`COLOR_0`), `TANGENT` attribute.
- Morph targets and morph-target animation channels.
- Animation sampler interpolation modes `STEP` and `CUBICSPLINE` (warning emitted; played as linear).
- Animations on un-skinned models (warning emitted; channels dropped).
- Skins beyond `skins[0]` (warning emitted).
- Material properties beyond `baseColorFactor`: `baseColorTexture` mapping, roughness/metallic, normal map, occlusion, emissive, alpha mode (`MASK` / `BLEND`), all `KHR_materials_*` extensions, and `KHR_materials_pbrSpecularGlossiness` (legacy).
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

## Animation Clips

Clips are glTF `animations[]` entries. The engine matches clips to gameplay states by the animation's `name` field (case-sensitive):

| Clip Name | Engine State | When Active | Looping |
|-----------|-------------|-------------|---------|
| `idle` | Standing still, no active orders | Default state | Yes |
| `walk` | Moving — Move order, AttackMove, or chasing an attack target | Movement or combat chase | Yes |
| `attack` | Attacking — plays during WindUp + Backswing phases, scaled to `attack_cooldown` | Combat system | No (restarts each swing) |
| `spell` | Casting an ability — plays during CastPoint + Backswing phases | Ability cast system | No |
| `death` | Unit has died | Death system | No (holds last frame) |

### Behavior

- **Missing clips**: If a model doesn't have a clip for a state, the bind pose is used (no crash)
- **State transitions**: 0.15s crossfade blend between states
- **Playback**: Animations run at render framerate (not simulation tick rate). The engine reads simulation state each frame to determine which clip to play
- **Attack timing**: The `attack` clip is uniformly scaled to fit `attack_cooldown`. The `animation.dmg_pt` fraction (from `units.json`) defines where the visual hit lands in the clip. The engine uses `combat.dmg_time` and `combat.backsw_time` (seconds) for gameplay timing
- **Spell timing**: The `spell` clip uses two-phase scaling. The `animation.cast_pt` fraction defines where the visual cast fires. The ability's `cast_time` and `backsw_time` (seconds) control gameplay timing

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
