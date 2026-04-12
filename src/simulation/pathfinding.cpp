#include "simulation/pathfinding.h"
#include "simulation/handle_types.h"
#include "simulation/world.h"
#include "map/terrain_data.h"
#include "core/log.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_set>
#include <vector>

namespace uldum::simulation {

// ── Tile queries ─────────────────────────────────────────────────────────

bool Pathfinder::can_occupy(u32 tx, u32 ty, MoveType move_type) const {
    if (!m_terrain) return false;
    if (move_type == MoveType::Air) return true;
    if (!m_terrain->is_tile_passable(tx, ty)) return false;
    if (move_type == MoveType::Ground && m_terrain->is_tile_water(tx, ty)) return false;

    // Runtime blocks (buildings): all 4 corners must be unblocked
    if (!m_runtime_blocked.empty()) {
        if (is_vertex_blocked(tx, ty) || is_vertex_blocked(tx+1, ty) ||
            is_vertex_blocked(tx, ty+1) || is_vertex_blocked(tx+1, ty+1))
            return false;
    }
    return true;
}

bool Pathfinder::are_connected(u32 src_tx, u32 src_ty, u32 dst_tx, u32 dst_ty,
                                u8 cliff_level, MoveType move_type) const {
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
            u8 flags = td.pathing_at(vx, vy);
            bool walkable = (flags & map::PATHING_WALKABLE) != 0;
            bool water = td.is_water(vx, vy);
            if (move_type == MoveType::Ground && (!walkable || water)) return false;
            if (move_type == MoveType::Amphibious && !walkable) return false;
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

    // Ramp: use min corner level (entering from low side) or max (from high side).
    // Use the average — the movement system will update cliff_level based on nearest vertex.
    u8 c[4] = { td.cliff_at(tx, ty),     td.cliff_at(tx+1, ty),
                 td.cliff_at(tx, ty+1),   td.cliff_at(tx+1, ty+1) };
    return std::min({c[0], c[1], c[2], c[3]});
}

// ── Delegated helpers (convenience wrappers around TerrainData) ──────────

glm::ivec2 Pathfinder::world_to_tile(f32 x, f32 y) const {
    return (m_terrain && m_terrain->is_valid()) ? m_terrain->world_to_tile(x, y) : glm::ivec2{0, 0};
}

glm::vec2 Pathfinder::tile_center(u32 tx, u32 ty) const {
    return m_terrain ? m_terrain->tile_center(tx, ty) : glm::vec2{0};
}

u8 Pathfinder::cliff_level_at(f32 x, f32 y) const {
    return (m_terrain && m_terrain->is_valid()) ? m_terrain->cliff_level_at(x, y) : 0;
}

f32 Pathfinder::tile_size() const {
    return (m_terrain && m_terrain->is_valid()) ? m_terrain->tile_size : 128.0f;
}

// ── Movement validation ──────────────────────────────────────────────────

bool Pathfinder::can_move_to(f32 old_x, f32 old_y, f32 new_x, f32 new_y,
                              u8 cliff_level, MoveType move_type) const {
    if (!m_terrain || !m_terrain->is_valid()) return true;
    if (move_type == MoveType::Air) return true;

    glm::ivec2 src = world_to_tile(old_x, old_y);
    glm::ivec2 dst = world_to_tile(new_x, new_y);

    if (src == dst) return true;  // same tile
    if (!can_occupy(dst.x, dst.y, move_type)) return false;

    return are_connected(src.x, src.y, dst.x, dst.y, cliff_level, move_type);
}

// ── A* on tile graph ─────────────────────────────────────────────────────

struct AStarNode {
    u32 tx, ty;
    f32 g_cost;
    f32 f_cost;
    u32 parent_idx;
    u8  cliff_level;
};

struct NodeCompare {
    bool operator()(const AStarNode& a, const AStarNode& b) const {
        return a.f_cost > b.f_cost;
    }
};

static u64 tile_key(u32 tx, u32 ty) {
    return (static_cast<u64>(ty) << 32) | tx;
}

// Build a set of tiles occupied by units (for A* cost penalty).
// Returns a set of tile keys where non-self units are standing.
static std::unordered_set<u64> build_unit_tile_set(const World* world, u32 self_id, f32 tile_size, u32 tiles_x, u32 tiles_y) {
    std::unordered_set<u64> occupied;
    if (!world) return occupied;

    for (u32 i = 0; i < world->transforms.count(); ++i) {
        u32 id = world->transforms.ids()[i];
        if (id == self_id) continue;
        if (world->dead_states.has(id)) continue;

        // Only consider units (not doodads, projectiles, etc.)
        auto* info = world->handle_infos.get(id);
        if (!info || info->category != Category::Unit) continue;

        auto& pos = world->transforms.data()[i].position;
        i32 tx = static_cast<i32>(pos.x / tile_size);
        i32 ty = static_cast<i32>(pos.y / tile_size);
        if (tx >= 0 && ty >= 0 &&
            static_cast<u32>(tx) < tiles_x && static_cast<u32>(ty) < tiles_y) {
            occupied.insert(tile_key(tx, ty));
        }
    }
    return occupied;
}

Corridor Pathfinder::find_corridor(glm::vec2 start, glm::vec2 goal,
                                    u8 start_cliff_level, MoveType move_type,
                                    const World* world, u32 self_id) const {
    Corridor result;
    if (!m_terrain || !m_terrain->is_valid()) return result;
    auto& td = *m_terrain;

    // Build unit occupancy for cost penalty
    auto unit_tiles = build_unit_tile_set(world, self_id, td.tile_size, td.tiles_x, td.tiles_y);
    static constexpr f32 UNIT_COST_PENALTY = 5.0f;

    glm::ivec2 s = world_to_tile(start.x, start.y);
    glm::ivec2 g = world_to_tile(goal.x, goal.y);

    // Goal must be occupiable; if not, find nearest occupiable tile
    if (!can_occupy(g.x, g.y, move_type)) {
        // Search in expanding rings
        bool found = false;
        for (i32 r = 1; r <= 10 && !found; ++r) {
            for (i32 dy = -r; dy <= r && !found; ++dy) {
                for (i32 dx = -r; dx <= r; ++dx) {
                    if (std::abs(dx) != r && std::abs(dy) != r) continue;
                    i32 nx = g.x + dx, ny = g.y + dy;
                    if (nx < 0 || ny < 0 ||
                        static_cast<u32>(nx) >= td.tiles_x ||
                        static_cast<u32>(ny) >= td.tiles_y) continue;
                    if (can_occupy(nx, ny, move_type)) {
                        g = {nx, ny};
                        found = true;
                        break;
                    }
                }
            }
        }
        if (!found) return result;
    }

    // Same tile: trivial corridor
    if (s == g) {
        result.tiles.push_back(s);
        result.valid = true;
        return result;
    }

    std::priority_queue<AStarNode, std::vector<AStarNode>, NodeCompare> open;
    std::unordered_set<u64> closed;
    std::vector<AStarNode> all_nodes;
    all_nodes.reserve(256);

    auto heuristic = [&](u32 tx, u32 ty) -> f32 {
        f32 dx = static_cast<f32>(tx) - static_cast<f32>(g.x);
        f32 dy = static_cast<f32>(ty) - static_cast<f32>(g.y);
        return std::sqrt(dx * dx + dy * dy);
    };

    open.push({static_cast<u32>(s.x), static_cast<u32>(s.y),
               0, heuristic(s.x, s.y), UINT32_MAX, start_cliff_level});

    // 8 directions: dx, dy, cost
    static constexpr i32 dx[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static constexpr i32 dy[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    static constexpr f32 cost[] = {1.414f, 1.0f, 1.414f, 1.0f, 1.0f, 1.414f, 1.0f, 1.414f};

    u32 max_iterations = td.tiles_x * td.tiles_y;
    u32 iterations = 0;

    // Track the closest reachable node to the goal (fallback if goal is unreachable)
    u32 best_reachable_idx = UINT32_MAX;
    f32 best_reachable_h = 1e9f;

    while (!open.empty() && iterations++ < max_iterations) {
        AStarNode current = open.top();
        open.pop();

        u64 key = tile_key(current.tx, current.ty);
        if (closed.contains(key)) continue;
        closed.insert(key);

        u32 current_idx = static_cast<u32>(all_nodes.size());
        all_nodes.push_back(current);

        // Track closest to goal
        f32 h = heuristic(current.tx, current.ty);
        if (h < best_reachable_h) {
            best_reachable_h = h;
            best_reachable_idx = current_idx;
        }

        // Goal reached
        if (current.tx == static_cast<u32>(g.x) && current.ty == static_cast<u32>(g.y)) {
            u32 idx = current_idx;
            while (idx != UINT32_MAX) {
                auto& n = all_nodes[idx];
                result.tiles.push_back({static_cast<i32>(n.tx), static_cast<i32>(n.ty)});
                idx = n.parent_idx;
            }
            std::reverse(result.tiles.begin(), result.tiles.end());
            result.valid = true;
            return result;
        }

        // Expand neighbors
        for (u32 i = 0; i < 8; ++i) {
            i32 nx = static_cast<i32>(current.tx) + dx[i];
            i32 ny = static_cast<i32>(current.ty) + dy[i];

            if (nx < 0 || ny < 0 ||
                static_cast<u32>(nx) >= td.tiles_x ||
                static_cast<u32>(ny) >= td.tiles_y)
                continue;

            u32 ntx = static_cast<u32>(nx);
            u32 nty = static_cast<u32>(ny);

            if (closed.contains(tile_key(ntx, nty))) continue;

            if (!are_connected(current.tx, current.ty, ntx, nty,
                               current.cliff_level, move_type))
                continue;

            u8 new_cliff = cliff_level_on_tile(ntx, nty);
            i32 eff = m_terrain->tile_effective_level(ntx, nty);
            if (eff == -1) {
                new_cliff = current.cliff_level;
                u8 ramp_min = cliff_level_on_tile(ntx, nty);
                u8 ramp_max = ramp_min + 1;
                if (current.cliff_level == ramp_min) new_cliff = ramp_max;
                else if (current.cliff_level == ramp_max) new_cliff = ramp_min;
            } else if (eff >= 0) {
                new_cliff = static_cast<u8>(eff);
            }

            f32 tile_cost = cost[i];
            if (unit_tiles.contains(tile_key(ntx, nty))) {
                tile_cost += UNIT_COST_PENALTY;
            }
            f32 new_g = current.g_cost + tile_cost;
            f32 new_f = new_g + heuristic(ntx, nty);
            open.push({ntx, nty, new_g, new_f, current_idx, new_cliff});
        }
    }

    // Goal unreachable — move to the closest reachable tile instead
    if (best_reachable_idx != UINT32_MAX && best_reachable_idx != 0) {
        u32 idx = best_reachable_idx;
        while (idx != UINT32_MAX) {
            auto& n = all_nodes[idx];
            result.tiles.push_back({static_cast<i32>(n.tx), static_cast<i32>(n.ty)});
            idx = n.parent_idx;
        }
        std::reverse(result.tiles.begin(), result.tiles.end());
        result.valid = true;
    }

    return result;
}

// ── Straight-line waypoint through corridor ──────────────────────────────

// Check if a straight line from A to B is passable on actual terrain.
// Not limited to the A* corridor — any passable tile is fine.
// Accounts for collision radius (swept capsule along the line).
static bool line_passable(glm::vec2 a, glm::vec2 b, f32 tile_size, f32 collision_radius,
                          u8 cliff_level, const Pathfinder& pf, MoveType move_type) {
    glm::vec2 dir = b - a;
    f32 len = glm::length(dir);
    if (len < 0.001f) return true;

    glm::vec2 fwd = dir / len;
    glm::vec2 perp{-fwd.y, fwd.x};  // perpendicular for collision radius

    f32 step = tile_size * 0.4f;
    u32 steps = static_cast<u32>(len / step) + 1;

    for (u32 i = 0; i <= steps; ++i) {
        f32 t = static_cast<f32>(i) / static_cast<f32>(steps);
        glm::vec2 pt = a + dir * t;

        // Center must be on a passable tile at the correct cliff level
        glm::ivec2 tile = pf.world_to_tile(pt.x, pt.y);
        if (!pf.can_occupy(tile.x, tile.y, move_type)) return false;
        i32 eff = pf.terrain()->tile_effective_level(tile.x, tile.y);
        if (eff >= 0 && static_cast<u8>(eff) != cliff_level) return false;

        // Collision radius: check perpendicular offsets
        if (collision_radius > 0) {
            glm::vec2 offsets[] = {
                pt + perp * collision_radius,
                pt - perp * collision_radius,
            };
            for (auto& off : offsets) {
                glm::ivec2 ot = pf.world_to_tile(off.x, off.y);
                if (ot == tile) continue;  // same tile, already checked
                if (!pf.can_occupy(ot.x, ot.y, move_type)) return false;
            }
        }
    }
    return true;
}

glm::vec2 Pathfinder::find_straight_waypoint(glm::vec2 from, std::span<const glm::ivec2> corridor_tiles,
                                              f32 collision_radius, u8 cliff_level,
                                              MoveType move_type) const {
    if (!m_terrain || corridor_tiles.empty()) return from;
    f32 ts = m_terrain->tile_size;

    // Find which corridor tile we're on (or nearest)
    u32 start_idx = 0;
    {
        glm::ivec2 cur = world_to_tile(from.x, from.y);
        for (u32 i = 0; i < corridor_tiles.size(); ++i) {
            if (corridor_tiles[i] == cur) { start_idx = i; break; }
        }
    }

    // Walk forward through corridor with exponential jumps to find the farthest
    // reachable tile, then refine.
    u32 end = static_cast<u32>(corridor_tiles.size());

    // Phase 1: exponential jumps (1, 2, 4, 8...) to find the rough limit
    u32 last_good = start_idx;
    u32 first_bad = end;
    for (u32 jump = 1; ; jump *= 2) {
        u32 test_idx = start_idx + jump;
        if (test_idx >= end) test_idx = end - 1;

        glm::vec2 target = tile_center(corridor_tiles[test_idx].x, corridor_tiles[test_idx].y);
        if (line_passable(from, target, ts, collision_radius, cliff_level, *this, move_type)) {
            last_good = test_idx;
            if (test_idx >= end - 1) break;  // reached the end
        } else {
            first_bad = test_idx;
            break;
        }
    }

    // Phase 2: binary search between last_good and first_bad
    while (last_good + 1 < first_bad) {
        u32 mid = (last_good + first_bad) / 2;
        glm::vec2 target = tile_center(corridor_tiles[mid].x, corridor_tiles[mid].y);
        if (line_passable(from, target, ts, collision_radius, cliff_level, *this, move_type)) {
            last_good = mid;
        } else {
            first_bad = mid;
        }
    }

    glm::vec2 best = tile_center(corridor_tiles[last_good].x, corridor_tiles[last_good].y);

    // If the best is still the start tile, try the next tile as minimum progress
    if (last_good == start_idx && start_idx + 1 < end) {
        best = tile_center(corridor_tiles[start_idx + 1].x, corridor_tiles[start_idx + 1].y);
    }

    return best;
}

// ── Recovery ─────────────────────────────────────────────────────────────

glm::vec2 Pathfinder::find_nearest_valid(f32 x, f32 y, MoveType move_type) const {
    if (!m_terrain || !m_terrain->is_valid()) return {x, y};
    auto& td = *m_terrain;

    glm::ivec2 t = world_to_tile(x, y);

    // Already valid?
    if (can_occupy(t.x, t.y, move_type)) return {x, y};

    // Search expanding rings
    for (i32 r = 1; r <= 20; ++r) {
        f32 best_dist = 1e9f;
        glm::vec2 best{x, y};
        bool found = false;

        for (i32 dy = -r; dy <= r; ++dy) {
            for (i32 dx = -r; dx <= r; ++dx) {
                if (std::abs(dx) != r && std::abs(dy) != r) continue;
                i32 nx = t.x + dx, ny = t.y + dy;
                if (nx < 0 || ny < 0 ||
                    static_cast<u32>(nx) >= td.tiles_x ||
                    static_cast<u32>(ny) >= td.tiles_y) continue;
                if (!can_occupy(nx, ny, move_type)) continue;

                glm::vec2 center = tile_center(nx, ny);
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
    return {x, y};  // give up
}

// ── Runtime pathing blocks ───────────────────────────────────────────────

void Pathfinder::block_vertices(const std::vector<glm::ivec2>& verts) {
    if (!m_terrain) return;
    if (m_runtime_blocked.empty())
        m_runtime_blocked.resize(m_terrain->vertex_count(), 0);

    for (auto& v : verts) {
        if (v.x >= 0 && v.y >= 0 &&
            static_cast<u32>(v.x) < m_terrain->verts_x() &&
            static_cast<u32>(v.y) < m_terrain->verts_y()) {
            u32 idx = v.y * m_terrain->verts_x() + v.x;
            if (m_runtime_blocked[idx] < 255) m_runtime_blocked[idx]++;
        }
    }
}

void Pathfinder::unblock_vertices(const std::vector<glm::ivec2>& verts) {
    if (m_runtime_blocked.empty()) return;

    for (auto& v : verts) {
        if (v.x >= 0 && v.y >= 0 &&
            static_cast<u32>(v.x) < m_terrain->verts_x() &&
            static_cast<u32>(v.y) < m_terrain->verts_y()) {
            u32 idx = v.y * m_terrain->verts_x() + v.x;
            if (m_runtime_blocked[idx] > 0) m_runtime_blocked[idx]--;
        }
    }
}

bool Pathfinder::is_vertex_blocked(u32 vx, u32 vy) const {
    if (m_runtime_blocked.empty() || !m_terrain) return false;
    if (vx >= m_terrain->verts_x() || vy >= m_terrain->verts_y()) return false;
    return m_runtime_blocked[vy * m_terrain->verts_x() + vx] > 0;
}

} // namespace uldum::simulation
