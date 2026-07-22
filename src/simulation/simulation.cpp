#include "simulation/simulation.h"
#include "simulation/systems.h"
#include "asset/asset.h"
#include "map/terrain_data.h"
#include "core/log.h"

namespace uldum::simulation {

static constexpr const char* TAG = "Simulation";

bool is_static_remembered_entity(const World& world, u32 entity_id) {
    const auto* info = world.handle_infos.get(entity_id);
    if (!info) return false;
    if (info->category == Category::Destructable) return true;
    if (info->category == Category::Doodad) return true;
    if (info->category == Category::Unit) {
        const auto* cls = world.classifications.get(entity_id);
        if (cls && has_classification(cls->flags, "structure")) return true;
    }
    return false;
}

bool Simulation::init(asset::AssetManager& /*assets*/) {
    m_world.types     = &m_types;
    m_world.abilities = &m_abilities;

    // Wire pathing unblock: when a building is destroyed, release the
    // tile rectangle it occupied.
    // PathingBlocker stores its rect in cell units, so we forward directly
    // to unblock_cells. The parameter names below were tile-flavored
    // historically; they now carry cell coords.
    m_world.unblock_pathing = [this](i32 cx, i32 cy, u32 w, u32 h) {
        m_pathfinder.unblock_cells(cx, cy, w, h);
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
    m_vision.init(0, 0, 0, 0, FogMode::None);  // releases per-player grids
    m_alliances.clear();
    m_player_count = 0;
    m_player_names.clear();
    // Drop the client-mode world/vision overrides. The Simulation instance is
    // reused across sessions; if a Client game left these pointing at the
    // network client world, a following Offline/Host session would read the
    // stale world through world()/vision() while ticking m_world.
    m_world_override = nullptr;
    m_vision_override = nullptr;
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
        m_pathfinder.block_cells(blocker.cx, blocker.cy, blocker.w, blocker.h);
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
                                      Unit caster, Unit target,
                                      std::string* out_specifier) const {
    auto reject = [out_specifier](std::string spec) {
        if (out_specifier) *out_specifier = std::move(spec);
        return false;
    };

    // Read world through the accessor so the client's m_world_override
    // is honored — without this, MP clients query the empty server
    // simulation's world and every target is rejected.
    const World& w = world();
    if (!w.contains(target)) return reject("");

    // Liveness gate. `alive` defaults true in JSON (parser-side), so
    // most filters only accept living targets. `dead` lets resurrect-
    // style abilities target corpses; both can be true for either.
    bool dead = w.dead_states.has(target.id);
    if (!filter.alive && !filter.dead) return reject("");
    if (dead) {
        if (!filter.dead) return reject("dead");
    } else {
        if (!filter.alive) return reject("alive");
    }

    // Self / ally / enemy gate. At least one of these must be set for
    // the filter to accept ANY unit; an empty filter (all three false)
    // rejects everything by design — authors must opt in to who can
    // be targeted.
    bool is_self = caster == target;
    if (is_self) {
        if (!filter.self_) return reject("self");
    } else {
        const auto* caster_owner = w.owners.get(caster.id);
        const auto* target_owner = w.owners.get(target.id);
        if (!caster_owner || !target_owner) return reject("");
        bool allied = is_allied(*caster_owner, *target_owner);
        if (allied) {
            if (!filter.ally) return reject("ally");
        } else {
            if (!filter.enemy) return reject("enemy");
        }
    }

    // Optional classification list. If non-empty, the target's
    // classification set must contain at least one of the listed tags.
    if (!filter.classifications.empty()) {
        const auto* cls = w.classifications.get(target.id);
        if (!cls) return reject("");
        bool any = false;
        for (const auto& want : filter.classifications) {
            for (const auto& have : cls->flags) {
                if (have == want) { any = true; break; }
            }
            if (any) break;
        }
        // Report the target's own first flag as the specifier — the
        // simplest path and what ui.error.target.<flag> expects.
        if (!any) return reject(cls->flags.empty() ? std::string{} : cls->flags[0]);
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
    system_death(m_world, dt);
    // After all the state-changing systems — regions read final
    // positions and dead/alive state for this tick.
    system_regions(m_world);

    m_vision.update(m_world, *this);
}

} // namespace uldum::simulation
