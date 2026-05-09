#include "simulation/simulation.h"
#include "simulation/systems.h"
#include "asset/asset.h"
#include "map/terrain_data.h"
#include "core/log.h"

namespace uldum::simulation {

static constexpr const char* TAG = "Simulation";

bool Simulation::init(asset::AssetManager& /*assets*/) {
    m_world.types     = &m_types;
    m_world.abilities = &m_abilities;

    // Wire pathing unblock: when a building is destroyed, release the
    // tile rectangle it occupied.
    m_world.on_pathing_unblock = [this](i32 tx, i32 ty, u32 w, u32 h) {
        m_pathfinder.unblock_tiles(tx, ty, w, h);
    };

    log::info(TAG, "Simulation initialized");
    return true;
}

void Simulation::shutdown() {
    // Wipe all per-session state so the next start_session begins
    // from a clean slate. The Simulation instance itself is reused
    // across sessions, so nothing leaves scope on its own.
    m_world.clear_entities();
    m_pathfinder.set_terrain(nullptr);   // drops runtime blocks too
    m_terrain = nullptr;
    m_types.clear();
    m_abilities.clear();
    m_fog.init(0, 0, 0, 0, FogMode::None);  // releases per-player grids
    m_alliances.clear();
    m_player_count = 0;
    m_player_names.clear();
    log::info(TAG, "Simulation shut down");
}

void Simulation::set_terrain(const map::TerrainData* terrain) {
    m_terrain = terrain;
    m_pathfinder.set_terrain(terrain);
    if (terrain && terrain->is_valid()) {
        m_spatial_grid.init(terrain->world_width(), terrain->world_height(), 512.0f, this);
    }
}

void Simulation::sync_pathing_blockers() {
    for (u32 i = 0; i < m_world.pathing_blockers.count(); ++i) {
        auto& blocker = m_world.pathing_blockers.data()[i];
        m_pathfinder.block_tiles(blocker.tx, blocker.ty, blocker.w, blocker.h);
    }
}

void Simulation::init_alliances(u32 player_count) {
    m_player_count = player_count;
    m_alliances.resize(player_count * player_count, AllianceFlags{});

    // Each player is allied with themselves (with shared vision)
    for (u32 i = 0; i < player_count; ++i) {
        m_alliances[i * player_count + i] = {true, false, true};
    }
}

void Simulation::set_alliance(Player a, Player b, bool allied, bool passive) {
    if (a.id >= m_player_count || b.id >= m_player_count) return;
    auto& flags = m_alliances[a.id * m_player_count + b.id];
    flags.allied = allied;
    flags.passive = passive;
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

void Simulation::set_shared_vision(Player a, Player b, bool shared) {
    if (a.id >= m_player_count || b.id >= m_player_count) return;
    m_alliances[a.id * m_player_count + b.id].shared_vision = shared;
}

bool Simulation::has_shared_vision(Player a, Player b) const {
    if (a.id == b.id) return true;
    if (a.id >= m_player_count || b.id >= m_player_count) return false;
    return m_alliances[a.id * m_player_count + b.id].shared_vision;
}

bool Simulation::target_filter_passes(const TargetFilter& filter,
                                      Unit caster, Unit target) const {
    if (!m_world.validate(target)) return false;

    // Liveness gate. `alive` defaults true in JSON (parser-side), so
    // most filters only accept living targets. `dead` lets resurrect-
    // style abilities target corpses; both can be true for either.
    bool dead = m_world.dead_states.has(target.id);
    if (!filter.alive && !filter.dead) return false;
    if (dead) {
        if (!filter.dead) return false;
    } else {
        if (!filter.alive) return false;
    }

    // Self / ally / enemy gate. At least one of these must be set for
    // the filter to accept ANY unit; an empty filter (all three false)
    // rejects everything by design — authors must opt in to who can
    // be targeted.
    bool is_self = (caster.id == target.id) && (caster.generation == target.generation);
    if (is_self) {
        if (!filter.self_) return false;
    } else {
        const auto* caster_owner = m_world.owners.get(caster.id);
        const auto* target_owner = m_world.owners.get(target.id);
        if (!caster_owner || !target_owner) return false;
        bool allied = is_allied(caster_owner->player, target_owner->player);
        if (allied) {
            if (!filter.ally) return false;
        } else {
            if (!filter.enemy) return false;
        }
    }

    // Optional classification list. If non-empty, the target's
    // classification set must contain at least one of the listed tags.
    if (!filter.classifications.empty()) {
        const auto* cls = m_world.classifications.get(target.id);
        if (!cls) return false;
        bool any = false;
        for (const auto& want : filter.classifications) {
            for (const auto& have : cls->flags) {
                if (have == want) { any = true; break; }
            }
            if (any) break;
        }
        if (!any) return false;
    }

    return true;
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
    system_movement(m_world, dt, m_pathfinder, m_spatial_grid, m_terrain);
    system_combat(m_world, dt, m_spatial_grid);
    system_ability(m_world, dt, m_abilities, m_spatial_grid);
    system_items(m_world, dt);
    system_projectile(m_world, dt);
    system_collision(m_world, m_spatial_grid, m_pathfinder);
    system_death(m_world);
    system_scale_pulse(m_world, dt);
    // After all the state-changing systems — regions read final
    // positions and dead/alive state for this tick.
    system_regions(m_world);

    m_fog.update(m_world, *this);
}

} // namespace uldum::simulation
