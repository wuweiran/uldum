# Uldum Engine — Terrain System Design

## Overview

Tile-based heightmap terrain inspired by Warcraft III. The terrain is the foundational surface for all gameplay — units walk on it, buildings sit on it, the camera looks at it. The engine provides heightmap sculpting, cliff layers, texture blending, water, and per-vertex pathing.

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
    ├── tile_layer: u8[]    — terrain type (0-3, index into texture layers)
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

Where adjacent vertices differ in cliff level, the renderer generates vertical wall geometry connecting the two plateaus. The wall uses a cliff-face texture (rock, stone) rather than ground splatmap textures.

### Ramps

Ramps are vertices marked with `PATHING_RAMP`. A ramp allows ground units to walk between adjacent vertices that differ by exactly 1 cliff level. The heightmap smoothly interpolates between the two layer heights across the ramp.

Ramp constraints:
- Only connects cliff levels that differ by 1
- Must form a contiguous strip of ramp-flagged vertices
- Heightmap values on ramp vertices interpolate between the two layer base heights

## Tile Layers (Ground Textures)

Each vertex stores a single terrain type index (0-3) selecting one of 4 ground
texture layers. No blending — each vertex is 100% one type, like WC3.

### Texture Layers

A map's tileset defines which textures occupy each layer slot (0-3). Typical setup:

| Layer | Example    |
|-------|------------|
| 0     | Grass      |
| 1     | Dirt       |
| 2     | Stone      |
| 3     | Water      |

The tileset is defined in the map's `tileset.json`, not by the engine.

### Painting

The editor's paint brush sets the tile layer at vertices within the brush radius.
Click and drag to paint. Each vertex becomes 100% the selected layer.

## Water

Water is tile layer 3 (`WATER_LAYER`). The engine checks `tile_layer == 3` to
determine water behavior.

### Water as a Tile Type

- **Painting**: Use the editor's Paint tool with layer 3 (Water), same as other ground types.
- **Pathing**: Ground units cannot traverse water vertices. Amphibious units can. Air units ignore water.
- **Rendering**: Currently renders as an opaque blue tile. Translucent water rendering with waves is a future improvement.

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
| Water (tile_layer == 3) | Blocked (amphibious can walk) | Can fly |
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
| `map`        | Owns `TerrainData`. Loads/saves from map file                |
| `render`     | Reads `TerrainData`, builds GPU mesh, draws terrain           |
| `simulation` | Reads heightmap for unit Z, pathing + tile_layer for movement  |
| `editor`     | Writes to `TerrainData` (sculpt, paint, cliff, ramp, pathing) |

## Rendering

### Terrain Mesh

The renderer builds a GPU mesh from the terrain data:
- One vertex per grid intersection with position, normal, texcoord, and texture layer weights
- Normal computed from cross product of adjacent edge vectors
- Two triangles per tile
- Tile layer converted to 4-channel weights at mesh build time; shader blends 4 texture samples
- Terrain mesh re-uploaded when modified (editor sculpt/paint)

### Cliff Walls

At cliff edges, the renderer generates vertical quad strips:
- Connects the top edge (higher cliff level) to the bottom edge (lower level)
- Uses a cliff texture (rock/stone), not splatmap blending
- Normals face outward from the cliff face

### Water Surface

Rendered as a separate translucent pass:
- One quad per water tile, positioned at `water_height`
- Alpha varies by depth (shallow = more transparent)
- Simple vertex-based wave animation
- Depth test on, depth write off (like particles)

### Shadow

Terrain receives shadows from the shadow map. Cliff walls and water also receive shadows.

### Future Rendering Improvements

- LOD: reduce triangle count for distant terrain chunks
- Chunked updates: only re-upload modified sections on edit
- Water reflections (planar or screen-space)
- Terrain holes (per-vertex flag to cut geometry for cave entrances)

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
