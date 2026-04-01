#pragma once

#include "core/types.h"

#include <vector>

namespace uldum::map {

// Per-tile pathing flags.
enum PathingFlag : u8 {
    PATHING_WALKABLE  = 1 << 0,
    PATHING_FLYABLE   = 1 << 1,
    PATHING_BUILDABLE = 1 << 2,
};

static constexpr u8 PATHING_DEFAULT = PATHING_WALKABLE | PATHING_FLYABLE | PATHING_BUILDABLE;

// Terrain data: heightmap + tile metadata.
// Owned by MapManager, read by renderer and simulation.
//
// Heights are per-vertex: (tiles_x+1) * (tiles_y+1) entries.
// Tile properties are per-tile: tiles_x * tiles_y entries.
struct TerrainData {
    u32 tiles_x  = 0;
    u32 tiles_y  = 0;
    f32 tile_size = 2.0f;   // world units per tile edge

    // Per-vertex data — indexed by iy * (tiles_x+1) + ix
    std::vector<f32> heightmap;

    // Per-tile data — indexed by iy * tiles_x + ix
    std::vector<u8>  tile_type;    // ground texture index
    std::vector<u8>  pathing;      // PathingFlag bitfield

    // Future:
    // std::vector<u8>  cliff_level;  // per-vertex discrete cliff layer
    // std::vector<f32> water_level;  // per-tile water height

    // ── Helpers ────────────────────────────────────────────────────────

    u32 verts_x() const { return tiles_x + 1; }
    u32 verts_y() const { return tiles_y + 1; }
    u32 vertex_count() const { return verts_x() * verts_y(); }
    u32 tile_count()   const { return tiles_x * tiles_y; }

    f32 world_width()  const { return static_cast<f32>(tiles_x) * tile_size; }
    f32 world_height() const { return static_cast<f32>(tiles_y) * tile_size; }

    f32& height_at(u32 ix, u32 iy) { return heightmap[iy * verts_x() + ix]; }
    f32  height_at(u32 ix, u32 iy) const { return heightmap[iy * verts_x() + ix]; }

    u8& tile_type_at(u32 ix, u32 iy) { return tile_type[iy * tiles_x + ix]; }
    u8  tile_type_at(u32 ix, u32 iy) const { return tile_type[iy * tiles_x + ix]; }

    u8& pathing_at(u32 ix, u32 iy) { return pathing[iy * tiles_x + ix]; }
    u8  pathing_at(u32 ix, u32 iy) const { return pathing[iy * tiles_x + ix]; }

    bool is_valid() const { return tiles_x > 0 && tiles_y > 0 && !heightmap.empty(); }
};

// Create a flat terrain with all default values.
TerrainData create_flat_terrain(u32 tiles_x, u32 tiles_y, f32 tile_size = 2.0f, f32 base_height = 0.0f);

// Create a procedural terrain with value-noise hills (placeholder for testing).
TerrainData create_procedural_terrain(u32 tiles_x, u32 tiles_y, f32 tile_size = 2.0f);

} // namespace uldum::map
