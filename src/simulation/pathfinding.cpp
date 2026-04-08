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

// ── Vertex passability ───────────────────────────────────────────────────

bool Pathfinder::is_passable(u32 vx, u32 vy, MoveType move_type) const {
    if (!m_terrain || vx >= m_terrain->verts_x() || vy >= m_terrain->verts_y()) return false;
    u8 flags = m_terrain->pathing_at(vx, vy);
    bool is_water = m_terrain->is_water(vx, vy);
    switch (move_type) {
        case MoveType::Ground:     return (flags & map::PATHING_WALKABLE) && !is_water;
        case MoveType::Air:        return (flags & map::PATHING_FLYABLE) != 0;
        case MoveType::Amphibious: return (flags & map::PATHING_WALKABLE) != 0;  // can walk on water
    }
    return false;
}

// ── Terrain sampling ─────────────────────────────────────────────────────

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

    // Bilinear interpolation using world_z_at (cliff + heightmap)
    f32 h00 = td.world_z_at(ix, iy);
    f32 h10 = td.world_z_at(ix + 1, iy);
    f32 h01 = td.world_z_at(ix, iy + 1);
    f32 h11 = td.world_z_at(ix + 1, iy + 1);

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

f32 Pathfinder::tile_size() const {
    return (m_terrain && m_terrain->is_valid()) ? m_terrain->tile_size : 128.0f;
}

i32 Pathfinder::tile_effective_level(u32 tx, u32 ty) const {
    auto& td = *m_terrain;
    if (tx >= td.tiles_x || ty >= td.tiles_y) return 0;

    u8 c[4] = { td.cliff_at(tx,ty), td.cliff_at(tx+1,ty),
                 td.cliff_at(tx,ty+1), td.cliff_at(tx+1,ty+1) };
    u8 cmin = std::min({c[0], c[1], c[2], c[3]});
    u8 cmax = std::max({c[0], c[1], c[2], c[3]});

    if (cmin == cmax) return static_cast<i32>(cmin);  // flat tile

    // Count ramp flags
    u32 ramp_count = 0;
    if (td.pathing_at(tx, ty) & map::PATHING_RAMP) ramp_count++;
    if (td.pathing_at(tx+1, ty) & map::PATHING_RAMP) ramp_count++;
    if (td.pathing_at(tx, ty+1) & map::PATHING_RAMP) ramp_count++;
    if (td.pathing_at(tx+1, ty+1) & map::PATHING_RAMP) ramp_count++;

    if (ramp_count == 4 && cmax - cmin == 1) return -1;  // ramp: connects both levels

    // Cliff tile (mixed levels, not a ramp): effective level = min (low side)
    return static_cast<i32>(cmin);
}

bool Pathfinder::can_move_to(f32 old_x, f32 old_y, f32 new_x, f32 new_y, u8 cliff_level, MoveType move_type) const {
    if (!m_terrain || !m_terrain->is_valid()) return true;
    if (move_type == MoveType::Air) return true;
    auto& td = *m_terrain;

    u32 old_tx = std::min(static_cast<u32>(old_x / td.tile_size), td.tiles_x - 1);
    u32 old_ty = std::min(static_cast<u32>(old_y / td.tile_size), td.tiles_y - 1);
    u32 new_tx = std::min(static_cast<u32>(new_x / td.tile_size), td.tiles_x - 1);
    u32 new_ty = std::min(static_cast<u32>(new_y / td.tile_size), td.tiles_y - 1);

    if (old_tx == new_tx && old_ty == new_ty) return true;  // same tile

    i32 src_level = tile_effective_level(old_tx, old_ty);
    i32 dest_level = tile_effective_level(new_tx, new_ty);

    // Either tile is a ramp: allow transition (ramp connects both levels)
    if (src_level == -1 || dest_level == -1) return true;

    // Both are flat/cliff tiles: must be same effective level
    return src_level == dest_level;
}

bool Pathfinder::is_ramp_at(f32 x, f32 y) const {
    if (!m_terrain || !m_terrain->is_valid()) return false;
    auto& td = *m_terrain;
    u32 tx = std::min(static_cast<u32>(x / td.tile_size), td.tiles_x - 1);
    u32 ty = std::min(static_cast<u32>(y / td.tile_size), td.tiles_y - 1);
    return tile_effective_level(tx, ty) == -1;
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

// ── A* pathfinding ───────────────────────────────────────────────────────

struct AStarNode {
    u32 vx, vy;    // vertex coordinates
    f32 g_cost;     // cost from start
    f32 f_cost;     // g + heuristic
    u32 parent_idx; // index into closed list
    u8  cliff_level = 0; // cliff level at this node
};

struct NodeCompare {
    bool operator()(const AStarNode& a, const AStarNode& b) const {
        return a.f_cost > b.f_cost;
    }
};

static u64 vert_key(u32 vx, u32 vy) {
    return (static_cast<u64>(vy) << 32) | vx;
}

Path Pathfinder::find_path(glm::vec3 start, glm::vec3 goal, MoveType move_type) const {
    Path result;
    if (!m_terrain || !m_terrain->is_valid()) return result;
    auto& td = *m_terrain;

    // Convert world positions to vertex coordinates
    u32 max_vx = td.verts_x() - 1;
    u32 max_vy = td.verts_y() - 1;
    u32 sx = static_cast<u32>(std::clamp(start.x / td.tile_size, 0.0f, static_cast<f32>(max_vx)));
    u32 sy = static_cast<u32>(std::clamp(start.y / td.tile_size, 0.0f, static_cast<f32>(max_vy)));
    u32 gx = static_cast<u32>(std::clamp(goal.x / td.tile_size, 0.0f, static_cast<f32>(max_vx)));
    u32 gy = static_cast<u32>(std::clamp(goal.y / td.tile_size, 0.0f, static_cast<f32>(max_vy)));

    if (!is_passable(gx, gy, move_type)) {
        return result;
    }

    // A* with 8-directional movement on vertex grid
    std::priority_queue<AStarNode, std::vector<AStarNode>, NodeCompare> open;
    std::unordered_set<u64> closed;
    std::vector<AStarNode> all_nodes;
    all_nodes.reserve(256);

    auto heuristic = [gx, gy](u32 vx, u32 vy) -> f32 {
        f32 dx = static_cast<f32>(vx) - static_cast<f32>(gx);
        f32 dy = static_cast<f32>(vy) - static_cast<f32>(gy);
        return std::sqrt(dx * dx + dy * dy);
    };

    AStarNode start_node{sx, sy, 0, heuristic(sx, sy), UINT32_MAX, td.cliff_at(sx, sy)};
    open.push(start_node);

    static constexpr i32 dx[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static constexpr i32 dy[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    static constexpr f32 cost[] = {1.414f, 1.0f, 1.414f, 1.0f, 1.0f, 1.414f, 1.0f, 1.414f};

    u32 max_iterations = td.verts_x() * td.verts_y();  // safety limit
    u32 iterations = 0;

    while (!open.empty() && iterations++ < max_iterations) {
        AStarNode current = open.top();
        open.pop();

        u64 key = vert_key(current.vx, current.vy);
        if (closed.contains(key)) continue;
        closed.insert(key);

        u32 current_idx = static_cast<u32>(all_nodes.size());
        all_nodes.push_back(current);

        // Goal reached
        if (current.vx == gx && current.vy == gy) {
            std::vector<glm::vec3> waypoints;
            u32 idx = current_idx;
            while (idx != UINT32_MAX) {
                auto& n = all_nodes[idx];
                f32 wx = static_cast<f32>(n.vx) * td.tile_size;
                f32 wy = static_cast<f32>(n.vy) * td.tile_size;
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
            i32 nx = static_cast<i32>(current.vx) + dx[i];
            i32 ny = static_cast<i32>(current.vy) + dy[i];

            if (nx < 0 || ny < 0 || static_cast<u32>(nx) > max_vx || static_cast<u32>(ny) > max_vy)
                continue;

            u32 nvx = static_cast<u32>(nx);
            u32 nvy = static_cast<u32>(ny);

            if (closed.contains(vert_key(nvx, nvy))) continue;
            if (!is_passable(nvx, nvy, move_type)) continue;

            // Tile-based cliff check
            f32 world_cur_x = static_cast<f32>(current.vx) * td.tile_size;
            f32 world_cur_y = static_cast<f32>(current.vy) * td.tile_size;
            {
                f32 world_nx = static_cast<f32>(nvx) * td.tile_size;
                f32 world_ny = static_cast<f32>(nvy) * td.tile_size;
                if (!can_move_to(world_cur_x, world_cur_y, world_nx, world_ny, current.cliff_level, move_type)) continue;
            }

            // For diagonal movement, check that both cardinal neighbors are passable
            if (dx[i] != 0 && dy[i] != 0) {
                u32 cx = static_cast<u32>(static_cast<i32>(current.vx) + dx[i]);
                u32 cy = current.vy;
                u32 rx = current.vx;
                u32 ry = static_cast<u32>(static_cast<i32>(current.vy) + dy[i]);
                if (!is_passable(cx, cy, move_type) || !is_passable(rx, ry, move_type))
                    continue;
                // Both cardinal steps must also be tile-passable
                f32 wcx = static_cast<f32>(cx) * td.tile_size;
                f32 wcy = static_cast<f32>(cy) * td.tile_size;
                f32 wrx = static_cast<f32>(rx) * td.tile_size;
                f32 wry = static_cast<f32>(ry) * td.tile_size;
                if (!can_move_to(world_cur_x, world_cur_y, wcx, wcy, current.cliff_level, move_type) ||
                    !can_move_to(world_cur_x, world_cur_y, wrx, wry, current.cliff_level, move_type))
                    continue;
            }

            // Update cliff level: on ramp tiles, adopt the vertex's cliff level
            // On flat/cliff tiles, keep current level (can_move_to already validated)
            f32 world_x = static_cast<f32>(nvx) * td.tile_size;
            f32 world_y = static_cast<f32>(nvy) * td.tile_size;
            u32 dtx = std::min(static_cast<u32>(world_x / td.tile_size), td.tiles_x - 1);
            u32 dty = std::min(static_cast<u32>(world_y / td.tile_size), td.tiles_y - 1);
            i32 dest_eff = tile_effective_level(dtx, dty);
            u8 node_cliff = (dest_eff == -1) ? td.cliff_at(nvx, nvy) : current.cliff_level;

            f32 new_g = current.g_cost + cost[i];
            f32 new_f = new_g + heuristic(nvx, nvy);
            open.push({nvx, nvy, new_g, new_f, current_idx, node_cliff});
        }
    }

    return result;  // no path found
}

} // namespace uldum::simulation
