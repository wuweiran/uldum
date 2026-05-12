#include "map/terrain_data.h"
#include "core/log.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace uldum::map {

static constexpr const char* TAG = "Terrain";

// ── Tile queries ─────────────────────────────────────────────────────────

i32 TerrainData::tile_effective_level(u32 tx, u32 ty) const {
    if (tx >= tiles_x || ty >= tiles_y) return -2;

    u8 c[4] = { cliff_at(tx, ty),     cliff_at(tx+1, ty),
                 cliff_at(tx, ty+1),   cliff_at(tx+1, ty+1) };
    u8 cmin = std::min({c[0], c[1], c[2], c[3]});
    u8 cmax = std::max({c[0], c[1], c[2], c[3]});

    if (cmin == cmax) return static_cast<i32>(cmin);

    if (cmax - cmin == 1) {
        bool all_ramp = (pathing_at(tx, ty)     & PATHING_RAMP) &&
                        (pathing_at(tx+1, ty)   & PATHING_RAMP) &&
                        (pathing_at(tx, ty+1)   & PATHING_RAMP) &&
                        (pathing_at(tx+1, ty+1) & PATHING_RAMP);
        if (all_ramp) return -1;
    }

    return -2;
}

bool TerrainData::is_tile_passable(u32 tx, u32 ty) const {
    if (tx >= tiles_x || ty >= tiles_y) return false;
    // tile_effective_level returns -2 for "uncrossable cliff transition"
    // and -1 for "ramp" (passable). Anything >= 0 is a flat tile at
    // that level. Per-vertex walkable / flyable bits are gone — the
    // runtime pathing-blocker layer is now the sole source of truth
    // for "this tile is occupied by a building".
    return tile_effective_level(tx, ty) != -2;
}

bool TerrainData::is_tile_deep_water(u32 tx, u32 ty) const {
    if (tx >= tiles_x || ty >= tiles_y) return false;
    return is_deep_water(tx, ty) || is_deep_water(tx+1, ty) ||
           is_deep_water(tx, ty+1) || is_deep_water(tx+1, ty+1);
}

glm::ivec2 TerrainData::world_to_tile(f32 x, f32 y) const {
    i32 tx = static_cast<i32>((x - origin_x()) / tile_size);
    i32 ty = static_cast<i32>((y - origin_y()) / tile_size);
    tx = std::clamp(tx, 0, static_cast<i32>(tiles_x - 1));
    ty = std::clamp(ty, 0, static_cast<i32>(tiles_y - 1));
    return {tx, ty};
}

glm::vec2 TerrainData::tile_center(u32 tx, u32 ty) const {
    return {origin_x() + (static_cast<f32>(tx) + 0.5f) * tile_size,
            origin_y() + (static_cast<f32>(ty) + 0.5f) * tile_size};
}

u8 TerrainData::cliff_level_at(f32 x, f32 y) const {
    u32 vx = static_cast<u32>(std::round((x - origin_x()) / tile_size));
    u32 vy = static_cast<u32>(std::round((y - origin_y()) / tile_size));
    vx = std::min(vx, tiles_x);
    vy = std::min(vy, tiles_y);
    return cliff_at(vx, vy);
}

// ── Ray-vs-heightmap (DDA + per-tile bilinear) ───────────────────────────

bool raycast_terrain(const TerrainData& td,
                     glm::vec3 ray_origin, glm::vec3 ray_dir,
                     glm::vec3& hit) {
    if (!td.is_valid()) return false;

    const f32 ts = td.tile_size;
    const f32 ox = td.origin_x();
    const f32 oy = td.origin_y();
    const i32 nx = static_cast<i32>(td.tiles_x);
    const i32 ny = static_cast<i32>(td.tiles_y);

    // ── 1. Clip the ray to the terrain XY bbox ───────────────────────
    // Slab test; produces [t_min, t_max] where the ray is inside the
    // terrain footprint. Rays that don't enter bail out cheaply.
    f32 t_min = 0.0f, t_max = 1e9f;
    auto clip_axis = [&](f32 o, f32 d, f32 lo, f32 hi) -> bool {
        if (std::abs(d) < 1e-9f) return o >= lo && o <= hi;  // parallel slab
        f32 inv = 1.0f / d;
        f32 t0 = (lo - o) * inv;
        f32 t1 = (hi - o) * inv;
        if (t0 > t1) std::swap(t0, t1);
        t_min = std::max(t_min, t0);
        t_max = std::min(t_max, t1);
        return t_min <= t_max;
    };
    if (!clip_axis(ray_origin.x, ray_dir.x, ox, ox + ts * nx)) return false;
    if (!clip_axis(ray_origin.y, ray_dir.y, oy, oy + ts * ny)) return false;

    // ── 2. DDA grid traversal (Amanatides-Woo) ───────────────────────
    // Walk one tile boundary at a time. Each iteration enters exactly
    // one new tile along the ray; no skips, no duplicates.
    glm::vec3 entry = ray_origin + ray_dir * t_min;
    i32 tx = std::clamp(static_cast<i32>(std::floor((entry.x - ox) / ts)), 0, nx - 1);
    i32 ty = std::clamp(static_cast<i32>(std::floor((entry.y - oy) / ts)), 0, ny - 1);

    i32 step_x = (ray_dir.x > 0) ? 1 : (ray_dir.x < 0 ? -1 : 0);
    i32 step_y = (ray_dir.y > 0) ? 1 : (ray_dir.y < 0 ? -1 : 0);
    f32 t_delta_x = (step_x != 0) ? std::abs(ts / ray_dir.x) : 1e9f;
    f32 t_delta_y = (step_y != 0) ? std::abs(ts / ray_dir.y) : 1e9f;

    auto next_boundary = [&](f32 origin_a, f32 dir_a, f32 axis_origin, i32 tile, i32 step) -> f32 {
        if (step == 0) return 1e9f;
        i32 target = (step > 0) ? (tile + 1) : tile;
        f32 boundary_world = axis_origin + static_cast<f32>(target) * ts;
        return (boundary_world - origin_a) / dir_a;
    };
    f32 t_next_x = next_boundary(ray_origin.x, ray_dir.x, ox, tx, step_x);
    f32 t_next_y = next_boundary(ray_origin.y, ray_dir.y, oy, ty, step_y);

    // ── 3. Per-tile analytical intersection ──────────────────────────
    // bilinear(lx, ly) = a + b·lx + c·ly + d·lx·ly. Substituting
    // lx(t) = Lx0 + Lx1·t, ly(t) = Ly0 + Ly1·t gives A·t² + B·t + C = 0
    // (linear when A == 0). Closed form, no march, no refine.
    f32 t_enter = t_min;
    while (t_enter <= t_max && tx >= 0 && ty >= 0 && tx < nx && ty < ny) {
        f32 t_exit = std::min({t_next_x, t_next_y, t_max});

        f32 h00 = td.world_z_at(static_cast<u32>(tx),     static_cast<u32>(ty));
        f32 h10 = td.world_z_at(static_cast<u32>(tx + 1), static_cast<u32>(ty));
        f32 h01 = td.world_z_at(static_cast<u32>(tx),     static_cast<u32>(ty + 1));
        f32 h11 = td.world_z_at(static_cast<u32>(tx + 1), static_cast<u32>(ty + 1));

        f32 a = h00;
        f32 b = h10 - h00;
        f32 c = h01 - h00;
        f32 d = h00 - h10 - h01 + h11;

        f32 inv_ts = 1.0f / ts;
        f32 vx = ox + static_cast<f32>(tx) * ts;
        f32 vy = oy + static_cast<f32>(ty) * ts;
        f32 Lx0 = (ray_origin.x - vx) * inv_ts;
        f32 Ly0 = (ray_origin.y - vy) * inv_ts;
        f32 Lx1 = ray_dir.x * inv_ts;
        f32 Ly1 = ray_dir.y * inv_ts;

        f32 A = d * Lx1 * Ly1;
        f32 B = b * Lx1 + c * Ly1 + d * (Lx0 * Ly1 + Lx1 * Ly0) - ray_dir.z;
        f32 C = a + b * Lx0 + c * Ly0 + d * Lx0 * Ly0 - ray_origin.z;

        auto in_tile_range = [&](f32 t) -> bool {
            constexpr f32 SLOP = 1e-3f;
            return t >= t_enter - SLOP && t <= t_exit + SLOP;
        };

        f32 t_hit = -1.0f;
        if (std::abs(A) < 1e-9f) {
            // Flat tile or ray parallel to one axis — linear in t.
            if (std::abs(B) > 1e-9f) {
                f32 t_candidate = -C / B;
                if (in_tile_range(t_candidate)) t_hit = t_candidate;
            }
        } else {
            f32 disc = B * B - 4.0f * A * C;
            if (disc >= 0.0f) {
                f32 sq = std::sqrt(disc);
                f32 t1 = (-B - sq) / (2.0f * A);
                f32 t2 = (-B + sq) / (2.0f * A);
                if (in_tile_range(t1)) t_hit = t1;
                if (t_hit < 0 && in_tile_range(t2)) t_hit = t2;
            }
        }

        if (t_hit >= 0) {
            glm::vec3 p = ray_origin + ray_dir * t_hit;
            f32 lx = (p.x - vx) * inv_ts;
            f32 ly = (p.y - vy) * inv_ts;
            hit.x = p.x;
            hit.y = p.y;
            hit.z = a + b * lx + c * ly + d * lx * ly;
            return true;
        }

        if (t_next_x < t_next_y) {
            tx += step_x;
            t_enter = t_next_x;
            t_next_x += t_delta_x;
        } else {
            ty += step_y;
            t_enter = t_next_y;
            t_next_y += t_delta_y;
        }
    }

    return false;
}

// ── Building placement snap ──────────────────────────────────────────────

f32 snap_building_axis(const TerrainData& td, f32 x, f32 origin, u32 footprint_extent) {
    if (!td.is_valid() || footprint_extent == 0) return x;
    f32 rel = (x - origin) / td.tile_size;
    if ((footprint_extent & 1u) != 0u) {
        // Odd footprint → tile center (half-tile offset).
        return origin + (std::floor(rel) + 0.5f) * td.tile_size;
    }
    // Even footprint → tile corner (vertex).
    return origin + std::round(rel) * td.tile_size;
}

f32 snap_cell_axis(const TerrainData& td, f32 x, f32 origin) {
    if (!td.is_valid()) return x;
    // PATHING_SUBDIV is 4 (lives in pathfinding.h, but duplicating the
    // 4 here keeps map/ from depending on simulation/).
    constexpr u32 PATHING_SUBDIV = 4;
    f32 cell_size = td.tile_size / static_cast<f32>(PATHING_SUBDIV);
    f32 rel = (x - origin) / cell_size;
    return origin + (std::floor(rel) + 0.5f) * cell_size;
}

// ── Terrain sampling (visual only) ───────────────────────────────────────

f32 sample_height(const TerrainData& td, f32 x, f32 y) {
    if (!td.is_valid()) return 0.0f;
    f32 fx = (x - td.origin_x()) / td.tile_size;
    f32 fy = (y - td.origin_y()) / td.tile_size;
    u32 ix = std::min(static_cast<u32>(std::floor(fx)), td.tiles_x - 1);
    u32 iy = std::min(static_cast<u32>(std::floor(fy)), td.tiles_y - 1);
    f32 tx = fx - static_cast<f32>(ix);
    f32 ty = fy - static_cast<f32>(iy);

    f32 h00 = td.world_z_at(ix, iy);
    f32 h10 = td.world_z_at(ix + 1, iy);
    f32 h01 = td.world_z_at(ix, iy + 1);
    f32 h11 = td.world_z_at(ix + 1, iy + 1);
    f32 h0 = h00 + tx * (h10 - h00);
    f32 h1 = h01 + tx * (h11 - h01);
    return h0 + ty * (h1 - h0);
}

glm::vec3 sample_normal(const TerrainData& td, f32 x, f32 y) {
    if (!td.is_valid()) return {0, 0, 1};
    f32 eps = td.tile_size * 0.5f;
    f32 hL = sample_height(td, x - eps, y);
    f32 hR = sample_height(td, x + eps, y);
    f32 hD = sample_height(td, x, y - eps);
    f32 hU = sample_height(td, x, y + eps);
    glm::vec3 ddx{2.0f * eps, 0.0f, hR - hL};
    glm::vec3 ddy{0.0f, 2.0f * eps, hU - hD};
    return glm::normalize(glm::cross(ddx, ddy));
}

// ── Terrain creation ─────────────────────────────────────────────────────

TerrainData create_flat_terrain(u32 tiles_x, u32 tiles_y, f32 tile_size, f32 base_height) {
    TerrainData td;
    td.tiles_x     = tiles_x;
    td.tiles_y     = tiles_y;
    td.tile_size   = tile_size;

    u32 vc = td.vertex_count();

    td.heightmap.assign(vc, base_height);
    td.cliff_level.assign(vc, 0);
    td.tile_layer.assign(vc, 0);  // default: layer 0
    td.pathing.assign(vc, 0);     // ramp marker only; no ramps on flat terrain

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

    td.heightmap.resize(vc);
    td.cliff_level.assign(vc, 0);
    td.tile_layer.assign(vc, 0);
    // pathing field stays for the RAMP bit; procedural terrain has
    // no ramps so it's all zeroes.
    td.pathing.assign(vc, 0);

    // Generate heights: two octaves of value noise
    for (u32 iy = 0; iy < td.verts_y(); ++iy) {
        for (u32 ix = 0; ix < td.verts_x(); ++ix) {
            f32 wx = static_cast<f32>(ix) * tile_size;
            f32 wy = static_cast<f32>(iy) * tile_size;

            f32 h = 0.0f;
            h += smooth_noise(wx * 0.001f, wy * 0.001f) * 64.0f;
            h += smooth_noise(wx * 0.003f, wy * 0.003f) * 16.0f;
            td.height_at(ix, iy) = h;
        }
    }

    // Assign tile layer based on height
    for (u32 iy = 0; iy < td.verts_y(); ++iy) {
        for (u32 ix = 0; ix < td.verts_x(); ++ix) {
            f32 h = td.height_at(ix, iy);
            u32 idx = iy * td.verts_x() + ix;
            if (h > 48.0f)       td.tile_layer[idx] = 2;  // stone
            else if (h > 24.0f)  td.tile_layer[idx] = 1;  // dirt
            else                 td.tile_layer[idx] = 0;  // grass
        }
    }

    log::info(TAG, "Created procedural terrain: {}x{} tiles, tile_size={}, {} vertices",
              tiles_x, tiles_y, tile_size, vc);
    return td;
}

// ── Binary serialization ─────────────────────────────────────────────────

// terrain.bin format:
// Terrain binary format:
//   tiles_x(u32), tiles_y(u32), tile_size(f32), layer_height(f32)
//   heightmap:    f32[vertex_count]
//   cliff_level:  u8[vertex_count]
//   tile_layer:   u8[vertex_count]
//   pathing:      u8[vertex_count]

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
    write(td.heightmap.data(),   vc * sizeof(f32));
    write(td.cliff_level.data(), vc * sizeof(u8));
    write(td.tile_layer.data(),  vc * sizeof(u8));
    write(td.pathing.data(),     vc * sizeof(u8));

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
    td.heightmap.resize(vc);
    td.cliff_level.resize(vc);
    td.tile_layer.resize(vc);
    td.pathing.resize(vc);

    read(td.heightmap.data(),   vc * sizeof(f32));
    read(td.cliff_level.data(), vc * sizeof(u8));
    read(td.tile_layer.data(),  vc * sizeof(u8));
    read(td.pathing.data(),     vc * sizeof(u8));

    if (!file.good()) {
        log::error(TAG, "Failed to read terrain data from {}", path);
        return {};
    }

    log::info(TAG, "Loaded terrain: {}x{} tiles, {} vertices from {}",
              td.tiles_x, td.tiles_y, vc, path);
    return td;
}

TerrainData load_terrain_from_memory(const u8* data, u32 size) {
    if (size < 16) return {};  // minimum: 4 u32s

    u32 pos = 0;
    auto read = [&](void* dst, u32 n) {
        if (pos + n <= size) std::memcpy(dst, data + pos, n);
        pos += n;
    };

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
    td.heightmap.resize(vc);
    td.cliff_level.resize(vc);
    td.tile_layer.resize(vc);
    td.pathing.resize(vc);

    read(td.heightmap.data(),   vc * sizeof(f32));
    read(td.cliff_level.data(), vc * sizeof(u8));
    read(td.tile_layer.data(),  vc * sizeof(u8));
    read(td.pathing.data(),     vc * sizeof(u8));

    if (pos > size) {
        log::error(TAG, "Terrain data truncated");
        return {};
    }

    log::info(TAG, "Loaded terrain from memory: {}x{} tiles, {} vertices", td.tiles_x, td.tiles_y, vc);
    return td;
}

} // namespace uldum::map
