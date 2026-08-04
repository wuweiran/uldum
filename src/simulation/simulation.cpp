#include "simulation/simulation.h"
#include "simulation/systems.h"
#include "asset/asset.h"
#include "map/map.h"
#include "map/terrain_data.h"
#include "core/log.h"

#include <nlohmann/json.hpp>
#include <cmath>

namespace uldum::simulation {

static constexpr const char* TAG = "Simulation";

bool register_map_types(Simulation& sim, asset::AssetManager& assets,
                        const std::string& map_root) {
    auto& types = sim.types();
    std::string types_dir = map_root + "/types/";
    try {
        types.load_unit_types_absolute(assets, types_dir + "units.json");
        types.load_destructable_types_absolute(assets, types_dir + "destructables.json");
        types.load_item_types_absolute(assets, types_dir + "items.json");
        types.load_doodad_types_absolute(assets, types_dir + "doodads.json");
        sim.abilities().load(assets, types_dir + "abilities.json");
    } catch (const nlohmann::json::exception& e) {
        log::error(TAG, "Map type data under '{}' has a malformed field: {}", types_dir, e.what());
        return false;
    }
    log::info(TAG, "Types loaded — {} units, {} destructables, {} doodads, {} items",
              types.unit_type_count(), types.destructable_type_count(),
              types.doodad_type_count(), types.item_type_count());
    return true;
}

void apply_scene_data(Simulation& sim, map::SceneData& scene) {
    auto& world = sim.world();
    auto& td = scene.terrain;

    auto sample_height = [&](f32 x, f32 y) -> f32 {
        if (!td.is_valid()) return 0.0f;
        u32 ix = std::min(static_cast<u32>((x - td.origin_x()) / td.tile_size), td.tiles_x);
        u32 iy = std::min(static_cast<u32>((y - td.origin_y()) / td.tile_size), td.tiles_y);
        return td.world_z_at(ix, iy);
    };

    u32 unit_count = 0;
    for (auto& pu : scene.units) {
        const auto* type_def = sim.types().get_unit_type(pu.type);
        u32 fw = type_def ? type_def->pathing_footprint_w : 0u;
        u32 fh = type_def ? type_def->pathing_footprint_h : 0u;
        if (fw > 0 && fh > 0 && td.is_valid()) {
            pu.x = map::snap_building_x(td, pu.x, fw);
            pu.y = map::snap_building_y(td, pu.y, fh);
        }

        Player owner{pu.owner};
        auto unit = create_unit(world, pu.type, owner, pu.x, pu.y, pu.facing);
        if (is_null_handle(unit)) continue;

        // Preplaced units are authored map state — they existed before any
        // player was watching, so they never play birth.
        if (auto* r = world.renderables.get(unit.id)) r->skip_birth = true;

        if (auto* t = world.transforms.get(unit.id)) t->position.z = sample_height(pu.x, pu.y);
        if (auto* pth = world.pathings.get(unit.id)) {
            u32 vx = std::min(static_cast<u32>(std::round((pu.x - td.origin_x()) / td.tile_size)), td.tiles_x);
            u32 vy = std::min(static_cast<u32>(std::round((pu.y - td.origin_y()) / td.tile_size)), td.tiles_y);
            pth->cliff_level = td.cliff_at(vx, vy);
        }

        if (fw > 0 && fh > 0 && td.is_valid()) {
            f32 left_tx_f   = (pu.x - td.origin_x()) / td.tile_size - 0.5f * static_cast<f32>(fw);
            f32 bottom_ty_f = (pu.y - td.origin_y()) / td.tile_size - 0.5f * static_cast<f32>(fh);
            i32 tx0 = static_cast<i32>(std::round(left_tx_f));
            i32 ty0 = static_cast<i32>(std::round(bottom_ty_f));
            PathingBlocker blocker;
            blocker.cx = tx0 * static_cast<i32>(PATHING_SUBDIV);
            blocker.cy = ty0 * static_cast<i32>(PATHING_SUBDIV);
            blocker.w  = fw * PATHING_SUBDIV;
            blocker.h  = fh * PATHING_SUBDIV;
            world.pathing_blockers.add(unit.id, std::move(blocker));
        }
        unit_count++;
    }

    u32 dest_count = 0;
    for (auto& pd : scene.destructables) {
        const auto* def = sim.types().get_destructable_type(pd.type);
        u32 fw = def ? def->pathing_footprint_w : 0u;
        u32 fh = def ? def->pathing_footprint_h : 0u;
        if (td.is_valid()) {
            pd.x = map::snap_cell_x(td, pd.x);
            pd.y = map::snap_cell_y(td, pd.y);
        }

        auto dest = create_destructable(world, pd.type, pd.x, pd.y, pd.facing, pd.variation);
        if (is_null_handle(dest)) continue;

        if (auto* t = world.transforms.get(dest.id)) {
            t->position.z = sample_height(pd.x, pd.y);
            t->prev_position.z = t->position.z;
        }
        if (fw > 0 && fh > 0 && td.is_valid()) {
            f32 cs = td.tile_size / static_cast<f32>(PATHING_SUBDIV);
            f32 left_cx_f   = (pd.x - td.origin_x()) / cs - 0.5f * static_cast<f32>(fw);
            f32 bottom_cy_f = (pd.y - td.origin_y()) / cs - 0.5f * static_cast<f32>(fh);
            PathingBlocker blocker;
            blocker.cx = static_cast<i32>(std::round(left_cx_f));
            blocker.cy = static_cast<i32>(std::round(bottom_cy_f));
            blocker.w  = fw;
            blocker.h  = fh;
            world.pathing_blockers.add(dest.id, std::move(blocker));
        }
        dest_count++;
    }

    u32 item_count = 0;
    for (const auto& pi : scene.items) {
        auto item = create_item(world, pi.type, pi.x, pi.y);
        if (is_null_handle(item)) continue;
        if (auto* t = world.transforms.get(item.id)) {
            t->position.z      = sample_height(pi.x, pi.y);
            t->prev_position.z = t->position.z;
        }
        item_count++;
    }

    u32 dood_count = 0;
    for (const auto& pd : scene.doodads) {
        auto dood = create_doodad(world, pd.type, pd.x, pd.y, pd.facing, pd.variation);
        if (is_null_entity(dood)) continue;
        if (auto* t = world.transforms.get(dood.id)) {
            t->position.z = sample_height(pd.x, pd.y);
            t->prev_position.z = t->position.z;
        }
        dood_count++;
    }

    for (const auto& r : scene.regions) {
        u32 rid = ++world.next_region_id;
        World::Region wr;
        wr.id     = rid;
        wr.id_str = r.id;
        for (const auto& rect : r.rects) wr.rects.push_back({rect.x0, rect.y0, rect.x1, rect.y1});
        for (const auto& c : r.circles) wr.circles.push_back({c.cx, c.cy, c.r});
        world.regions[rid] = std::move(wr);
    }

    log::info(TAG, "Placements: {} units, {} destructables, {} doodads, {} items, {} regions",
              unit_count, dest_count, dood_count, item_count, scene.regions.size());
}

void export_scene_data(const World& world, map::SceneData& out) {
    for (u32 i = 0; i < world.handle_infos.count(); ++i) {
        u32 id = world.handle_infos.ids()[i];
        const auto& info = world.handle_infos.data()[i];
        const auto* t = world.transforms.get(id);
        if (!t) continue;

        switch (info.category) {
        case Category::Unit: {
            map::PlacedUnit pu;
            pu.type = info.type_id; pu.x = t->position.x; pu.y = t->position.y; pu.facing = t->facing;
            if (const auto* owner = world.owners.get(id)) pu.owner = owner->id;
            out.units.push_back(std::move(pu));
            break;
        }
        case Category::Destructable: {
            map::PlacedDestructable pd;
            pd.type = info.type_id; pd.x = t->position.x; pd.y = t->position.y; pd.facing = t->facing;
            if (const auto* dc = world.destructables.get(id)) pd.variation = dc->variation;
            out.destructables.push_back(std::move(pd));
            break;
        }
        case Category::Item: {
            map::PlacedItem pi;
            pi.type = info.type_id; pi.x = t->position.x; pi.y = t->position.y;
            out.items.push_back(std::move(pi));
            break;
        }
        case Category::Doodad: {
            map::PlacedDoodad pd;
            pd.type = info.type_id; pd.x = t->position.x; pd.y = t->position.y; pd.facing = t->facing;
            if (const auto* dc = world.doodads.get(id)) pd.variation = dc->variation;
            out.doodads.push_back(std::move(pd));
            break;
        }
        default:
            break;
        }
    }
}

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
    log::info(TAG, "Simulation shut down");
}

void Simulation::set_terrain(const map::TerrainData* terrain) {
    m_terrain = terrain;
    // Set it on the ACTIVE world (the client's override world when set, else the
    // authoritative m_world) so creation — which builds into world() — can sample
    // ground height. Also set the base m_world so it's correct if the override is
    // later cleared.
    m_world.terrain = terrain;
    world().terrain = terrain;
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

    // Reads this sim's own world — the client's GameClient sim owns the mirror,
    // so an MP client resolves targets against the replicated entities directly.
    const World& w = world();
    if (!w.contains(target)) return reject("");

    // Liveness gate. `alive` defaults true in JSON (parser-side), so
    // most filters only accept living targets. `dead` lets resurrect-
    // style abilities target corpses; both can be true for either.
    bool dead = w.corpses.has(target.id);
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

    // `any`: if non-empty, the target must carry at least one listed tag.
    if (!filter.any.empty()) {
        const auto* cls = w.classifications.get(target.id);
        if (!cls) return reject("");
        bool matched = false;
        for (const auto& want : filter.any) {
            for (const auto& have : cls->flags) {
                if (have == want) { matched = true; break; }
            }
            if (matched) break;
        }
        // Report the target's own first flag as the specifier — the
        // simplest path and what error.target.<flag> expects.
        if (!matched) return reject(cls->flags.empty() ? std::string{} : cls->flags[0]);
    }

    // `not_any`: if the target carries any listed tag, reject and report it
    // (e.g. error.target.structure).
    if (!filter.not_any.empty()) {
        if (const auto* cls = w.classifications.get(target.id)) {
            for (const auto& banned : filter.not_any) {
                for (const auto& have : cls->flags) {
                    if (have == banned) return reject(banned);
                }
            }
        }
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
    // Combat before movement: combat decides strike-vs-approach and sets the
    // approach target/range; movement executes it the same tick. Reversed, a
    // fresh Attack on an in-range target nudged one tick before combat could
    // stop it (combat.target still cleared by issue_order).
    system_combat(m_world, dt, m_spatial_grid);
    system_movement(m_world, dt, m_pathfinder, m_spatial_grid, m_terrain);
    // Build after movement: movement consumes the approach fields system_build
    // sets to walk the worker to its site; on arrival system_build spawns the
    // structure and advances construction.
    system_build(m_world, dt, m_pathfinder, m_terrain);
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

// The network client's per-frame derivation pass — the sibling of tick().
// INVARIANT: never runs a rules system (movement / combat / collision / death /
// ability / projectile). The client's world is replicated truth; re-running
// rules would double-simulate and fight the snapshots. This only decays
// local-only timers the host doesn't stream continuously (they'd otherwise
// freeze) and recomputes fog.
void Simulation::client_tick(float dt) {
    if (dt <= 0) return;
    World& w = world();

    // Attack cycle: advance the swing state machine so attack anims + flinch play
    // (the timing half of system_combat; no acquisition/damage — host's job).
    for (u32 i = 0; i < w.combats.count(); ++i) {
        auto& combat = w.combats.data()[i];
        if (combat.attack_state == AttackState::Idle) continue;

        if (!advance_swing(combat, dt)) continue;

        if (combat.attack_state == AttackState::Backswing) {
            // Damage point — bump hit_count so the target's renderer plays
            // the flinch clip, matching the host's system_health.
            if (combat.target.id != UINT32_MAX) {
                if (auto* thp = w.healths.get(combat.target.id)) {
                    ++thp->hit_count;
                }
            }
        } else if (combat.attack_state == AttackState::Idle) {
            // The host owns target validity and streams attack_state.
            begin_swing(combat);
        }
    }

    // Ability cooldowns: the host broadcasts the cooldown START but the client
    // never runs system_abilities, so without this the HUD's cooldown_remaining
    // freezes at its start value and the slot greys forever.
    for (u32 i = 0; i < w.ability_sets.count(); ++i) {
        auto& aset = w.ability_sets.data()[i];
        for (auto& ability : aset.abilities) {
            if (ability.cooldown_remaining > 0.0f) {
                ability.cooldown_remaining -= dt;
                if (ability.cooldown_remaining < 0.0f) ability.cooldown_remaining = 0.0f;
            }
        }
    }

    // Projectile teardown: same clip-sized death_timer drain the host runs in
    // system_projectile. The client owns this — the host sends no S_DESTROY for
    // projectiles.
    tick_projectile_death(w, dt);

    // Recompute fog from the mirror world — the call tick() ends with. Without it
    // the client fog never advances past its init state. (true-sight queries this
    // sim's spatial_grid, which the client never ticks, so it relies on the host's
    // send-gate — same as before.)
    if (m_vision.enabled()) {
        m_vision.update(w, *this);
    }
}

} // namespace uldum::simulation
