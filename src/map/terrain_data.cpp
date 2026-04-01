#include "map/terrain_data.h"
#include "core/log.h"

#include <cmath>

namespace uldum::map {

static constexpr const char* TAG = "Terrain";

TerrainData create_flat_terrain(u32 tiles_x, u32 tiles_y, f32 tile_size, f32 base_height) {
    TerrainData td;
    td.tiles_x   = tiles_x;
    td.tiles_y   = tiles_y;
    td.tile_size = tile_size;

    td.heightmap.assign(td.vertex_count(), base_height);
    td.tile_type.assign(td.tile_count(), 0);  // grass
    td.pathing.assign(td.tile_count(), PATHING_DEFAULT);

    log::info(TAG, "Created flat terrain: {}x{} tiles, tile_size={}", tiles_x, tiles_y, tile_size);
    return td;
}

// ── Value noise for procedural generation ──────────────────────────────────

static f32 hash2d(i32 x, i32 y) {
    i32 n = x + y * 57;
    n = (n << 13) ^ n;
    return static_cast<f32>((n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) / 2147483647.0f;
}

static f32 smooth_noise(f32 x, f32 y) {
    i32 ix = static_cast<i32>(std::floor(x));
    i32 iy = static_cast<i32>(std::floor(y));
    f32 fx = x - static_cast<f32>(ix);
    f32 fy = y - static_cast<f32>(iy);

    // Smoothstep
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);

    f32 a = hash2d(ix, iy);
    f32 b = hash2d(ix + 1, iy);
    f32 c = hash2d(ix, iy + 1);
    f32 d = hash2d(ix + 1, iy + 1);

    return a + fx * (b - a) + fy * (c - a) + fx * fy * (a - b - c + d);
}

TerrainData create_procedural_terrain(u32 tiles_x, u32 tiles_y, f32 tile_size) {
    TerrainData td;
    td.tiles_x   = tiles_x;
    td.tiles_y   = tiles_y;
    td.tile_size = tile_size;

    td.heightmap.resize(td.vertex_count());
    td.tile_type.assign(td.tile_count(), 0);
    td.pathing.assign(td.tile_count(), PATHING_DEFAULT);

    // Generate heights: two octaves of value noise
    for (u32 iy = 0; iy < td.verts_y(); ++iy) {
        for (u32 ix = 0; ix < td.verts_x(); ++ix) {
            f32 wx = static_cast<f32>(ix) * tile_size;
            f32 wy = static_cast<f32>(iy) * tile_size;

            f32 h = 0.0f;
            h += smooth_noise(wx * 0.05f, wy * 0.05f) * 4.0f;  // large hills
            h += smooth_noise(wx * 0.15f, wy * 0.15f) * 1.0f;  // small bumps
            td.height_at(ix, iy) = h;
        }
    }

    // Assign tile types based on height (simple threshold)
    for (u32 iy = 0; iy < tiles_y; ++iy) {
        for (u32 ix = 0; ix < tiles_x; ++ix) {
            // Average height of the tile's four corners
            f32 avg = (td.height_at(ix, iy) + td.height_at(ix + 1, iy) +
                       td.height_at(ix, iy + 1) + td.height_at(ix + 1, iy + 1)) * 0.25f;

            if (avg > 3.5f)      td.tile_type_at(ix, iy) = 2;  // stone (high)
            else if (avg > 2.0f) td.tile_type_at(ix, iy) = 1;  // dirt (mid)
            else                 td.tile_type_at(ix, iy) = 0;  // grass (low)
        }
    }

    log::info(TAG, "Created procedural terrain: {}x{} tiles, tile_size={}, {} vertices",
              tiles_x, tiles_y, tile_size, td.vertex_count());
    return td;
}

} // namespace uldum::map
