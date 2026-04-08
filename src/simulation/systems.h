#pragma once

namespace uldum::simulation {

struct World;
class Pathfinder;
class SpatialGrid;

// Each system processes one simulation tick (fixed timestep).
void system_health(World& world, float dt);
void system_state(World& world, float dt);
void system_movement(World& world, float dt, const Pathfinder& pathfinder, const SpatialGrid& grid);
void system_combat(World& world, float dt, const Pathfinder& pathfinder, const SpatialGrid& grid);
void system_ability(World& world, float dt, const class AbilityRegistry& abilities, const SpatialGrid& grid);
void system_projectile(World& world, float dt, const Pathfinder& pathfinder);
void system_collision(World& world, const SpatialGrid& grid, const Pathfinder& pathfinder);
void system_death(World& world);
void system_scale_pulse(World& world, float dt);

} // namespace uldum::simulation
