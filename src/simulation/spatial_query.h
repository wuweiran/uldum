#pragma once

#include "simulation/handle_types.h"
#include "core/types.h"

#include <glm/vec3.hpp>

#include <functional>
#include <string>
#include <vector>

namespace uldum::simulation {

struct World;
class Simulation;

// Filter for spatial queries. All conditions are AND-ed.
// Empty/default fields are ignored (match everything).
struct UnitFilter {
    Player      owner;                          // only units owned by this player (invalid = any)
    Player      enemy_of;                       // only units NOT allied with this player (invalid = any)
    std::vector<std::string> classifications;   // unit must have ALL of these flags
    bool        alive_only = true;              // only living units
    bool        exclude_buildings = false;       // skip units with "structure" classification

    // Custom predicate (for C++ callers; Lua uses the fields above)
    std::function<bool(Unit)> predicate;
};

// Uniform grid for fast spatial lookups.
class SpatialGrid {
public:
    void init(f32 world_width, f32 world_height, f32 cell_size, const Simulation* sim = nullptr);

    // Rebuild grid from current world state. Call once per tick.
    void update(const World& world);

    // Query: find all units within radius of position, matching filter.
    std::vector<Unit> units_in_range(const World& world, glm::vec3 center, f32 radius, const UnitFilter& filter = {}) const;

    // Query: find all units within an axis-aligned rectangle, matching filter.
    std::vector<Unit> units_in_rect(const World& world, f32 x, f32 y, f32 width, f32 height, const UnitFilter& filter = {}) const;

    // Query: find the nearest unit matching filter within max_radius. Returns invalid handle if none.
    Unit nearest_unit(const World& world, glm::vec3 center, f32 max_radius, const UnitFilter& filter = {}) const;

private:
    bool passes_filter(const World& world, Unit unit, const UnitFilter& filter) const;
    void get_cell_range(f32 x, f32 y, f32 radius, i32& min_cx, i32& min_cy, i32& max_cx, i32& max_cy) const;

    struct Cell {
        std::vector<Unit> units;
    };

    std::vector<Cell> m_cells;
    f32 m_cell_size   = 8.0f;
    u32 m_cells_x     = 0;
    u32 m_cells_y     = 0;
    f32 m_world_width = 0;
    f32 m_world_height = 0;
    const Simulation* m_sim = nullptr;
};

} // namespace uldum::simulation
