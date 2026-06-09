#pragma once

namespace uldum::map { struct TerrainData; }

namespace uldum::simulation {

struct World;
class Pathfinder;
class SpatialGrid;

// Each system processes one simulation tick (fixed timestep).
void system_health(World& world, float dt);
void system_state(World& world, float dt);
void system_movement(World& world, float dt, const Pathfinder& pathfinder, const SpatialGrid& grid, const map::TerrainData* terrain);
void system_combat(World& world, float dt, const SpatialGrid& grid);
// Walks every authored region, computes which alive units sit inside,
// and fires enter / leave callbacks for the diff vs last tick. Cheap
// O(R*N) — fine for the small region counts maps tend to author; can
// switch to a spatial-grid query later if needed.
void system_regions(World& world);
void system_ability(World& world, float dt, const class AbilityRegistry& abilities, const SpatialGrid& grid);
void system_items(World& world, float dt);
void system_projectile(World& world, float dt);
void system_collision(World& world, const SpatialGrid& grid, const Pathfinder& pathfinder);
void system_death(World& world, float dt);

// Detection pass for UNIT_STATUS_INVISIBLE. For each unit with numeric
// attribute "true_sight" > 0, queries enemy invisible units within
// that radius and ORs the detector's player bit onto their
// TrueSightVisibility component. Cleared and rebuilt every tick. The
// renderer consults the resulting mask in is_fog_hidden.
void system_true_sight(World& world, const SpatialGrid& grid);

} // namespace uldum::simulation
