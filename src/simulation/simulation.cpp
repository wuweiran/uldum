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
        m_spatial_grid.init(terrain->world_width(), terrain->world_height(), 512.0f, this);
    }
}

void Simulation::init_alliances(u32 player_count) {
    m_player_count = player_count;
    m_alliances.resize(player_count * player_count, AllianceFlags{});

    // Each player is allied with themselves
    for (u32 i = 0; i < player_count; ++i) {
        m_alliances[i * player_count + i] = {true, false};
    }
}

void Simulation::set_alliance(Player a, Player b, bool allied, bool passive) {
    if (a.id >= m_player_count || b.id >= m_player_count) return;
    m_alliances[a.id * m_player_count + b.id] = {allied, passive};
}

bool Simulation::is_allied(Player a, Player b) const {
    if (a.id == b.id) return true;
    if (a.id >= m_player_count || b.id >= m_player_count) return false;
    return m_alliances[a.id * m_player_count + b.id].allied;
}

bool Simulation::is_passive(Player a, Player b) const {
    if (a.id >= m_player_count || b.id >= m_player_count) return false;
    return m_alliances[a.id * m_player_count + b.id].passive;
}

bool Simulation::is_enemy(Player a, Player b) const {
    if (a.id == b.id) return false;
    return !is_allied(a, b);
}

void Simulation::tick(float dt) {
    // Snapshot transforms for render interpolation
    for (u32 i = 0; i < m_world.transforms.count(); ++i) {
        auto& t = m_world.transforms.data()[i];
        t.prev_position = t.position;
        t.prev_facing   = t.facing;
    }

    m_spatial_grid.update(m_world);

    system_health(m_world, dt);
    system_state(m_world, dt);
    system_movement(m_world, dt, m_pathfinder, m_spatial_grid);
    system_combat(m_world, dt, m_pathfinder, m_spatial_grid);
    system_ability(m_world, dt, m_abilities, m_spatial_grid);
    system_projectile(m_world, dt, m_pathfinder);
    system_collision(m_world, m_spatial_grid, m_pathfinder);
    system_death(m_world);
    system_scale_pulse(m_world, dt);
}

} // namespace uldum::simulation
