#pragma once

#include "core/types.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <vector>

namespace uldum::map { struct TerrainData; }

namespace uldum::simulation {

enum class MoveType : u8;

// A path: sequence of world-space waypoints from start to goal.
struct Path {
    std::vector<glm::vec3> waypoints;  // world-space positions (Z = 0, caller sets Z from terrain)
    bool valid = false;
};

// Stateless pathfinding service. Queries terrain pathing data.
// Pathing is per-vertex: each vertex at (ix, iy) acts as a pathing cell.
class Pathfinder {
public:
    void set_terrain(const map::TerrainData* terrain) { m_terrain = terrain; }

    // Find a path from start to goal. Returns empty path if unreachable.
    // move_type determines which vertices are traversable.
    Path find_path(glm::vec3 start, glm::vec3 goal, MoveType move_type) const;

    // Check if a vertex is passable for a given move type.
    bool is_passable(u32 vx, u32 vy, MoveType move_type) const;

    // Sample terrain height at world position (bilinear interpolation).
    // Returns final height: cliff_level * layer_height + heightmap.
    f32 sample_height(f32 x, f32 y) const;

    // Sample terrain normal at world position.
    glm::vec3 sample_normal(f32 x, f32 y) const;

    // Get tile size from terrain data (for world→grid conversion).
    f32 tile_size() const;

    // Check if a unit at (old_x, old_y) with cliff_level can move to (new_x, new_y).
    bool can_move_to(f32 old_x, f32 old_y, f32 new_x, f32 new_y, u8 cliff_level, MoveType move_type) const;

    // Get the cliff level at a world position (nearest vertex).
    u8 cliff_level_at(f32 x, f32 y) const;

    // Check if a world position is on a ramp tile.
    bool is_ramp_at(f32 x, f32 y) const;

private:
    // Get the effective cliff level of a tile. Ramp tiles connect both levels.
    // Returns -1 for ramp tiles (passable from either level).
    i32 tile_effective_level(u32 tx, u32 ty) const;
    const map::TerrainData* m_terrain = nullptr;
};

} // namespace uldum::simulation
