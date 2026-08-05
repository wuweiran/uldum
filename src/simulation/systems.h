#pragma once

#include "core/types.h"

namespace uldum::map { struct TerrainData; }

namespace uldum::simulation {

struct World;
class Pathfinder;
class SpatialGrid;
class AbilityRegistry;

void system_state(World& world, float dt);
void system_combat(World& world, float dt, const SpatialGrid& grid);
void system_guard_position(World& world, float dt, const Pathfinder& pathfinder);
void system_movement(World& world, float dt, const Pathfinder& pathfinder,
                     const SpatialGrid& grid, const map::TerrainData* terrain);
void system_build(World& world, float dt, Pathfinder& pathfinder,
                  const map::TerrainData* terrain);
void system_ability(World& world, float dt, const AbilityRegistry& abilities,
                    const SpatialGrid& grid);
void system_items(World& world, float dt);
void system_projectile(World& world, float dt);
void system_collision(World& world, const SpatialGrid& grid,
                      const Pathfinder& pathfinder);
void system_death(World& world, float dt);
void system_regions(World& world);

void tick_projectile_death(World& world, float dt);

} // namespace uldum::simulation
