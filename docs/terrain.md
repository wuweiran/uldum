# Uldum Engine — Terrain System Design

## Overview

Tile-based heightmap terrain inspired by Warcraft III. The terrain is the foundational surface for all gameplay — units walk on it, buildings sit on it, the camera looks at it.

## Data Model

Terrain is a regular grid of tiles. Heights are stored per-vertex (grid intersections), other properties are stored per-tile.

```
TerrainData
├── tiles_x, tiles_y    — grid dimensions in tiles
├── tile_size           — world-space size of each tile (default 2.0)
├── heightmap: float[]  — height (Z) per vertex, size = (tiles_x+1) * (tiles_y+1)
├── tile_type: u8[]     — ground texture index per tile, size = tiles_x * tiles_y
├── pathing: u8[]       — flag bits per tile, size = tiles_x * tiles_y
├── cliff_level: u8[]   — discrete cliff layer per vertex (future)
└── water_level: float[]— water height per tile (future)
```

### Coordinate System

- Game coordinates: X = right, Y = forward, Z = up
- Terrain origin at (0, 0)
- Terrain spans `[0, tiles_x * tile_size]` in X, `[0, tiles_y * tile_size]` in Y
- Height (Z) stored per vertex in the heightmap array

### Vertex Indexing

Vertex at grid position `(ix, iy)` is at array index `iy * (tiles_x + 1) + ix`.

World position of vertex `(ix, iy)`:
```
x = ix * tile_size
y = iy * tile_size
z = heightmap[iy * (tiles_x + 1) + ix]
```

### Tile Indexing

Tile at grid position `(ix, iy)` is at array index `iy * tiles_x + ix`. A tile is the quad bounded by vertices `(ix, iy)`, `(ix+1, iy)`, `(ix, iy+1)`, `(ix+1, iy+1)`.

## Tile Types

Each tile has a `tile_type` index referencing a ground texture (grass, dirt, stone, etc.). Adjacent tiles with different types blend at the boundary via splatmap in the shader.

Default tile types (engine-defined, map-overridable):

| Index | Name       |
|-------|------------|
| 0     | Grass      |
| 1     | Dirt       |
| 2     | Stone      |
| 3     | Sand       |

## Pathing Flags

Per-tile bitfield controlling movement and placement:

```
bit 0: walkable    (ground units can traverse)
bit 1: flyable     (air units can traverse)
bit 2: buildable   (buildings can be placed)
bit 3: blight      (undead blight overlay — future)
```

Default: all tiles are walkable + flyable + buildable (0x07).

## Cliff System (Future)

Discrete height layers per vertex. A cliff edge is where adjacent vertices differ by one or more cliff levels. Cliff geometry replaces the smooth heightmap at those edges with vertical wall meshes.

## Water (Future)

Per-tile water level. Where `water_level > heightmap`, water surface is rendered as a translucent plane at the water height. Shallow water vs deep water determined by depth difference.

## Module Ownership

| Module     | Responsibility                                              |
|------------|-------------------------------------------------------------|
| `map`      | Owns `TerrainData`. Loads from map file, provides to others |
| `render`   | Reads `TerrainData`, builds GPU mesh, draws terrain         |
| `simulation` | Reads heightmap for unit Z placement, pathing for movement |
| `editor`   | Writes to `TerrainData` (sculpt, paint, mark pathing)       |

## Rendering

The renderer builds a GPU mesh from the heightmap:
- One vertex per grid intersection, with position, normal, and texcoord
- Normals computed from cross product of adjacent edge vectors
- Two triangles per tile (triangle list)
- Terrain mesh is re-uploaded when heightmap is modified (editor)

### Current Implementation (Phase 5c)

- Procedural heightmap with value noise (placeholder until map loading)
- Normal-based coloring (no textures yet)
- Single draw call for entire terrain

### Future Rendering Improvements

- Splatmap texturing: blend up to 4 ground textures per tile (Phase 5d)
- LOD: reduce triangle count for distant terrain chunks
- Chunked updates: only re-upload modified sections on edit
- Cliff wall meshes at cliff edges
- Water surface rendering with transparency and reflection
