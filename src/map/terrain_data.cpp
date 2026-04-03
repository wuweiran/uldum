#include "map/terrain_data.h"
#include "core/log.h"

#include <cmath>
#include <cstring>
#include <fstream>

namespace uldum::map {

static constexpr const char* TAG = "Terrain";

TerrainData create_flat_terrain(u32 tiles_x, u32 tiles_y, f32 tile_size, f32 base_height) {
    TerrainData td;
    td.tiles_x     = tiles_x;
    td.tiles_y     = tiles_y;
    td.tile_size   = tile_size;

    u32 vc = td.vertex_count();
    u32 tc = td.tile_count();

    td.heightmap.assign(vc, base_height);
    td.cliff_level.assign(vc, 0);
    for (auto& layer : td.splatmap) layer.assign(vc, 0);
    td.splatmap[0].assign(vc, 255);  // default: full weight on layer 0
    td.pathing.assign(vc, PATHING_DEFAULT);
    td.water_height.assign(tc, -1.0f);  // no water

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
    td.tiles_x     = tiles_x;
    td.tiles_y     = tiles_y;
    td.tile_size   = tile_size;

    u32 vc = td.vertex_count();
    u32 tc = td.tile_count();

    td.heightmap.resize(vc);
    td.cliff_level.assign(vc, 0);   // all on level 0
    for (auto& layer : td.splatmap) layer.assign(vc, 0);
    td.pathing.assign(vc, PATHING_DEFAULT);
    td.water_height.assign(tc, -1.0f);

    // Generate heights: two octaves of value noise
    for (u32 iy = 0; iy < td.verts_y(); ++iy) {
        for (u32 ix = 0; ix < td.verts_x(); ++ix) {
            f32 wx = static_cast<f32>(ix) * tile_size;
            f32 wy = static_cast<f32>(iy) * tile_size;

            f32 h = 0.0f;
            h += smooth_noise(wx * 0.001f, wy * 0.001f) * 64.0f;   // large hills (scaled for WC3 units)
            h += smooth_noise(wx * 0.003f, wy * 0.003f) * 16.0f;   // small bumps
            td.height_at(ix, iy) = h;
        }
    }

    // Assign splatmap based on height (simple threshold)
    for (u32 iy = 0; iy < td.verts_y(); ++iy) {
        for (u32 ix = 0; ix < td.verts_x(); ++ix) {
            f32 h = td.height_at(ix, iy);
            u32 idx = iy * td.verts_x() + ix;

            // Clear all layers
            for (auto& layer : td.splatmap) layer[idx] = 0;

            if (h > 48.0f)       td.splatmap[2][idx] = 255;  // stone (high)
            else if (h > 24.0f)  td.splatmap[1][idx] = 255;  // dirt (mid)
            else                 td.splatmap[0][idx] = 255;  // grass (low)
        }
    }

    log::info(TAG, "Created procedural terrain: {}x{} tiles, tile_size={}, {} vertices",
              tiles_x, tiles_y, tile_size, vc);
    return td;
}

// ── Binary serialization ─────────────────────────────────────────────────

// terrain.bin format:
//   Header: tiles_x(u32), tiles_y(u32), tile_size(f32), layer_height(f32)
//   heightmap:    f32[vertex_count]
//   cliff_level:  u8[vertex_count]
//   splatmap[0]:  u8[vertex_count]
//   splatmap[1]:  u8[vertex_count]
//   splatmap[2]:  u8[vertex_count]
//   splatmap[3]:  u8[vertex_count]
//   pathing:      u8[vertex_count]
//   water_height: f32[tile_count]

bool save_terrain(const TerrainData& td, std::string_view path) {
    std::ofstream file(std::string(path), std::ios::binary);
    if (!file) {
        log::error(TAG, "Failed to open terrain file for writing: {}", path);
        return false;
    }

    auto write = [&](const void* data, size_t size) { file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size)); };

    write(&td.tiles_x, sizeof(u32));
    write(&td.tiles_y, sizeof(u32));
    write(&td.tile_size, sizeof(f32));
    write(&td.layer_height, sizeof(f32));

    u32 vc = td.vertex_count();
    u32 tc = td.tile_count();

    write(td.heightmap.data(),   vc * sizeof(f32));
    write(td.cliff_level.data(), vc * sizeof(u8));
    for (u32 i = 0; i < 4; ++i)
        write(td.splatmap[i].data(), vc * sizeof(u8));
    write(td.pathing.data(),     vc * sizeof(u8));
    write(td.water_height.data(), tc * sizeof(f32));

    log::info(TAG, "Saved terrain: {}x{} tiles to {}", td.tiles_x, td.tiles_y, path);
    return file.good();
}

TerrainData load_terrain(std::string_view path) {
    std::ifstream file(std::string(path), std::ios::binary);
    if (!file) {
        log::warn(TAG, "No terrain file: {}", path);
        return {};
    }

    auto read = [&](void* data, size_t size) { file.read(static_cast<char*>(data), static_cast<std::streamsize>(size)); };

    TerrainData td;
    read(&td.tiles_x, sizeof(u32));
    read(&td.tiles_y, sizeof(u32));
    read(&td.tile_size, sizeof(f32));
    read(&td.layer_height, sizeof(f32));

    if (td.tiles_x == 0 || td.tiles_y == 0 || td.tiles_x > 4096 || td.tiles_y > 4096) {
        log::error(TAG, "Invalid terrain dimensions: {}x{}", td.tiles_x, td.tiles_y);
        return {};
    }

    u32 vc = td.vertex_count();
    u32 tc = td.tile_count();

    td.heightmap.resize(vc);
    td.cliff_level.resize(vc);
    for (auto& layer : td.splatmap) layer.resize(vc);
    td.pathing.resize(vc);
    td.water_height.resize(tc);

    read(td.heightmap.data(),   vc * sizeof(f32));
    read(td.cliff_level.data(), vc * sizeof(u8));
    for (u32 i = 0; i < 4; ++i)
        read(td.splatmap[i].data(), vc * sizeof(u8));
    read(td.pathing.data(),     vc * sizeof(u8));
    read(td.water_height.data(), tc * sizeof(f32));

    if (!file.good()) {
        log::error(TAG, "Failed to read terrain data from {}", path);
        return {};
    }

    log::info(TAG, "Loaded terrain: {}x{} tiles, tile_size={}, {} vertices from {}",
              td.tiles_x, td.tiles_y, td.tile_size, vc, path);
    return td;
}

} // namespace uldum::map
