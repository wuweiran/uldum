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

i32 Pathfinder::tile_effective_level(u32 tx, u32 ty) const {
    auto& td = *m_terrain;
    if (tx >= td.tiles_x || ty >= td.tiles_y) return -2;

    u8 c[4] = { td.cliff_at(tx, ty),     td.cliff_at(tx+1, ty),
                 td.cliff_at(tx, ty+1),   td.cliff_at(tx+1, ty+1) };
    u8 cmin = std::min({c[0], c[1], c[2], c[3]});
    u8 cmax = std::max({c[0], c[1], c[2], c[3]});

    // Flat tile: all corners same level
    if (cmin == cmax) return static_cast<i32>(cmin);

    // Ramp: all 4 corners have RAMP flag, exactly 1 level difference
    if (cmax - cmin == 1) {
        bool all_ramp = (td.pathing_at(tx, ty)     & map::PATHING_RAMP) &&
                        (td.pathing_at(tx+1, ty)   & map::PATHING_RAMP) &&
                        (td.pathing_at(tx, ty+1)   & map::PATHING_RAMP) &&
                        (td.pathing_at(tx+1, ty+1) & map::PATHING_RAMP);
        if (all_ramp) return -1;  // ramp: connects both levels
    }

    // Cliff wall: mixed levels, not a valid ramp — impassable
    return -2;
}

bool Pathfinder::can_occupy(u32 tx, u32 ty, MoveType move_type) const {
    if (!m_terrain || tx >= m_terrain->tiles_x || ty >= m_terrain->tiles_y) return false;
    if (move_type == MoveType::Air) return true;

    i32 eff = tile_effective_level(tx, ty);
    if (eff == -2) return false;  // cliff wall

    auto& td = *m_terrain;
    // Check that at least one corner is walkable and not water
    for (u32 vy = ty; vy <= ty + 1; ++vy) {
        for (u32 vx = tx; vx <= tx + 1; ++vx) {
            u8 flags = td.pathing_at(vx, vy);
            bool walkable = (flags & map::PATHING_WALKABLE) != 0;
            bool water = td.is_water(vx, vy);
            if (move_type == MoveType::Ground && walkable && !water) return true;
            if (move_type == MoveType::Amphibious && walkable) return true;
        }
    }
    return false;
}

bool Pathfinder::are_connected(u32 src_tx, u32 src_ty, u32 dst_tx, u32 dst_ty,
                                u8 cliff_level, MoveType move_type) const {
    if (!m_terrain) return false;
    if (move_type == MoveType::Air) return true;

    // Both tiles must be occupiable
    if (!can_occupy(src_tx, src_ty, move_type)) return false;
    if (!can_occupy(dst_tx, dst_ty, move_type)) return false;

    // Check cliff level compatibility
    i32 src_eff = tile_effective_level(src_tx, src_ty);
    i32 dst_eff = tile_effective_level(dst_tx, dst_ty);

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

    i32 eff = tile_effective_level(tx, ty);
    if (eff >= 0) return static_cast<u8>(eff);

    // Ramp: use min corner level (entering from low side) or max (from high side).
    // Use the average — the movement system will update cliff_level based on nearest vertex.
    u8 c[4] = { td.cliff_at(tx, ty),     td.cliff_at(tx+1, ty),
                 td.cliff_at(tx, ty+1),   td.cliff_at(tx+1, ty+1) };
    return std::min({c[0], c[1], c[2], c[3]});
}

// ── World coordinate helpers ─────────────────────────────────────────────

glm::ivec2 Pathfinder::world_to_tile(f32 x, f32 y) const {
    if (!m_terrain || !m_terrain->is_valid()) return {0, 0};
    auto& td = *m_terrain;
    i32 tx = static_cast<i32>(x / td.tile_size);
    i32 ty = static_cast<i32>(y / td.tile_size);
    tx = std::clamp(tx, 0, static_cast<i32>(td.tiles_x - 1));
    ty = std::clamp(ty, 0, static_cast<i32>(td.tiles_y - 1));
    return {tx, ty};
}

glm::vec2 Pathfinder::tile_center(u32 tx, u32 ty) const {
    f32 ts = m_terrain ? m_terrain->tile_size : 128.0f;
    return {(static_cast<f32>(tx) + 0.5f) * ts,
            (static_cast<f32>(ty) + 0.5f) * ts};
}

u8 Pathfinder::cliff_level_at(f32 x, f32 y) const {
    if (!m_terrain || !m_terrain->is_valid()) return 0;
    auto& td = *m_terrain;
    u32 vx = static_cast<u32>(std::round(x / td.tile_size));
    u32 vy = static_cast<u32>(std::round(y / td.tile_size));
    vx = std::min(vx, td.tiles_x);
    vy = std::min(vy, td.tiles_y);
    return td.cliff_at(vx, vy);
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

    while (!open.empty() && iterations++ < max_iterations) {
        AStarNode current = open.top();
        open.pop();

        u64 key = tile_key(current.tx, current.ty);
        if (closed.contains(key)) continue;
        closed.insert(key);

        u32 current_idx = static_cast<u32>(all_nodes.size());
        all_nodes.push_back(current);

        // Goal reached
        if (current.tx == static_cast<u32>(g.x) && current.ty == static_cast<u32>(g.y)) {
            // Reconstruct corridor
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

            // Update cliff level for the new tile
            u8 new_cliff = cliff_level_on_tile(ntx, nty);
            i32 eff = tile_effective_level(ntx, nty);
            if (eff == -1) {
                // On ramp: keep current level unless we've fully transitioned
                // Use the vertex-based cliff to determine which side we're on
                new_cliff = current.cliff_level;
                // If the ramp connects current level to another, and we're going
                // toward the other side, update
                u8 ramp_min = cliff_level_on_tile(ntx, nty);
                u8 ramp_max = ramp_min + 1;
                if (current.cliff_level == ramp_min) new_cliff = ramp_max;
                else if (current.cliff_level == ramp_max) new_cliff = ramp_min;
                // If coming from another ramp, keep current level
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

    return result;  // no path found
}

// ── Straight-line waypoint through corridor ──────────────────────────────

// Check if a world-space point is inside any tile of the corridor.
static bool point_in_corridor(const Corridor& corridor, glm::vec2 pt, f32 tile_size) {
    i32 tx = static_cast<i32>(std::floor(pt.x / tile_size));
    i32 ty = static_cast<i32>(std::floor(pt.y / tile_size));
    for (auto& t : corridor.tiles) {
        if (t.x == tx && t.y == ty) return true;
    }
    return false;
}

// Check if a line segment from A to B stays within the corridor, accounting for
// collision radius. Center must stay in the corridor. Offset points (collision
// radius) must be on occupiable terrain (not necessarily in the corridor — open
// adjacent tiles are fine, but cliffs/walls are not).
static bool line_in_corridor(const Corridor& corridor, glm::vec2 a, glm::vec2 b,
                              f32 tile_size, f32 collision_radius,
                              const Pathfinder& pf, MoveType move_type) {
    glm::vec2 dir = b - a;
    f32 len = glm::length(dir);
    if (len < 0.001f) return true;

    f32 step = tile_size * 0.4f;
    u32 steps = static_cast<u32>(len / step) + 1;

    for (u32 i = 0; i <= steps; ++i) {
        f32 t = static_cast<f32>(i) / static_cast<f32>(steps);
        glm::vec2 pt = a + dir * t;

        // Center must be in the corridor
        if (!point_in_corridor(corridor, pt, tile_size)) return false;

        // TODO: collision radius check disabled for debugging — re-enable after fixing
        // if (collision_radius > 0) {
        //     glm::vec2 offsets[] = {
        //         {collision_radius, 0}, {-collision_radius, 0},
        //         {0, collision_radius}, {0, -collision_radius}
        //     };
        //     for (auto& off : offsets) {
        //         glm::ivec2 t2 = pf.world_to_tile(pt.x + off.x, pt.y + off.y);
        //         if (!pf.can_occupy(t2.x, t2.y, move_type)) return false;
        //     }
        // }
    }
    return true;
}

glm::vec2 Pathfinder::find_straight_waypoint(glm::vec2 from, const Corridor& corridor,
                                              f32 collision_radius, MoveType move_type) const {
    if (!m_terrain || corridor.tiles.empty()) return from;
    f32 ts = m_terrain->tile_size;

    // Find which corridor tile we're on (or nearest)
    u32 start_idx = 0;
    {
        glm::ivec2 cur = world_to_tile(from.x, from.y);
        for (u32 i = 0; i < corridor.tiles.size(); ++i) {
            if (corridor.tiles[i] == cur) { start_idx = i; break; }
        }
    }

    // Walk forward through corridor with exponential jumps to find the farthest
    // reachable tile, then refine.
    glm::vec2 best = tile_center(corridor.tiles[start_idx].x, corridor.tiles[start_idx].y);
    u32 best_idx = start_idx;
    u32 end = static_cast<u32>(corridor.tiles.size());

    // Phase 1: exponential jumps (1, 2, 4, 8...) to find the rough limit
    u32 last_good = start_idx;
    u32 first_bad = end;
    for (u32 jump = 1; ; jump *= 2) {
        u32 test_idx = start_idx + jump;
        if (test_idx >= end) test_idx = end - 1;

        glm::vec2 target = tile_center(corridor.tiles[test_idx].x, corridor.tiles[test_idx].y);
        if (line_in_corridor(corridor, from, target, ts, collision_radius, *this, move_type)) {
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
        glm::vec2 target = tile_center(corridor.tiles[mid].x, corridor.tiles[mid].y);
        if (line_in_corridor(corridor, from, target, ts, collision_radius, *this, move_type)) {
            last_good = mid;
        } else {
            first_bad = mid;
        }
    }

    best_idx = last_good;
    best = tile_center(corridor.tiles[best_idx].x, corridor.tiles[best_idx].y);

    // If the best is still the start tile, try the next tile as minimum progress
    if (best_idx == start_idx && start_idx + 1 < end) {
        best = tile_center(corridor.tiles[start_idx + 1].x, corridor.tiles[start_idx + 1].y);
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

} // namespace uldum::simulation
