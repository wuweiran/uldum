# Uldum Engine — Terrain System Design

## Overview

Tile-based heightmap terrain inspired by Warcraft III. The terrain is the foundational surface for all gameplay — units walk on it, buildings sit on it, the camera looks at it. The engine provides heightmap sculpting, cliff layers, texture painting, water, and per-vertex pathing.

## Data Model

Terrain is a regular grid of tiles at WC3 scale (1 tile = 128 game units). All data is per-vertex.

```
TerrainData
├── tiles_x, tiles_y        — grid dimensions in tiles
├── tile_size               — world-space size per tile (128.0 game units)
├── layer_height            — height per cliff level (128.0 game units)
│
└── Per-vertex data — (tiles_x+1) * (tiles_y+1) entries
    ├── heightmap: f32[]    — smooth height offset within cliff layer
    ├── cliff_level: u8[]   — discrete elevation layer (0-15)
    ├── tile_layer: u8[]    — terrain type index (0-255, into tileset layers)
    └── pathing: u8[]       — flag bits (walkable, flyable, ramp)
```

### Final Vertex Height

A vertex's world Z is determined by both its cliff layer and heightmap offset:

```
z = cliff_level * layer_height + heightmap
```

- `cliff_level` places the vertex on a discrete plateau (0, 128, 256, ...)
- `heightmap` adds smooth variation within that plateau (hills, slopes)

### Coordinate System

- Game coordinates: X = right, Y = forward, Z = up
- Terrain origin at (0, 0)
- Terrain spans `[0, tiles_x * tile_size]` in X, `[0, tiles_y * tile_size]` in Y

### Vertex Indexing

Vertex at grid position `(ix, iy)` is at array index `iy * (tiles_x + 1) + ix`.

World position of vertex `(ix, iy)`:
```
x = ix * tile_size
y = iy * tile_size
z = cliff_level[index] * layer_height + heightmap[index]
```

### Tile Indexing

Tile at grid position `(ix, iy)` is at array index `iy * tiles_x + ix`. A tile is the quad bounded by vertices `(ix, iy)`, `(ix+1, iy)`, `(ix, iy+1)`, `(ix+1, iy+1)`.

## Cliff System

Each vertex has a discrete cliff level (0-15). Cliff levels create sharp impassable elevation changes between adjacent vertices with different levels.

### Cliff Rules

- **Same level**: Smooth heightmap terrain, normal pathability
- **Different level**: Impassable cliff wall between vertices (auto-generated geometry)
- **Ramp**: Special per-vertex flag that allows ground movement between adjacent cliff levels

### Cliff Wall Rendering

Where adjacent vertices differ in cliff level, the renderer generates vertical wall geometry connecting the two plateaus. The wall uses a cliff-face texture (rock, stone) rather than ground textures.

### Ramps

Ramps are vertices marked with `PATHING_RAMP`. A ramp allows ground units to walk between adjacent vertices that differ by exactly 1 cliff level. The heightmap smoothly interpolates between the two layer heights across the ramp.

Ramp constraints:
- Only connects cliff levels that differ by 1
- Must form a contiguous strip of ramp-flagged vertices
- Heightmap values on ramp vertices interpolate between the two layer base heights

## Tileset & Terrain Textures

Each map defines a **tileset** in `tileset.json`. The tileset lists all terrain layer types available in that map. Each layer has a diffuse texture and a blend mode for transitions.

### Tileset Format

```json
{
    "name": "Ashenvale",
    "layers": [
        { "id": 0, "name": "grass",  "diffuse": "textures/grass.ktx2" },
        { "id": 1, "name": "dirt",   "diffuse": "textures/dirt.ktx2" },
        { "id": 2, "name": "stone",  "diffuse": "textures/stone.ktx2" },
        { "id": 3, "name": "mud",    "diffuse": "textures/mud.ktx2" },
        { "id": 4, "name": "shallow_water", "type": "water_shallow",
          "color": [0.15, 0.35, 0.45], "opacity": 0.5, "wave_speed": 0.3 },
        { "id": 5, "name": "deep_water", "type": "water_deep",
          "color": [0.05, 0.12, 0.25], "opacity": 0.9, "wave_speed": 0.6 }
    ]
}
```

- **`diffuse`**: Path to diffuse color texture (KTX2). Relative to map root.
- **`type`**: Optional. Marks special layer types the engine handles differently.

### Layer Count

Maps can define up to **16 layers** (stored as `u8` per vertex, shader uses `sampler2DArray`). Most maps use 4-8. The engine imposes no minimum.

### Special Layer Types

| Type | Engine behavior |
|------|----------------|
| (none) | Standard ground. Opaque, receives shadows, units walk on it. |
| `water_shallow` | Transparent water surface rendered on top of ground below. |
| `water_deep` | Opaque water surface. Ground not visible. |
| `grass` | Standard ground + geometry grass rendered on top (future). |

### Diffuse Textures

One diffuse texture per layer, provided by the map as KTX2 (authored from PNG via `scripts/png_to_ktx2.ps1`). Tiles once per terrain tile (128 game units). The engine generates procedural fallback textures if files are missing.

Per-layer normal maps are not supported yet. Terrain uses vertex normals for lighting. An optional `"normal"` field per layer can be added later for surface detail.

### Transition Noise

A single procedural noise texture (generated at engine startup) adds organic variation to terrain transition curves. Without it, transitions would be perfectly smooth mathematical shapes. The noise perturbs the curve position per-pixel, creating natural-looking irregular edges.

### Transition Curves

At tile boundaries, the engine computes curved transitions using signed-distance-field (SDF) math, similar to marching squares. Each tile's 4 corners are classified as dominant or secondary type, creating cases:

| Case | Pattern | Curve shape |
|------|---------|-------------|
| 4-0 | AAAA | No transition |
| 3-1 | AAAB | Quarter-circle arc in the minority corner |
| 2-2 edge | AABB | Straight line across tile middle |
| 2-2 diagonal | ABAB | Two quarter-circle arcs at secondary corners |
| 1-3 | ABBB | Inverse quarter-circle (dominant corner "island") |

Curves pass through the **midpoints of edges where types change**, ensuring adjacent tiles' curves connect seamlessly. The per-layer blend mask adds organic variation to the curve position.

### Painting

The editor's paint brush sets `tile_layer` at vertices within the brush radius. Each vertex is exactly one terrain type — no blending weights.

## Water

Water is a terrain layer type, not a separate data structure. The editor paints water the same way as any terrain type. The engine renders water layers differently.

### Shallow Water

- **Rendering**: Two-pass. Ground terrain is rendered normally first (the riverbed/lakebed is visible). Then a transparent water surface is drawn slightly above the ground height.
- **Surface height**: Ground Z + small fixed offset (e.g., 8 game units). The water surface sits just above the terrain.
- **Visuals**: Animated UV scrolling, wave distortion, tinted color, configurable opacity. Map defines color/opacity/wave_speed in tileset.
- **Pathing**: Ground units cannot traverse. Amphibious units can. Air units can fly over.
- **Interactions**: Future — unit splashes, ripple effects at unit positions.

### Deep Water (Sea)

- **Rendering**: One-pass. Opaque water surface, ground below is not visible.
- **Surface height**: Same as shallow — ground Z + offset.
- **Visuals**: Same animated surface as shallow but with higher opacity (~0.9) and darker color.
- **Pathing**: Same as shallow water.

### Water Properties (from Tileset)

| Property | Description | Default |
|----------|-------------|---------|
| `color` | RGB tint of the water surface | [0.1, 0.3, 0.5] |
| `opacity` | Surface transparency (0=invisible, 1=opaque) | 0.6 |
| `wave_speed` | UV animation speed | 0.4 |

## Pathing

Per-vertex bitfield controlling movement. A tile is passable only if **all 4 of
its corner vertices** are walkable. Marking any single vertex blocks all tiles
touching it.

### Pathing Flags

```
bit 0: WALKABLE   — ground units can traverse
bit 1: FLYABLE    — air units can traverse
bit 2: RAMP       — allows ground movement between adjacent cliff levels
```

Default: all vertices are `WALKABLE | FLYABLE` (0x03).

### Pathing Rules (Engine-Enforced)

| Condition | Ground unit | Air unit |
|-----------|------------|----------|
| All 4 corners WALKABLE, not water | Can walk | Can fly |
| Shallow water | Can walk (ground visible below) | Can fly |
| Deep water | Blocked (amphibious can walk) | Can fly |
| Any corner not WALKABLE | Blocked | Can fly (if FLYABLE) |
| Adjacent cliff levels differ | Blocked | Can fly |
| Adjacent cliff levels differ + all 4 RAMP | Can walk (slope) | Can fly |

### Ramp Validity

A ramp tile requires **all 4 corners** to have the RAMP flag and exactly 1 cliff
level difference. Partial ramp flags (3 of 4) are invalid — the editor enforces
this by setting/clearing ramp per-tile, not per-vertex.

## Module Ownership

| Module       | Responsibility                                               |
|--------------|--------------------------------------------------------------|
| `map`        | Owns `TerrainData`. Loads/saves from map file. Parses tileset. |
| `render`     | Reads `TerrainData` + tileset, builds GPU mesh, draws terrain + water |
| `simulation` | Reads heightmap for unit Z, pathing + tile_layer for movement  |
| `editor`     | Writes to `TerrainData` (sculpt, paint, cliff, ramp, pathing) |

## Rendering

### Terrain Mesh (Ground Pass)

The renderer builds a GPU mesh from the terrain data:
- One vertex per grid intersection: position, normal, texcoord, terrain type index
- Normal computed from cross product of adjacent edge vectors
- Two triangles per tile
- Texcoord maps once per tile (1 tile = 1 texture repeat)
- Terrain mesh re-uploaded when modified (editor sculpt/paint)

### Terrain Shader

- Terrain type index per vertex selects into a `sampler2DArray` (all diffuse textures stacked)
- At tile boundaries (where adjacent vertices have different types), blend-mask blending:
  1. Sample both layers' diffuse textures and blend masks
  2. Compare blend mask values to decide which layer wins at each fragment
  3. Produces natural-looking transition edges (jagged grass border, scattered dirt patches)
- Vertex normals used for lighting (per-layer normal maps can be added later)

### Cliff Walls

At cliff edges, the renderer generates vertical quad strips:
- Connects the top edge (higher cliff level) to the bottom edge (lower level)
- Uses a cliff texture (rock/stone), not ground layer blending
- Normals face outward from the cliff face

### Water Pass

Rendered as a separate pass after the ground terrain:
- Water surface mesh: one quad per water tile, at ground Z + small offset
- Depth test on, depth write off (transparent surface)
- Animated UV scrolling for wave effect
- Color and opacity from tileset water properties
- Shallow water: alpha blend over the ground below (ground visible through surface)
- Deep water: near-opaque, ground not visible

### Shadow

Terrain receives shadows from the shadow map. Cliff walls and water also receive shadows.

### Future Rendering Improvements

- LOD: reduce triangle count for distant terrain chunks
- Chunked updates: only re-upload modified sections on edit
- Planar water reflections
- Terrain holes (per-vertex flag to cut geometry for cave entrances)
- Geometry grass: billboard quads on grass-type tiles, wind animation, unit push-away

## Serialization

Terrain data is stored in the `.uldmap` package as binary:

```
terrain.bin
├── tiles_x(u32), tiles_y(u32), tile_size(f32), layer_height(f32)
├── heightmap:    f32[vertex_count]
├── cliff_level:  u8[vertex_count]
├── tile_layer:   u8[vertex_count]
└── pathing:      u8[vertex_count]
```

Flat arrays, no compression. A 64x64 tile map:
- vertex_count = 65 * 65 = 4225
- Total: ~25 KB (header 16B + heightmap 16.9K + cliff 4.2K + tile_layer 4.2K + pathing 4.2K)
