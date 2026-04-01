#include "simulation/pathfinding.h"
#include "simulation/handle_types.h"
#include "map/terrain_data.h"
#include "core/log.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_set>
#include <vector>

namespace uldum::simulation {

// ── Tile passability ──────────────────────────────────────────────────────

bool Pathfinder::is_tile_passable(u32 tx, u32 ty, MoveType move_type) const {
    if (!m_terrain || tx >= m_terrain->tiles_x || ty >= m_terrain->tiles_y) return false;
    u8 flags = m_terrain->pathing_at(tx, ty);
    switch (move_type) {
        case MoveType::Ground:     return (flags & map::PATHING_WALKABLE) != 0;
        case MoveType::Air:        return (flags & map::PATHING_FLYABLE) != 0;
        case MoveType::Amphibious: return (flags & (map::PATHING_WALKABLE | map::PATHING_FLYABLE)) != 0;
    }
    return false;
}

// ── Terrain sampling ──────────────────────────────────────────────────────

f32 Pathfinder::sample_height(f32 x, f32 y) const {
    if (!m_terrain || !m_terrain->is_valid()) return 0.0f;
    auto& td = *m_terrain;

    f32 fx = x / td.tile_size;
    f32 fy = y / td.tile_size;
    u32 ix = static_cast<u32>(std::floor(fx));
    u32 iy = static_cast<u32>(std::floor(fy));
    ix = std::min(ix, td.tiles_x - 1);
    iy = std::min(iy, td.tiles_y - 1);

    f32 tx = fx - static_cast<f32>(ix);
    f32 ty = fy - static_cast<f32>(iy);

    // Bilinear interpolation
    f32 h00 = td.height_at(ix, iy);
    f32 h10 = td.height_at(ix + 1, iy);
    f32 h01 = td.height_at(ix, iy + 1);
    f32 h11 = td.height_at(ix + 1, iy + 1);

    f32 h0 = h00 + tx * (h10 - h00);
    f32 h1 = h01 + tx * (h11 - h01);
    return h0 + ty * (h1 - h0);
}

glm::vec3 Pathfinder::sample_normal(f32 x, f32 y) const {
    if (!m_terrain || !m_terrain->is_valid()) return {0, 0, 1};
    auto& td = *m_terrain;

    // Central differences
    f32 eps = td.tile_size * 0.5f;
    f32 hL = sample_height(x - eps, y);
    f32 hR = sample_height(x + eps, y);
    f32 hD = sample_height(x, y - eps);
    f32 hU = sample_height(x, y + eps);

    glm::vec3 dx{2.0f * eps, 0.0f, hR - hL};
    glm::vec3 dy{0.0f, 2.0f * eps, hU - hD};
    return glm::normalize(glm::cross(dx, dy));
}

// ── A* pathfinding ────────────────────────────────────────────────────────

struct AStarNode {
    u32 tx, ty;
    f32 g_cost;     // cost from start
    f32 f_cost;     // g + heuristic
    u32 parent_idx; // index into closed list
};

struct NodeCompare {
    bool operator()(const AStarNode& a, const AStarNode& b) const {
        return a.f_cost > b.f_cost;
    }
};

static u64 tile_key(u32 tx, u32 ty) {
    return (static_cast<u64>(ty) << 32) | tx;
}

Path Pathfinder::find_path(glm::vec3 start, glm::vec3 goal, MoveType move_type) const {
    Path result;
    if (!m_terrain || !m_terrain->is_valid()) return result;
    auto& td = *m_terrain;

    // Convert world positions to tile coordinates
    u32 sx = static_cast<u32>(std::clamp(start.x / td.tile_size, 0.0f, static_cast<f32>(td.tiles_x - 1)));
    u32 sy = static_cast<u32>(std::clamp(start.y / td.tile_size, 0.0f, static_cast<f32>(td.tiles_y - 1)));
    u32 gx = static_cast<u32>(std::clamp(goal.x / td.tile_size, 0.0f, static_cast<f32>(td.tiles_x - 1)));
    u32 gy = static_cast<u32>(std::clamp(goal.y / td.tile_size, 0.0f, static_cast<f32>(td.tiles_y - 1)));

    if (!is_tile_passable(gx, gy, move_type)) {
        // Goal is blocked — find nearest passable tile (simple fallback)
        return result;
    }

    // A* with 8-directional movement
    std::priority_queue<AStarNode, std::vector<AStarNode>, NodeCompare> open;
    std::unordered_set<u64> closed;
    std::vector<AStarNode> all_nodes;
    all_nodes.reserve(256);

    auto heuristic = [gx, gy](u32 tx, u32 ty) -> f32 {
        f32 dx = static_cast<f32>(tx) - static_cast<f32>(gx);
        f32 dy = static_cast<f32>(ty) - static_cast<f32>(gy);
        return std::sqrt(dx * dx + dy * dy);
    };

    AStarNode start_node{sx, sy, 0, heuristic(sx, sy), UINT32_MAX};
    open.push(start_node);

    static constexpr i32 dx[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static constexpr i32 dy[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    static constexpr f32 cost[] = {1.414f, 1.0f, 1.414f, 1.0f, 1.0f, 1.414f, 1.0f, 1.414f};

    u32 max_iterations = td.tiles_x * td.tiles_y;  // safety limit
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
        if (current.tx == gx && current.ty == gy) {
            // Reconstruct path
            std::vector<glm::vec3> waypoints;
            u32 idx = current_idx;
            while (idx != UINT32_MAX) {
                auto& n = all_nodes[idx];
                f32 wx = (static_cast<f32>(n.tx) + 0.5f) * td.tile_size;
                f32 wy = (static_cast<f32>(n.ty) + 0.5f) * td.tile_size;
                waypoints.push_back({wx, wy, 0.0f});
                idx = n.parent_idx;
            }
            std::reverse(waypoints.begin(), waypoints.end());

            // Replace first waypoint with exact start, last with exact goal
            if (!waypoints.empty()) {
                waypoints.front() = {start.x, start.y, 0.0f};
                waypoints.back() = {goal.x, goal.y, 0.0f};
            }

            result.waypoints = std::move(waypoints);
            result.valid = true;
            return result;
        }

        // Expand neighbors
        for (u32 i = 0; i < 8; ++i) {
            i32 nx = static_cast<i32>(current.tx) + dx[i];
            i32 ny = static_cast<i32>(current.ty) + dy[i];

            if (nx < 0 || ny < 0 || static_cast<u32>(nx) >= td.tiles_x || static_cast<u32>(ny) >= td.tiles_y)
                continue;

            u32 ntx = static_cast<u32>(nx);
            u32 nty = static_cast<u32>(ny);

            if (closed.contains(tile_key(ntx, nty))) continue;
            if (!is_tile_passable(ntx, nty, move_type)) continue;

            // For diagonal movement, check that both cardinal neighbors are passable
            if (dx[i] != 0 && dy[i] != 0) {
                if (!is_tile_passable(current.tx + dx[i], current.ty, move_type) ||
                    !is_tile_passable(current.tx, current.ty + dy[i], move_type))
                    continue;
            }

            f32 new_g = current.g_cost + cost[i];
            f32 new_f = new_g + heuristic(ntx, nty);
            open.push({ntx, nty, new_g, new_f, current_idx});
        }
    }

    return result;  // no path found
}

} // namespace uldum::simulation
