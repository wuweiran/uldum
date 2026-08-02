# Overlay Textures

Ground-plane decals drawn by `WorldOverlays` — selection rings, cast range
rings, AoE previews (circle / cone / line), the cast-arrow curve, the reticle,
and the mobile snap-target column. They sit on the terrain under units and
targeting UI. See [ui.md](ui.md) for the surrounding World UI system.

## Generating the set

The decal set is generated procedurally by the editor's **New Map** command
(File → New Map): a new map is bootstrapped with the whole set written into its
`textures/overlays/`, encoded in-process (no external tool), with the style
chosen from the map's input preset (RTS → Original, Action → Action). The pixel
math in `src/editor/overlay_decals.cpp` is the source of truth — tweak it to
change the shipped look.

To re-generate or restyle the decals for an **existing** map, author each decal
by hand (any image editor) and encode it via the editor's **Import PNG...**
(linear) into `textures/overlays/` — the mask convention below is all that
matters.


## How overlays get their texture

Each overlay slot is one of `WorldOverlays::TextureId`. Two are **engine-owned
defaults**, generated procedurally at init (1×1 solid white, tinted per-draw) so
overlays work with zero map art:

- `Solid` — untextured fills / rings / ribbons. Not overridable.
- `Placement` — build/editor footprint (green = valid, red = blocked). Overridable.

Every other slot is **unbound until a map supplies a KTX2** via `hud.json`. An
`add_*` call on an unbound slot is silently dropped, so a map that ships no
overlay art simply gets no selection ring / AoE preview / etc. The shipped maps
(`test_map`, `action_test`) each carry a full set under
`<map>.uldmap/textures/overlays/*.ktx2`.

`hud.json` points each slot at a file (paths are map-relative):

```jsonc
"targeting": {
  "selection_texture": "textures/overlays/ring_stroke.ktx2",
  "range_ring":   { "texture": "textures/overlays/ring_stroke.ktx2" },
  "cast_curve":   { "texture": "textures/overlays/curve_stroke.ktx2" },
  "snap_target":  { "texture": "textures/overlays/snap_target.ktx2" },
  "aoe": {
    "circle_texture": "textures/overlays/aoe_circle.ktx2",
    "cone_texture":   "textures/overlays/aoe_cone.ktx2",
    "line_texture":   "textures/overlays/aoe_line.ktx2"
  }
}
```

## Texture format

- **Container / codec:** KTX2 + UASTC, mipmapped, **linear** (these are alpha-mask
  data, not color — do not encode sRGB), Supercompression Scheme **NONE**
  (the runtime transcoder rejects Zstd). Encode as any data texture — use the
  editor's Import PNG with the **linear** option checked.
- **Alpha-mask convention:** author each pixel as `(a, a, a, a)` — RGB equal to
  alpha. The overlay shader computes `texture * vertex_color`, and the per-draw
  vertex color carries the tint (premultiplied). Storing RGB = A means the
  multiply yields premultiplied output **regardless** of the tint's RGB, so one
  grayscale mask restyles to any color from `hud.json`.
- The **shape lives entirely in the texture**; geometry is a thin substrate. This
  is why a new look is a texture swap, never a code change.

## The decal set — dimensions and UV mapping

The shipped decals (what the shipped maps use). Dimensions matter because each
primitive samples a specific UV layout:

| File | Slot(s) | Size (px) | Primitive | UV mapping |
|------|---------|-----------|-----------|------------|
| `ring_stroke.ktx2` | selection ring, range ring, target-unit ring | 64×4 | ribbon | U across stroke width (0=inner rim, 1=outer), V wraps around the ring |
| `aoe_line.ktx2` | AoE line | 256×256 | ribbon | U across width, V along the beam (V=1 = impact end) |
| `curve_stroke.ktx2` | cast arrow | 64×256 | ribbon | U across width, V along length (V=0 caster end → 1 target end) |
| `aoe_circle.ktx2` | AoE circle | 256×256 | quad | full `[0,1]²` across the quad, radial pattern centered |
| `reticle.ktx2` | ground-target reticle | 256×256 | quad | full `[0,1]²`, centered donut |
| `aoe_cone.ktx2` | AoE cone | 256×256 | fan | V=0 apex → V=1 rim, U=0→1 across the wedge angle |
| `snap_target.ktx2` | snap-target column | 32×256 | pillar | U across width (cylindrical cross-section), V=0 top → 1 base |

Sizes above are what the shipped maps happen to use, **not fixed requirements** —
every primitive maps its texture **once** across the drawn shape (UVs span `[0,1]`,
ClampToEdge, no tiling), so the mask always stretches to fit and the resolution
only needs to match how much detail the mask carries. Pick dimensions per axis:

- The **quads** and **cone** are 256² because they carry a full 2-D pattern.
- `ring_stroke` and `curve_stroke`'s **U** (across-width) axis only needs a stroke
  profile, so 64 px wide is plenty.
- `ring_stroke` ships 64×4 because its **V** (around-the-ring) axis is uniform —
  4 px is just the minimum valid strip (one UASTC block row). But V is a real
  axis: give it height and detail if you want variation along the length.

`aoe_line` is the clearest example — see the note below.

### Authoring guidance per primitive

- **Ring stroke** (`ring_stroke`): the meaningful axis is U — a stroke profile,
  peak in the middle, falling to 0 at both edges. V (around the ring) is uniform,
  so a few px of height is enough.
- **AoE line** (`aoe_line`): a **full ribbon with width (U) and length (V)** — not
  just a 1-D stroke. The generator emits a 256² beam with directional chevrons
  flowing toward the impact end (V=1). Note the ribbon maps the texture **once**
  across the beam (V=0→1 over the whole length, ClampToEdge — no tiling), so the
  chevron *count* is fixed per beam and stretches/squashes with beam length rather
  than holding constant spacing. Same primitive as the cast arrow — treat it as a
  2-D canvas (dashes, arrowheads, a gradient toward impact) when authoring by hand.
- **Curve stroke:** U is the stroke profile; V modulates alpha along the length —
  e.g. fade the caster end to ~0 so the arrow points.
- **Quads** (`aoe_circle`, `reticle`): a centered radial design. Keep alpha 0
  outside the intended radius so the decal doesn't fill its bounding box.
- **Cone:** paint in unwrapped fan space — V is apex→rim, U is the angular sweep.
  Put the rim band near V=1 and soften the U edges so the wedge boundary reads.
- **Pillar** (`snap_target`): a vertical band; give the horizontal cross-section a
  `sqrt(1-u²)`-style falloff for perceived cylindrical volume.

## Per-map customization

Overlays are per-map art. A map restyles by dropping its own KTX2s into
`<map>.uldmap/textures/overlays/` and pointing `hud.json` at them — no engine
change. `action_test` ships a deliberately distinct "action" set (crisp
techy rings, MOBA-style targeting reticle, twin-band snap column) to demonstrate
that two maps in one build can look entirely different while sharing the engine's
overlay code.
