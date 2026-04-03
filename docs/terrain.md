# Uldum Engine — Terrain System Design

## Overview

Tile-based heightmap terrain inspired by Warcraft III. The terrain is the foundational surface for all gameplay — units walk on it, buildings sit on it, the camera looks at it. The engine provides heightmap sculpting, cliff layers, texture blending, water, and per-vertex pathing.

## Data Model

Terrain is a regular grid of tiles at WC3 scale (1 tile = 128 game units). Heights, cliff levels, texture blend weights, and pathing are stored per-vertex. Water is stored per-tile.

```
TerrainData
├── tiles_x, tiles_y        — grid dimensions in tiles
├── tile_size               — world-space size per tile (128.0 game units)
├── layer_height            — height per cliff level (128.0 game units)
│
├── Per-vertex data — (tiles_x+1) * (tiles_y+1) entries
│   ├── heightmap: f32[]    — smooth height offset within cliff layer
│   ├── cliff_level: u8[]   — discrete elevation layer (0-15)
│   ├── splatmap: u8[4][]   — blend weights for 4 texture layers (0-255 each)
│   └── pathing: u8[]       — flag bits (walkable, flyable, ramp, water)
│
└── Per-tile data — tiles_x * tiles_y entries
    └── water_height: f32[] — water surface height (< 0 = no water)
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

## Splatmap (Texture Blending)

Each vertex stores 4 blend weights (u8, 0-255) controlling how 4 ground texture layers mix at that point. The shader normalizes the weights and blends the texture samples.

```
final_color = sum(texture[i].sample(uv) * weight[i]) / sum(weight[i])
```

### Texture Layers

A map's tileset defines which textures occupy each layer slot (0-3). Typical setup:

| Layer | Example    |
|-------|------------|
| 0     | Grass      |
| 1     | Dirt       |
| 2     | Stone      |
| 3     | Sand       |

The tileset is defined in the map's `tileset.json`, not by the engine. The engine only provides the 4-layer blending mechanism.

### Painting

The editor's paint brush modifies splatmap weights at nearby vertices:
- Increase the selected layer's weight
- Optionally decrease other layers to maintain a normalized sum
- Brush has radius and strength (falloff from center)

## Water

Per-tile water height. A tile has water when `water_height >= 0`. The water surface renders as a translucent plane at the specified height.

### Water Properties

- **Depth**: `water_height - terrain_z` at any point. Determines shallow vs deep rendering.
- **Pathing**: The engine auto-sets `PATHING_WATER` on vertices of tiles that have water. Ground units cannot traverse water vertices unless they have a "swim" or "amphibious" classification. Air units ignore water.
- **Rendering**: Translucent plane with simple animation (vertex wave, alpha by depth). No reflections in v1.

### Water and Cliff Interaction

Water respects cliff levels — a lake on cliff level 0 does not flood up to cliff level 1. Water height is absolute (world Z), so it naturally pools in low areas.

## Pathing

Per-vertex bitfield controlling movement. This is higher resolution than per-tile — each vertex acts as a pathing cell, giving `(tiles_x+1) * (tiles_y+1)` cells.

### Pathing Flags

```
bit 0: WALKABLE   — ground units can traverse
bit 1: FLYABLE    — air units can traverse
bit 2: RAMP       — allows ground movement between adjacent cliff levels
bit 3: WATER      — auto-set by engine from water_height, swim-only
```

Default: all vertices are `WALKABLE | FLYABLE` (0x03).

### Pathing Rules (Engine-Enforced)

| Condition | Ground unit | Air unit |
|-----------|------------|----------|
| WALKABLE, no water | Can walk | Can fly |
| WALKABLE + WATER | Only if amphibious | Can fly |
| Not WALKABLE | Blocked | Can fly (if FLYABLE) |
| Adjacent cliff levels differ | Blocked | Can fly |
| Adjacent cliff levels differ + RAMP | Can walk (slope) | Can fly |

### Auto-Pathing

The engine automatically updates pathing based on terrain state:
- Cliff edges: clear WALKABLE between vertices with different cliff levels (unless RAMP)
- Water tiles: set WATER flag on affected vertices
- The editor can manually override pathing (e.g., mark a flat area as unwalkable for gameplay)

## Module Ownership

| Module       | Responsibility                                               |
|--------------|--------------------------------------------------------------|
| `map`        | Owns `TerrainData`. Loads/saves from map file                |
| `render`     | Reads `TerrainData`, builds GPU mesh, draws terrain + water  |
| `simulation` | Reads heightmap for unit Z, pathing for movement/collision   |
| `editor`     | Writes to `TerrainData` (sculpt, paint, cliff, water, path)  |

## Rendering

### Terrain Mesh

The renderer builds a GPU mesh from the terrain data:
- One vertex per grid intersection with position, normal, texcoord, and splatmap weights
- Normal computed from cross product of adjacent edge vectors
- Two triangles per tile
- Splatmap weights passed as vertex attributes, shader blends 4 texture samples
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
├── Header: tiles_x, tiles_y, tile_size, layer_height
├── heightmap:    f32[vertex_count]
├── cliff_level:  u8[vertex_count]
├── splatmap[0]:  u8[vertex_count]
├── splatmap[1]:  u8[vertex_count]
├── splatmap[2]:  u8[vertex_count]
├── splatmap[3]:  u8[vertex_count]
├── pathing:      u8[vertex_count]
└── water_height: f32[tile_count]
```

Straightforward flat arrays, no compression in v1. A 64x64 tile map:
- vertex_count = 65 * 65 = 4225
- tile_count = 64 * 64 = 4096
- Total: ~37 KB (heightmap 16.9K + cliff 4.2K + splatmap 16.9K + pathing 4.2K + water 16K)
