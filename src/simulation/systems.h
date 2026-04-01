#pragma once

namespace uldum::simulation {

struct World;

// Each system processes one simulation tick (fixed timestep).
void system_health(World& world, float dt);
void system_state(World& world, float dt);  // tick regen for map-defined states
void system_movement(World& world, float dt);
void system_combat(World& world, float dt);
void system_ability(World& world, float dt); // includes applied ability (buff) duration + aura scanning
void system_projectile(World& world, float dt);

} // namespace uldum::simulation
