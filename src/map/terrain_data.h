#pragma once

#include "core/types.h"

#include <glm/vec3.hpp>

#include <string_view>
#include <vector>

namespace uldum::map {

// Per-vertex pathing flags.
enum PathingFlag : u8 {
    PATHING_WALKABLE = 1 << 0,   // ground units can traverse
    PATHING_FLYABLE  = 1 << 1,   // air units can traverse
    PATHING_RAMP     = 1 << 2,   // allows walk between adjacent cliff levels
};

static constexpr u8 PATHING_DEFAULT = PATHING_WALKABLE | PATHING_FLYABLE;

// Splatmap layer indices. Water is a tile type, not a separate data layer.
// The engine checks if the dominant splatmap layer at a vertex is WATER_LAYER
// to determine water pathfinding and rendering behavior.
static constexpr u32 WATER_LAYER = 3;


// Terrain data: heightmap + cliff levels + splatmap + pathing.
// Owned by MapManager, read by renderer and simulation.
//
// Per-vertex arrays: (tiles_x+1) * (tiles_y+1) entries.
//
// Final vertex height = cliff_level * layer_height + heightmap
struct TerrainData {
    u32 tiles_x   = 0;
    u32 tiles_y   = 0;
    f32 tile_size  = 128.0f;   // world units per tile edge (WC3 scale)
    f32 layer_height = 128.0f; // height per cliff level

    // Per-vertex data — indexed by iy * (tiles_x+1) + ix
    std::vector<f32> heightmap;      // smooth offset within cliff layer
    std::vector<u8>  cliff_level;    // discrete elevation layer (0-15)
    std::vector<u8>  tile_layer;     // terrain type per vertex (0-3, index into texture layers)
    std::vector<u8>  pathing;        // PathingFlag bitfield

    // ── Helpers ────────────────────────────────────────────────────────

    u32 verts_x() const { return tiles_x + 1; }
    u32 verts_y() const { return tiles_y + 1; }
    u32 vertex_count() const { return verts_x() * verts_y(); }
    u32 tile_count()   const { return tiles_x * tiles_y; }

    f32 world_width()  const { return static_cast<f32>(tiles_x) * tile_size; }
    f32 world_height() const { return static_cast<f32>(tiles_y) * tile_size; }

    // Per-vertex accessors
    f32& height_at(u32 ix, u32 iy) { return heightmap[iy * verts_x() + ix]; }
    f32  height_at(u32 ix, u32 iy) const { return heightmap[iy * verts_x() + ix]; }

    u8& cliff_at(u32 ix, u32 iy) { return cliff_level[iy * verts_x() + ix]; }
    u8  cliff_at(u32 ix, u32 iy) const { return cliff_level[iy * verts_x() + ix]; }

    u8& pathing_at(u32 ix, u32 iy) { return pathing[iy * verts_x() + ix]; }
    u8  pathing_at(u32 ix, u32 iy) const { return pathing[iy * verts_x() + ix]; }

    // Final world Z at a vertex
    f32 world_z_at(u32 ix, u32 iy) const {
        u32 idx = iy * verts_x() + ix;
        return static_cast<f32>(cliff_level[idx]) * layer_height + heightmap[idx];
    }

    // Check if a vertex is water
    bool is_water(u32 ix, u32 iy) const {
        return tile_layer[iy * verts_x() + ix] == WATER_LAYER;
    }

    bool is_valid() const { return tiles_x > 0 && tiles_y > 0 && !heightmap.empty(); }
};

// ── Terrain sampling (visual only — not for pathfinding) ─────────────────

// Bilinear interpolated height at world position.
f32 sample_height(const TerrainData& td, f32 x, f32 y);

// Terrain surface normal at world position (central differences).
glm::vec3 sample_normal(const TerrainData& td, f32 x, f32 y);

// Create a flat terrain with all default values.
TerrainData create_flat_terrain(u32 tiles_x, u32 tiles_y, f32 tile_size = 128.0f, f32 base_height = 0.0f);

// Create a procedural terrain with value-noise hills (placeholder for testing).
TerrainData create_procedural_terrain(u32 tiles_x, u32 tiles_y, f32 tile_size = 128.0f);

bool save_terrain(const TerrainData& td, std::string_view path);
TerrainData load_terrain(std::string_view path);

} // namespace uldum::map
