#include "simulation/simulation.h"
#include "simulation/systems.h"
#include "asset/asset.h"
#include "core/log.h"

namespace uldum::simulation {

static constexpr const char* TAG = "Simulation";

bool Simulation::init(asset::AssetManager& assets) {
    // Wire world to type registry (types loaded externally — by map system or test code)
    m_world.types = &m_types;

    log::info(TAG, "Simulation initialized");
    return true;
}

void Simulation::shutdown() {
    log::info(TAG, "Simulation shut down");
}

void Simulation::tick(float dt) {
    system_health(m_world, dt);
    system_state(m_world, dt);
    system_movement(m_world, dt, m_pathfinder);
    system_combat(m_world, dt);
    system_ability(m_world, dt);
    system_projectile(m_world, dt);
}

} // namespace uldum::simulation
