#include "simulation/pathfinding.h"
#include "simulation/handle_types.h"
#include "simulation/world.h"
#include "map/terrain_data.h"
#include "core/log.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <queue>
#include <vector>

namespace uldum::simulation {

// ── Tile queries ─────────────────────────────────────────────────────────

bool Pathfinder::can_occupy(u32 tx, u32 ty, MoveType move_type) const {
    if (!m_terrain) return false;
    if (move_type == MoveType::Air) return true;
    if (!m_terrain->is_tile_passable(tx, ty)) return false;
    if (move_type == MoveType::Ground && m_terrain->is_tile_deep_water(tx, ty)) return false;

    // Runtime blocks (buildings): direct per-tile lookup. WC3-style
    // pathing — a building with a W×H footprint marks exactly W×H
    // tiles, adjacent tiles stay walkable.
    if (is_tile_blocked(tx, ty)) return false;
    return true;
}

bool Pathfinder::are_connected(u32 src_tx, u32 src_ty, u32 dst_tx, u32 dst_ty,
                                MoveType move_type) const {
    if (!m_terrain) return false;
    if (move_type == MoveType::Air) return true;

    // Both tiles must be occupiable
    if (!can_occupy(src_tx, src_ty, move_type)) return false;
    if (!can_occupy(dst_tx, dst_ty, move_type)) return false;

    // Check cliff level compatibility
    i32 src_eff = m_terrain->tile_effective_level(src_tx, src_ty);
    i32 dst_eff = m_terrain->tile_effective_level(dst_tx, dst_ty);

    // Either tile is a ramp: allow transition
    if (src_eff == -1 || dst_eff == -1) {
        // ok — ramp bridges levels
    } else {
        // Both flat: must be same effective level
        if (src_eff != dst_eff) return false;
    }

    auto& td = *m_terrain;

    // Check shared edge/corner vertices are walkable
    // Determine shared vertices between adjacent tiles
    u32 min_vx = std::max(src_tx, dst_tx);
    u32 max_vx = std::min(src_tx + 1, dst_tx + 1);
    u32 min_vy = std::max(src_ty, dst_ty);
    u32 max_vy = std::min(src_ty + 1, dst_ty + 1);

    for (u32 vy = min_vy; vy <= max_vy; ++vy) {
        for (u32 vx = min_vx; vx <= max_vx; ++vx) {
            // Ground units bounce off deep-water vertices. Per-vertex
            // walkable / flyable bits are gone (see terrain_data.h);
            // can_occupy already covers tile-level building blocks.
            if (move_type == MoveType::Ground && td.is_deep_water(vx, vy)) return false;
        }
    }

    // Diagonal movement: both cardinal intermediate tiles must be occupiable and connected
    i32 dtx = static_cast<i32>(dst_tx) - static_cast<i32>(src_tx);
    i32 dty = static_cast<i32>(dst_ty) - static_cast<i32>(src_ty);
    if (dtx != 0 && dty != 0) {
        // Check both cardinal neighbors
        u32 mid1_tx = static_cast<u32>(static_cast<i32>(src_tx) + dtx);
        u32 mid1_ty = src_ty;
        u32 mid2_tx = src_tx;
        u32 mid2_ty = static_cast<u32>(static_cast<i32>(src_ty) + dty);

        if (!can_occupy(mid1_tx, mid1_ty, move_type) ||
            !can_occupy(mid2_tx, mid2_ty, move_type))
            return false;
    }

    return true;
}

u8 Pathfinder::cliff_level_on_tile(u32 tx, u32 ty) const {
    if (!m_terrain || tx >= m_terrain->tiles_x || ty >= m_terrain->tiles_y) return 0;
    auto& td = *m_terrain;

    i32 eff = m_terrain->tile_effective_level(tx, ty);
    if (eff >= 0) return static_cast<u8>(eff);

    // Ramp: take the lowest corner level; the movement system re-derives
    // cliff_level per nearest vertex as the unit crosses.
    u8 c[4] = { td.cliff_at(tx, ty),     td.cliff_at(tx+1, ty),
                 td.cliff_at(tx, ty+1),   td.cliff_at(tx+1, ty+1) };
    return std::min({c[0], c[1], c[2], c[3]});
}

// ── Delegated helpers (convenience wrappers around TerrainData) ──────────

glm::ivec2 Pathfinder::world_to_tile(f32 x, f32 y) const {
    return (m_terrain && m_terrain->is_valid()) ? m_terrain->world_to_tile(x, y) : glm::ivec2{0, 0};
}

glm::ivec2 Pathfinder::world_to_cell(f32 x, f32 y) const {
    if (!m_terrain || !m_terrain->is_valid()) return {0, 0};
    f32 cs = cell_size();
    return { static_cast<i32>(std::floor((x - m_terrain->origin_x()) / cs)),
             static_cast<i32>(std::floor((y - m_terrain->origin_y()) / cs)) };
}

glm::vec2 Pathfinder::tile_center(u32 tx, u32 ty) const {
    return m_terrain ? m_terrain->tile_center(tx, ty) : glm::vec2{0};
}

glm::vec2 Pathfinder::cell_center(i32 cx, i32 cy) const {
    if (!m_terrain) return {0, 0};
    f32 cs = cell_size();
    return { m_terrain->origin_x() + (static_cast<f32>(cx) + 0.5f) * cs,
             m_terrain->origin_y() + (static_cast<f32>(cy) + 0.5f) * cs };
}

u8 Pathfinder::cliff_level_at(f32 x, f32 y) const {
    return (m_terrain && m_terrain->is_valid()) ? m_terrain->cliff_level_at(x, y) : 0;
}

f32 Pathfinder::tile_size() const {
    return (m_terrain && m_terrain->is_valid()) ? m_terrain->tile_size : 128.0f;
}

f32 Pathfinder::cell_size() const {
    return tile_size() / static_cast<f32>(PATHING_SUBDIV);
}

bool Pathfinder::is_cell_blocked(i32 cx, i32 cy) const {
    if (m_runtime_blocked_cells.empty() || !m_terrain) return false;
    if (cx < 0 || cy < 0) return false;
    if (static_cast<u32>(cx) >= m_cells_w || static_cast<u32>(cy) >= m_cells_h) return false;
    return m_runtime_blocked_cells[static_cast<u32>(cy) * m_cells_w + static_cast<u32>(cx)] > 0;
}

bool Pathfinder::can_occupy_cell(i32 cx, i32 cy, MoveType move_type) const {
    if (!m_terrain) return false;
    if (move_type == MoveType::Air) return true;
    if (cx < 0 || cy < 0) return false;
    if (static_cast<u32>(cx) >= m_cells_w || static_cast<u32>(cy) >= m_cells_h) return false;
    // Static cache (terrain passable + not deep water) was filled in
    // build_cache; fall through to the slow path if the map is too
    // small / pre-cache. Runtime blocker check stays per-call — that
    // bitmap changes whenever a building is placed.
    if (!m_static_occupiable_ground.empty()) {
        const u32 idx = static_cast<u32>(cy) * m_cells_w + static_cast<u32>(cx);
        if (!m_static_occupiable_ground[idx]) return false;
    } else {
        u32 tx = static_cast<u32>(cx) / PATHING_SUBDIV;
        u32 ty = static_cast<u32>(cy) / PATHING_SUBDIV;
        if (!m_terrain->is_tile_passable(tx, ty)) return false;
        if (m_terrain->is_tile_deep_water(tx, ty)) return false;
    }
    if (is_cell_blocked(cx, cy)) return false;
    return true;
}

// Terrain-level tile-to-tile connectivity (cliff levels, deep-water
// vertices) — the cliff/edge half of are_connected, WITHOUT the runtime-
// block check. Cell-level A* uses this for cross-tile transitions; the
// per-cell runtime block is checked separately by can_occupy_cell.
static bool tiles_terrain_connected(const map::TerrainData& td,
                                     u32 src_tx, u32 src_ty,
                                     u32 dst_tx, u32 dst_ty,
                                     MoveType move_type) {
    if (move_type == MoveType::Air) return true;
    if (!td.is_tile_passable(src_tx, src_ty)) return false;
    if (!td.is_tile_passable(dst_tx, dst_ty)) return false;
    if (move_type == MoveType::Ground) {
        if (td.is_tile_deep_water(src_tx, src_ty)) return false;
        if (td.is_tile_deep_water(dst_tx, dst_ty)) return false;
    }
    i32 src_eff = td.tile_effective_level(src_tx, src_ty);
    i32 dst_eff = td.tile_effective_level(dst_tx, dst_ty);
    if (src_eff != -1 && dst_eff != -1 && src_eff != dst_eff) return false;
    u32 min_vx = std::max(src_tx, dst_tx);
    u32 max_vx = std::min(src_tx + 1, dst_tx + 1);
    u32 min_vy = std::max(src_ty, dst_ty);
    u32 max_vy = std::min(src_ty + 1, dst_ty + 1);
    for (u32 vy = min_vy; vy <= max_vy; ++vy) {
        for (u32 vx = min_vx; vx <= max_vx; ++vx) {
            if (move_type == MoveType::Ground && td.is_deep_water(vx, vy)) return false;
        }
    }
    return true;
}

// ── Movement validation ──────────────────────────────────────────────────

bool Pathfinder::can_move_to(f32 old_x, f32 old_y, f32 new_x, f32 new_y,
                              MoveType move_type) const {
    if (!m_terrain || !m_terrain->is_valid()) return true;
    if (move_type == MoveType::Air) return true;

    // Destination block check at CELL granularity. A destructable's
    // footprint can straddle a tile boundary (a 2×2-cell tree snapped
    // to the cell grid lands on the corner shared by four tiles), so
    // is_tile_blocked's "any cell blocked → tile blocked" aggregate
    // would falsely reject moves into the clear portion of those
    // tiles. The cell test rejects only the actual blocked cells.
    glm::ivec2 dst_cell = world_to_cell(new_x, new_y);
    if (!can_occupy_cell(dst_cell.x, dst_cell.y, move_type)) return false;

    glm::ivec2 src_tile = world_to_tile(old_x, old_y);
    glm::ivec2 dst_tile = world_to_tile(new_x, new_y);
    if (src_tile == dst_tile) return true;

    // Cross-tile transition still respects cliff levels and deep
    // water, but skips the tile-level runtime-block aggregate (handled
    // by the cell check above).
    return tiles_terrain_connected(*m_terrain,
                                   static_cast<u32>(src_tile.x), static_cast<u32>(src_tile.y),
                                   static_cast<u32>(dst_tile.x), static_cast<u32>(dst_tile.y),
                                   move_type);
}

// ── A* on tile graph ─────────────────────────────────────────────────────

// 8 directions matching the A* neighbor order
static constexpr i32 s_dx[] = {-1, 0, 1, -1, 1, -1, 0, 1};
static constexpr i32 s_dy[] = {-1, -1, -1, 0, 0, 1, 1, 1};

void Pathfinder::build_cache() {
    // Keep the cached cell dimensions a faithful mirror of the current
    // terrain: a null / invalid terrain has zero cells, matching what the
    // old `tiles_x * PATHING_SUBDIV` recompute yielded for that case (so
    // block_cells / is_cell_blocked bound-check against 0 and no-op).
    if (!m_terrain || !m_terrain->is_valid()) {
        m_cells_w = 0;
        m_cells_h = 0;
        return;
    }
    auto& td = *m_terrain;
    u32 w = td.tiles_x, h = td.tiles_y;
    m_cache_w = w;
    m_cache_h = h;
    m_cells_w = w * PATHING_SUBDIV;
    m_cells_h = h * PATHING_SUBDIV;
    u32 count = w * h;

    m_occupiable.assign(count, false);
    m_effective_level.assign(count, -2);
    m_cliff_level.resize(count);
    m_connectivity.assign(count, 0);

    // Pass 1: per-tile properties
    for (u32 ty = 0; ty < h; ++ty) {
        for (u32 tx = 0; tx < w; ++tx) {
            u32 idx = ty * w + tx;
            m_occupiable[idx] = can_occupy(tx, ty, MoveType::Ground);
            m_effective_level[idx] = static_cast<i8>(td.tile_effective_level(tx, ty));
            m_cliff_level[idx] = cliff_level_on_tile(tx, ty);
        }
    }

    // Pass 2: per-tile 8-direction connectivity
    for (u32 ty = 0; ty < h; ++ty) {
        for (u32 tx = 0; tx < w; ++tx) {
            u32 idx = ty * w + tx;
            if (!m_occupiable[idx]) continue;

            u8 conn = 0;
            for (u32 d = 0; d < 8; ++d) {
                i32 nx = static_cast<i32>(tx) + s_dx[d];
                i32 ny = static_cast<i32>(ty) + s_dy[d];
                if (nx < 0 || ny < 0 || static_cast<u32>(nx) >= w || static_cast<u32>(ny) >= h)
                    continue;
                if (are_connected(tx, ty, static_cast<u32>(nx), static_cast<u32>(ny), MoveType::Ground))
                    conn |= (1u << d);
            }
            m_connectivity[idx] = conn;
        }
    }

    // Pass 3: per-cell static occupiable cache. Captures only the
    // never-changing terrain bits (passable + not deep-water); the
    // runtime block check is applied per call. Pre-baking these here
    // turns can_occupy_cell into one array read on the hot path.
    // Loop is tile-outer / cell-inner: compute the per-tile result
    // once, then broadcast to the SUBDIV×SUBDIV cell block. Saves a
    // factor of SUBDIV² (16× by default) tile-level lookups + divisions.
    {
        const u32 cells_total = m_cells_w * m_cells_h;
        m_static_occupiable_ground.assign(cells_total, 0);
        for (u32 ty = 0; ty < h; ++ty) {
            for (u32 tx = 0; tx < w; ++tx) {
                const u8 ok = (td.is_tile_passable(tx, ty)
                            && !td.is_tile_deep_water(tx, ty)) ? 1 : 0;
                if (!ok) continue;  // assign(…, 0) already covers the zero case
                const u32 c0x = tx * PATHING_SUBDIV;
                const u32 c0y = ty * PATHING_SUBDIV;
                for (u32 dy = 0; dy < PATHING_SUBDIV; ++dy) {
                    u8* row = &m_static_occupiable_ground[(c0y + dy) * m_cells_w + c0x];
                    for (u32 dx = 0; dx < PATHING_SUBDIV; ++dx) row[dx] = 1;
                }
            }
        }
    }

    // Visit-stamp scratch (used by A*'s generation-counter closed set).
    // Size matches the new cell count; bump m_visit_gen to 0 so the
    // lazy "if (++m_visit_gen == 0) clear" branch in find_corridor
    // doesn't trigger a spurious extra clear on the very first search.
    m_visit_stamp.assign(static_cast<size_t>(m_cells_w) * m_cells_h, 0);
    m_visit_gen = 0;

    // Connected-components storage: sized but not yet populated. The
    // per-MoveType dirty flags trigger a flood on the first find_corridor
    // that actually needs each map type, so maps that never use
    // Amphibious units never pay that flood cost.
    m_components_ground.assign(static_cast<size_t>(m_cells_w) * m_cells_h, 0);
    m_components_amphibious.assign(static_cast<size_t>(m_cells_w) * m_cells_h, 0);
    m_components_ground_dirty     = true;
    m_components_amphibious_dirty = true;

    log::info("Pathfind", "Connectivity cache built: {}x{} tiles", w, h);
}

// ── Connected components ─────────────────────────────────────────────────
//
// Build a per-cell component id map by BFS-flooding every walkable
// cell. Two grids — Ground and Amphibious — because deep water is
// walkable for the latter and not the former, so connectivity differs.
//
// Cardinal-only flood: an A* diagonal step requires both cardinal
// intermediates to also be walkable (corner-cut prevention). So any two
// cells reachable diagonally are reachable cardinally too — using only
// 4-neighbor flood for component assignment is conservatively correct
// and slightly simpler.
//
// Cliff levels: same-level tiles flood freely; a ramp tile is wired to
// both adjacent levels via tiles_terrain_connected, so the BFS naturally
// merges levels through ramps. End result: a hill and its valley share a
// component IFF a ramp connects them, which is exactly what A* would
// find. Buildings (runtime blocks) participate via is_cell_blocked, so
// a wall splits components correctly.
static void flood_components(const map::TerrainData& td,
                              const Pathfinder& pf, MoveType mt,
                              u32 cells_w, u32 cells_h,
                              std::vector<u32>& out) {
    out.assign(static_cast<size_t>(cells_w) * cells_h, 0);
    if (mt == MoveType::Air) return;  // air doesn't need components

    std::vector<u32> queue;
    queue.reserve(1024);
    u32 next_id = 1;

    auto idx_of = [&](u32 cx, u32 cy) { return cy * cells_w + cx; };

    for (u32 cy0 = 0; cy0 < cells_h; ++cy0) {
        for (u32 cx0 = 0; cx0 < cells_w; ++cx0) {
            const u32 seed = idx_of(cx0, cy0);
            if (out[seed] != 0) continue;
            if (!pf.can_occupy_cell(static_cast<i32>(cx0),
                                    static_cast<i32>(cy0), mt)) continue;

            // Start a new component. u32 ids give us ~4B distinct
            // components before any wrap concern; on a realistic map
            // we're talking hundreds at most.
            const u32 id = next_id++;
            out[seed] = id;
            queue.clear();
            queue.push_back(seed);

            while (!queue.empty()) {
                const u32 cur = queue.back();
                queue.pop_back();
                const u32 cx = cur % cells_w;
                const u32 cy = cur / cells_w;
                const u32 cur_tx = cx / PATHING_SUBDIV;
                const u32 cur_ty = cy / PATHING_SUBDIV;

                static constexpr i32 ndx[] = {-1, 1, 0, 0};
                static constexpr i32 ndy[] = { 0, 0,-1, 1};
                for (u32 d = 0; d < 4; ++d) {
                    const i32 nx = static_cast<i32>(cx) + ndx[d];
                    const i32 ny = static_cast<i32>(cy) + ndy[d];
                    if (nx < 0 || ny < 0
                        || static_cast<u32>(nx) >= cells_w
                        || static_cast<u32>(ny) >= cells_h) continue;
                    const u32 ni = idx_of(static_cast<u32>(nx), static_cast<u32>(ny));
                    if (out[ni] != 0) continue;
                    if (!pf.can_occupy_cell(nx, ny, mt)) continue;

                    // Cross-tile transitions need the cliff/edge
                    // connectivity check — same logic A* uses.
                    const u32 n_tx = static_cast<u32>(nx) / PATHING_SUBDIV;
                    const u32 n_ty = static_cast<u32>(ny) / PATHING_SUBDIV;
                    if ((n_tx != cur_tx || n_ty != cur_ty) &&
                        !tiles_terrain_connected(td, cur_tx, cur_ty, n_tx, n_ty, mt))
                        continue;

                    out[ni] = id;
                    queue.push_back(ni);
                }
            }
        }
    }
}

void Pathfinder::refresh_components_if_dirty(MoveType mt) const {
    if (!m_terrain || !m_terrain->is_valid()) return;
    if (mt == MoveType::Air) return;
    if (mt == MoveType::Amphibious) {
        if (!m_components_amphibious_dirty) return;
        flood_components(*m_terrain, *this, MoveType::Amphibious,
                         m_cells_w, m_cells_h, m_components_amphibious);
        m_components_amphibious_dirty = false;
    } else {
        if (!m_components_ground_dirty) return;
        flood_components(*m_terrain, *this, MoveType::Ground,
                         m_cells_w, m_cells_h, m_components_ground);
        m_components_ground_dirty = false;
    }
}

Corridor Pathfinder::find_corridor(glm::vec2 start, glm::vec2 goal,
                                    u8 start_cliff_level, MoveType move_type) const {
    Corridor result;
    if (!m_terrain || !m_terrain->is_valid()) return result;
    auto& td = *m_terrain;

    // Cell grid dimensions.
    const u32 cells_x = m_cells_w;
    const u32 cells_y = m_cells_h;

    // No per-tile unit-occupancy soft cost — the +5.0 bias made A*'s
    // heuristic underestimate and fan out exploring alternate routes.
    // local_steer handles unit-on-unit avoidance instead.

    glm::ivec2 s = world_to_cell(start.x, start.y);
    glm::ivec2 g = world_to_cell(goal.x, goal.y);

    auto in_bounds = [&](i32 cx, i32 cy) {
        return cx >= 0 && cy >= 0 &&
               static_cast<u32>(cx) < cells_x &&
               static_cast<u32>(cy) < cells_y;
    };

    if (!in_bounds(s.x, s.y) || !in_bounds(g.x, g.y)) return result;

    // Goal must be occupiable; if not, find the nearest walkable cell
    // to the goal. Search radius scales with SUBDIV so we cover the same
    // world-space distance as the old 10-tile ring.
    if (!can_occupy_cell(g.x, g.y, move_type)) {
        bool found = false;
        glm::ivec2 best_g = g;
        f32 best_d2_goal  = 1e30f;
        f32 best_d2_start = 1e30f;
        const i32 max_r = 10 * static_cast<i32>(PATHING_SUBDIV);
        for (i32 r = 1; r <= max_r && !found; ++r) {
            for (i32 dy = -r; dy <= r; ++dy) {
                for (i32 dx = -r; dx <= r; ++dx) {
                    if (std::abs(dx) != r && std::abs(dy) != r) continue;
                    i32 nx = g.x + dx, ny = g.y + dy;
                    if (!in_bounds(nx, ny)) continue;
                    if (!can_occupy_cell(nx, ny, move_type)) continue;
                    f32 d2_g = static_cast<f32>(dx*dx + dy*dy);
                    f32 sx   = static_cast<f32>(nx) - static_cast<f32>(s.x);
                    f32 sy   = static_cast<f32>(ny) - static_cast<f32>(s.y);
                    f32 d2_s = sx*sx + sy*sy;
                    if (d2_g < best_d2_goal ||
                        (d2_g == best_d2_goal && d2_s < best_d2_start)) {
                        best_d2_goal  = d2_g;
                        best_d2_start = d2_s;
                        best_g = {nx, ny};
                        found = true;
                    }
                }
            }
        }
        if (!found) return result;
        g = best_g;
    }

    // ── Connected-components pre-flight ─────────────────────────────────
    //
    // If the goal isn't in the same reachable region as the start, A*
    // would otherwise flood the entire start-component before giving up
    // — pathological worst case for the unreachable-goal scenario. We
    // catch it cheaply here: compare component ids, and if they differ,
    // retarget the goal to the closest cell that IS in the start's
    // component. The user wanted "walk as close as you can get," and
    // this delivers it without ever running A* on a doomed search.
    //
    // Air units skip the check (everything is reachable).
    //
    // If the start cell itself is unreachable (component id 0 — happens
    // when a unit gets knocked onto a freshly-blocked cell, or a
    // building is dropped on it, or terrain is repainted in-editor),
    // snap to the nearest occupiable cell so A* can still find an
    // escape path. Without this snap a unit that ends up on a blocked
    // cell goes inert until stuck-detection drops the order.
    refresh_components_if_dirty(move_type);
    if (move_type != MoveType::Air) {
        const auto& comps = (move_type == MoveType::Amphibious)
            ? m_components_amphibious : m_components_ground;
        if (!comps.empty()) {
            u32 start_comp = comps[static_cast<u32>(s.y) * cells_x + static_cast<u32>(s.x)];
            const u32 goal_comp  = comps[static_cast<u32>(g.y) * cells_x + static_cast<u32>(g.x)];
            if (start_comp == 0) {
                // Snap to the nearest occupiable cell; find_straight_waypoint
                // reconciles the unit's real position with the corridor start.
                bool snap_found = false;
                glm::ivec2 best_s = s;
                f32 best_d2 = 1e30f;
                const i32 max_r = 20 * static_cast<i32>(PATHING_SUBDIV);
                for (i32 r = 1; r <= max_r && !snap_found; ++r) {
                    for (i32 dy = -r; dy <= r; ++dy) {
                        for (i32 dx = -r; dx <= r; ++dx) {
                            if (std::abs(dx) != r && std::abs(dy) != r) continue;
                            i32 nx = s.x + dx, ny = s.y + dy;
                            if (!in_bounds(nx, ny)) continue;
                            const u32 c = comps[static_cast<u32>(ny) * cells_x + static_cast<u32>(nx)];
                            if (c == 0) continue;
                            f32 d2 = static_cast<f32>(dx*dx + dy*dy);
                            if (d2 < best_d2) {
                                best_d2 = d2;
                                best_s  = {nx, ny};
                                snap_found = true;
                            }
                        }
                    }
                }
                if (!snap_found) return result;  // truly nowhere to go
                s = best_s;
                start_comp = comps[static_cast<u32>(s.y) * cells_x + static_cast<u32>(s.x)];
            }
            if (start_comp != goal_comp) {
                // Goal is in a different region. Spiral out from g
                // looking for the closest cell in start_comp. Search a
                // generous radius — covers reasonable "go as close as
                // you can" cases. If nothing in start's component is
                // close to the goal, return invalid (caller treats as
                // "no path"; stuck-detection handles user feedback).
                bool found = false;
                glm::ivec2 best_g = g;
                f32 best_d2 = 1e30f;
                const i32 max_r = 64 * static_cast<i32>(PATHING_SUBDIV);
                for (i32 r = 1; r <= max_r && !found; ++r) {
                    for (i32 dy = -r; dy <= r; ++dy) {
                        for (i32 dx = -r; dx <= r; ++dx) {
                            if (std::abs(dx) != r && std::abs(dy) != r) continue;
                            i32 nx = g.x + dx, ny = g.y + dy;
                            if (!in_bounds(nx, ny)) continue;
                            const u32 c = comps[static_cast<u32>(ny) * cells_x + static_cast<u32>(nx)];
                            if (c != start_comp) continue;
                            f32 d2 = static_cast<f32>(dx*dx + dy*dy);
                            if (d2 < best_d2) {
                                best_d2 = d2;
                                best_g  = {nx, ny};
                                found = true;
                            }
                        }
                    }
                }
                if (!found) return result;
                g = best_g;
            }
        }
    }

    if (s == g) {
        result.cells.push_back(s);
        result.valid = true;
        return result;
    }

    // Closed set: persistent stamp array, generation-counter cleared.
    // Bump the generation; "closed" = m_visit_stamp[idx] == m_visit_gen.
    // On rollover the stamp array clears once so old stamps can't match.
    const size_t cells_total = static_cast<size_t>(cells_x) * cells_y;
    if (m_visit_stamp.size() < cells_total) m_visit_stamp.assign(cells_total, 0);
    if (++m_visit_gen == 0) {
        std::fill(m_visit_stamp.begin(), m_visit_stamp.end(), 0);
        m_visit_gen = 1;
    }
    const u32 gen = m_visit_gen;
    auto cell_idx = [&](u32 cx, u32 cy) -> u32 { return cy * cells_x + cx; };

    std::priority_queue<AStarNode, std::vector<AStarNode>, NodeCompare> open;
    m_astar_nodes.clear();

    // Octile distance — exact minimum cost on an 8-direction grid where
    // cardinal moves cost 1 and diagonals cost √2. Tighter than Euclidean
    // (which underestimates because it pretends you can fly), so A* fans
    // out less and commits to a direction sooner. Still admissible →
    // optimal paths preserved.
    static constexpr f32 SQRT2_MINUS_1 = 0.41421356f;
    auto heuristic = [&](u32 cx, u32 cy) -> f32 {
        f32 dx = std::fabs(static_cast<f32>(cx) - static_cast<f32>(g.x));
        f32 dy = std::fabs(static_cast<f32>(cy) - static_cast<f32>(g.y));
        f32 lo = std::min(dx, dy);
        f32 hi = std::max(dx, dy);
        return hi + SQRT2_MINUS_1 * lo;
    };

    open.push({static_cast<u32>(s.x), static_cast<u32>(s.y),
               0, heuristic(s.x, s.y), UINT32_MAX, start_cliff_level});

    static constexpr i32 dx[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static constexpr i32 dy[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    static constexpr f32 cost[] = {1.414f, 1.0f, 1.414f, 1.0f, 1.0f, 1.414f, 1.0f, 1.414f};

    // Safety net only — the CC pre-flight above already guarantees the
    // goal is reachable. The cap protects against pathological detours
    // where the heuristic underestimates and A* fans out a large part of
    // the component. On a hit, the closest-reachable fallback below
    // returns a partial path and the unit re-paths.
    static constexpr u32 max_iterations = 1024;
    u32 iterations = 0;

    u32 best_reachable_idx = UINT32_MAX;
    f32 best_reachable_h = 1e9f;

    while (!open.empty()) {
        AStarNode current = open.top();
        open.pop();

        const u32 ck = cell_idx(current.tx, current.ty);
        if (m_visit_stamp[ck] == gen) continue;   // already closed
        m_visit_stamp[ck] = gen;
        if (iterations++ >= max_iterations) break;

        u32 current_idx = static_cast<u32>(m_astar_nodes.size());
        m_astar_nodes.push_back(current);

        f32 h = heuristic(current.tx, current.ty);
        if (h < best_reachable_h) {
            best_reachable_h = h;
            best_reachable_idx = current_idx;
        }

        if (current.tx == static_cast<u32>(g.x) && current.ty == static_cast<u32>(g.y)) {
            u32 idx = current_idx;
            while (idx != UINT32_MAX) {
                auto& n = m_astar_nodes[idx];
                result.cells.push_back({static_cast<i32>(n.tx), static_cast<i32>(n.ty)});
                idx = n.parent_idx;
            }
            std::reverse(result.cells.begin(), result.cells.end());
            result.valid = true;
            return result;
        }

        // Containing tile of the current cell — used for cross-tile cliff
        // and edge-vertex connectivity. Cells within the same tile share
        // a cliff level and need only the cell-level occupancy check.
        u32 cur_tx = current.tx / PATHING_SUBDIV;
        u32 cur_ty = current.ty / PATHING_SUBDIV;

        for (u32 i = 0; i < 8; ++i) {
            i32 ncx = static_cast<i32>(current.tx) + dx[i];
            i32 ncy = static_cast<i32>(current.ty) + dy[i];
            if (!in_bounds(ncx, ncy)) continue;

            u32 ucx = static_cast<u32>(ncx);
            u32 ucy = static_cast<u32>(ncy);

            if (m_visit_stamp[cell_idx(ucx, ucy)] == gen) continue;
            if (!can_occupy_cell(ncx, ncy, move_type)) continue;

            // Diagonal corner-cut prevention at cell granularity. We
            // also require both cardinal intermediates to be cliff/edge
            // connected when they cross tile boundaries — matches the
            // flood used to build connected components (which uses
            // cardinal-only steps). Without this, A* could "jump" via a
            // diagonal across a cliff edge that CC considers a wall,
            // and the CC pre-flight would falsely route the goal away
            // from a reachable destination.
            if (dx[i] != 0 && dy[i] != 0) {
                const i32 ix1 = static_cast<i32>(current.tx) + dx[i];
                const i32 iy1 = static_cast<i32>(current.ty);
                const i32 ix2 = static_cast<i32>(current.tx);
                const i32 iy2 = static_cast<i32>(current.ty) + dy[i];
                if (!can_occupy_cell(ix1, iy1, move_type)) continue;
                if (!can_occupy_cell(ix2, iy2, move_type)) continue;
                const u32 i1_tx = static_cast<u32>(ix1) / PATHING_SUBDIV;
                const u32 i1_ty = static_cast<u32>(iy1) / PATHING_SUBDIV;
                const u32 i2_tx = static_cast<u32>(ix2) / PATHING_SUBDIV;
                const u32 i2_ty = static_cast<u32>(iy2) / PATHING_SUBDIV;
                if ((i1_tx != cur_tx || i1_ty != cur_ty) &&
                    !tiles_terrain_connected(td, cur_tx, cur_ty, i1_tx, i1_ty, move_type))
                    continue;
                if ((i2_tx != cur_tx || i2_ty != cur_ty) &&
                    !tiles_terrain_connected(td, cur_tx, cur_ty, i2_tx, i2_ty, move_type))
                    continue;
            }

            // Cross-tile transitions need a cliff / edge-vertex check.
            u32 n_tx = ucx / PATHING_SUBDIV;
            u32 n_ty = ucy / PATHING_SUBDIV;
            if ((n_tx != cur_tx || n_ty != cur_ty) &&
                !tiles_terrain_connected(td, cur_tx, cur_ty, n_tx, n_ty, move_type))
                continue;

            // Cliff level inherits from the destination cell's containing
            // tile. Ramps bridge two levels — pick the one we're entering.
            u32 t_idx = n_ty * m_cache_w + n_tx;
            u8 new_cliff = m_cliff_level[t_idx];
            i8 eff = m_effective_level[t_idx];
            if (eff == -1) {
                new_cliff = current.cliff_level;
                u8 ramp_min = m_cliff_level[t_idx];
                u8 ramp_max = ramp_min + 1;
                if (current.cliff_level == ramp_min) new_cliff = ramp_max;
                else if (current.cliff_level == ramp_max) new_cliff = ramp_min;
            } else if (eff >= 0) {
                new_cliff = static_cast<u8>(eff);
            }

            f32 step_cost = cost[i];
            f32 new_g = current.g_cost + step_cost;
            f32 new_f = new_g + heuristic(ucx, ucy);
            open.push({ucx, ucy, new_g, new_f, current_idx, new_cliff});
        }
    }

    // Goal unreachable — fall back to closest reachable cell.
    if (best_reachable_idx != UINT32_MAX && best_reachable_idx != 0) {
        u32 idx = best_reachable_idx;
        while (idx != UINT32_MAX) {
            auto& n = m_astar_nodes[idx];
            result.cells.push_back({static_cast<i32>(n.tx), static_cast<i32>(n.ty)});
            idx = n.parent_idx;
        }
        std::reverse(result.cells.begin(), result.cells.end());
        result.valid = true;
    }

    return result;
}

// ── Straight-line waypoint through corridor ──────────────────────────────

// Straight-line passability — samples cells along the segment with a
// step <= half a cell. Accounts for collision radius via two
// perpendicular offsets per sample. Operates on cells, not tiles.
static bool line_passable(glm::vec2 a, glm::vec2 b, f32 step_size, f32 collision_radius,
                          u8 cliff_level, const Pathfinder& pf, MoveType move_type) {
    glm::vec2 dir = b - a;
    f32 len = glm::length(dir);
    if (len < 0.001f) return true;

    glm::vec2 fwd = dir / len;
    glm::vec2 perp{-fwd.y, fwd.x};

    // Half a cell — keeps adjacent samples in adjacent cells.
    f32 step = step_size * 0.5f;
    if (step < 1.0f) step = 1.0f;
    u32 steps = static_cast<u32>(len / step) + 1;

    for (u32 i = 0; i <= steps; ++i) {
        f32 t = static_cast<f32>(i) / static_cast<f32>(steps);
        glm::vec2 pt = a + dir * t;

        glm::ivec2 cell = pf.world_to_cell(pt.x, pt.y);
        if (!pf.can_occupy_cell(cell.x, cell.y, move_type)) return false;
        // Cliff level still lives at tile granularity.
        i32 tx = cell.x / static_cast<i32>(PATHING_SUBDIV);
        i32 ty = cell.y / static_cast<i32>(PATHING_SUBDIV);
        i32 eff = pf.terrain()->tile_effective_level(tx, ty);
        if (eff >= 0 && static_cast<u8>(eff) != cliff_level) return false;

        if (collision_radius > 0) {
            glm::vec2 offsets[] = {
                pt + perp * collision_radius,
                pt - perp * collision_radius,
            };
            for (auto& off : offsets) {
                glm::ivec2 oc = pf.world_to_cell(off.x, off.y);
                if (oc == cell) continue;
                if (!pf.can_occupy_cell(oc.x, oc.y, move_type)) return false;
            }
        }
    }
    return true;
}

glm::vec2 Pathfinder::find_straight_waypoint(glm::vec2 from, std::span<const glm::ivec2> corridor_cells,
                                              f32 collision_radius, u8 cliff_level,
                                              MoveType move_type) const {
    if (!m_terrain || corridor_cells.empty()) return from;
    f32 cs = cell_size();

    // Find which corridor cell we're on (or nearest).
    u32 start_idx = 0;
    {
        glm::ivec2 cur = world_to_cell(from.x, from.y);
        for (u32 i = 0; i < corridor_cells.size(); ++i) {
            if (corridor_cells[i] == cur) { start_idx = i; break; }
        }
    }

    u32 end = static_cast<u32>(corridor_cells.size());

    // Phase 1: exponential jumps (1, 2, 4, 8...) to find the rough limit.
    u32 last_good = start_idx;
    u32 first_bad = end;
    for (u32 jump = 1; ; jump *= 2) {
        u32 test_idx = start_idx + jump;
        if (test_idx >= end) test_idx = end - 1;

        glm::vec2 target = cell_center(corridor_cells[test_idx].x, corridor_cells[test_idx].y);
        if (line_passable(from, target, cs, collision_radius, cliff_level, *this, move_type)) {
            last_good = test_idx;
            if (test_idx >= end - 1) break;
        } else {
            first_bad = test_idx;
            break;
        }
    }

    // Phase 2: binary search between last_good and first_bad.
    while (last_good + 1 < first_bad) {
        u32 mid = (last_good + first_bad) / 2;
        glm::vec2 target = cell_center(corridor_cells[mid].x, corridor_cells[mid].y);
        if (line_passable(from, target, cs, collision_radius, cliff_level, *this, move_type)) {
            last_good = mid;
        } else {
            first_bad = mid;
        }
    }

    glm::vec2 best = cell_center(corridor_cells[last_good].x, corridor_cells[last_good].y);
    if (last_good == start_idx && start_idx + 1 < end) {
        best = cell_center(corridor_cells[start_idx + 1].x, corridor_cells[start_idx + 1].y);
    }
    return best;
}

// ── Recovery ─────────────────────────────────────────────────────────────

glm::vec2 Pathfinder::find_nearest_valid(f32 x, f32 y, MoveType move_type) const {
    if (!m_terrain || !m_terrain->is_valid()) return {x, y};

    glm::ivec2 c = world_to_cell(x, y);
    if (can_occupy_cell(c.x, c.y, move_type)) return {x, y};

    // Ring radius in cells. The old code searched 20 tiles; scale to
    // preserve world-space reach.
    const i32 max_r = 20 * static_cast<i32>(PATHING_SUBDIV);
    for (i32 r = 1; r <= max_r; ++r) {
        f32 best_dist = 1e9f;
        glm::vec2 best{x, y};
        bool found = false;

        for (i32 dy = -r; dy <= r; ++dy) {
            for (i32 dx = -r; dx <= r; ++dx) {
                if (std::abs(dx) != r && std::abs(dy) != r) continue;
                i32 nx = c.x + dx, ny = c.y + dy;
                if (!can_occupy_cell(nx, ny, move_type)) continue;

                glm::vec2 center = cell_center(nx, ny);
                f32 dist = glm::length(center - glm::vec2{x, y});
                if (dist < best_dist) {
                    best_dist = dist;
                    best = center;
                    found = true;
                }
            }
        }
        if (found) return best;
    }
    return {x, y};
}

// ── Runtime pathing blocks ───────────────────────────────────────────────

void Pathfinder::block_cells(i32 cx, i32 cy, u32 w, u32 h) {
    if (!m_terrain || w == 0 || h == 0) return;
    const u32 cells_x = m_cells_w;
    const u32 cells_y = m_cells_h;
    if (m_runtime_blocked_cells.empty()) {
        m_runtime_blocked_cells.resize(cells_x * cells_y, 0);
    }
    for (u32 dy = 0; dy < h; ++dy) {
        i32 y = cy + static_cast<i32>(dy);
        if (y < 0 || static_cast<u32>(y) >= cells_y) continue;
        for (u32 dx = 0; dx < w; ++dx) {
            i32 x = cx + static_cast<i32>(dx);
            if (x < 0 || static_cast<u32>(x) >= cells_x) continue;
            u32 idx = static_cast<u32>(y) * cells_x + static_cast<u32>(x);
            if (m_runtime_blocked_cells[idx] < 255) m_runtime_blocked_cells[idx]++;
        }
    }
    // Blocked cells can split a previously connected region in two —
    // mark both MoveType component maps stale. The next find_corridor
    // refloods only the map for its requested MoveType, so games that
    // never use Amphibious pay nothing.
    m_components_ground_dirty     = true;
    m_components_amphibious_dirty = true;
}

void Pathfinder::unblock_cells(i32 cx, i32 cy, u32 w, u32 h) {
    if (m_runtime_blocked_cells.empty() || !m_terrain) return;
    const u32 cells_x = m_cells_w;
    const u32 cells_y = m_cells_h;
    for (u32 dy = 0; dy < h; ++dy) {
        i32 y = cy + static_cast<i32>(dy);
        if (y < 0 || static_cast<u32>(y) >= cells_y) continue;
        for (u32 dx = 0; dx < w; ++dx) {
            i32 x = cx + static_cast<i32>(dx);
            if (x < 0 || static_cast<u32>(x) >= cells_x) continue;
            u32 idx = static_cast<u32>(y) * cells_x + static_cast<u32>(x);
            if (m_runtime_blocked_cells[idx] > 0) m_runtime_blocked_cells[idx]--;
        }
    }
    // Unblocked cells may merge two previously separate components —
    // same flag pair, same refresh path.
    m_components_ground_dirty     = true;
    m_components_amphibious_dirty = true;
}

void Pathfinder::block_tiles(i32 tx, i32 ty, u32 w, u32 h) {
    block_cells(tx * static_cast<i32>(PATHING_SUBDIV),
                ty * static_cast<i32>(PATHING_SUBDIV),
                w * PATHING_SUBDIV,
                h * PATHING_SUBDIV);
}

void Pathfinder::unblock_tiles(i32 tx, i32 ty, u32 w, u32 h) {
    unblock_cells(tx * static_cast<i32>(PATHING_SUBDIV),
                  ty * static_cast<i32>(PATHING_SUBDIV),
                  w * PATHING_SUBDIV,
                  h * PATHING_SUBDIV);
}

bool Pathfinder::is_tile_blocked(u32 tx, u32 ty) const {
    if (m_runtime_blocked_cells.empty() || !m_terrain) return false;
    if (tx >= m_terrain->tiles_x || ty >= m_terrain->tiles_y) return false;
    const u32 cells_x = m_cells_w;
    u32 c0x = tx * PATHING_SUBDIV;
    u32 c0y = ty * PATHING_SUBDIV;
    for (u32 dy = 0; dy < PATHING_SUBDIV; ++dy) {
        for (u32 dx = 0; dx < PATHING_SUBDIV; ++dx) {
            if (m_runtime_blocked_cells[(c0y + dy) * cells_x + (c0x + dx)] > 0)
                return true;
        }
    }
    return false;
}

} // namespace uldum::simulation
