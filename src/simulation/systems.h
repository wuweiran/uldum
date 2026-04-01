#pragma once

namespace uldum::simulation {

struct World;
class Pathfinder;

// Each system processes one simulation tick (fixed timestep).
void system_health(World& world, float dt);
void system_state(World& world, float dt);
void system_movement(World& world, float dt, const Pathfinder& pathfinder);
void system_combat(World& world, float dt);
void system_ability(World& world, float dt);
void system_projectile(World& world, float dt);

} // namespace uldum::simulation
