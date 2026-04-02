#include "simulation/simulation.h"
#include "simulation/systems.h"
#include "asset/asset.h"
#include "map/terrain_data.h"
#include "core/log.h"

namespace uldum::simulation {

static constexpr const char* TAG = "Simulation";

bool Simulation::init(asset::AssetManager& /*assets*/) {
    m_world.types = &m_types;

    log::info(TAG, "Simulation initialized");
    return true;
}

void Simulation::shutdown() {
    log::info(TAG, "Simulation shut down");
}

void Simulation::set_terrain(const map::TerrainData* terrain) {
    m_pathfinder.set_terrain(terrain);
    if (terrain && terrain->is_valid()) {
        m_spatial_grid.init(terrain->world_width(), terrain->world_height(), 8.0f);
    }
}

void Simulation::tick(float dt) {
    m_spatial_grid.update(m_world);

    system_health(m_world, dt);
    system_state(m_world, dt);
    system_movement(m_world, dt, m_pathfinder, m_spatial_grid);
    system_combat(m_world, dt, m_pathfinder);
    system_ability(m_world, dt);
    system_projectile(m_world, dt, m_pathfinder);
    system_death(m_world);
    system_scale_pulse(m_world, dt);
}

} // namespace uldum::simulation
