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
class Pathfinder {
public:
    void set_terrain(const map::TerrainData* terrain) { m_terrain = terrain; }

    // Find a path from start to goal. Returns empty path if unreachable.
    // move_type determines which tiles are traversable (ground checks walkable, air checks flyable).
    Path find_path(glm::vec3 start, glm::vec3 goal, MoveType move_type) const;

    // Check if a tile is walkable for a given move type.
    bool is_tile_passable(u32 tx, u32 ty, MoveType move_type) const;

    // Sample terrain height at world position (bilinear interpolation).
    f32 sample_height(f32 x, f32 y) const;

    // Sample terrain normal at world position.
    glm::vec3 sample_normal(f32 x, f32 y) const;

private:
    const map::TerrainData* m_terrain = nullptr;
};

} // namespace uldum::simulation
