#include "simulation/simulation.h"
#include "simulation/systems.h"
#include "asset/asset.h"
#include "core/log.h"

namespace uldum::simulation {

static constexpr const char* TAG = "Simulation";

bool Simulation::init(asset::AssetManager& assets) {
    // Load type definitions
    m_types.load_unit_types(assets, "config/unit_types.json");
    m_types.load_destructable_types(assets, "config/destructable_types.json");
    m_types.load_item_types(assets, "config/item_types.json");

    // Wire world to type registry
    m_world.types = &m_types;

    log::info(TAG, "Simulation initialized — {} unit types, {} destructable types, {} item types",
              m_types.unit_type_count(), m_types.destructable_type_count(), m_types.item_type_count());
    return true;
}

void Simulation::shutdown() {
    log::info(TAG, "Simulation shut down");
}

void Simulation::tick(float dt) {
    system_health(m_world, dt);
    system_movement(m_world, dt);
    system_combat(m_world, dt);
    system_ability(m_world, dt);
    system_buff(m_world, dt);
    system_projectile(m_world, dt);
}

} // namespace uldum::simulation
