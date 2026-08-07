#include "network/game_client.h"
#include "simulation/vision.h"
#include "simulation/world_view.h"
#include "simulation/type_registry.h"
#include "simulation/ability_def.h"
#include "simulation/world.h"
#include "asset/asset.h"
#include "core/log.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <chrono>
#include <unordered_map>

namespace uldum::network {

static constexpr const char* TAG = "GameClient";

static f64 wall_time() {
    using namespace std::chrono;
    return duration<f64>(steady_clock::now().time_since_epoch()).count();
}

static void apply_unit_state_scalars(simulation::World& world, const UnitState& state) {
    if (state.flags & 0x20) {
        if (auto* health = world.healths.get(state.entity_id)) {
            health->current = state.health_current;
        }
    }
    if (auto* movement = world.movements.get(state.entity_id)) {
        movement->moving = (state.flags & 0x01) != 0;
    }
    if (auto* abilities = world.ability_sets.get(state.entity_id)) {
        bool casting = (state.flags & 0x04) != 0;
        if (casting && abilities->cast_state == simulation::CastState::None) {
            abilities->cast_state = simulation::CastState::Foreswing;
        } else if (!casting && abilities->cast_state != simulation::CastState::None) {
            abilities->cast_state = simulation::CastState::None;
        }
    }
    if (!state.state_currents.empty()) {
        if (auto* states = world.state_blocks.get(state.entity_id);
            states && states->states.size() == state.state_currents.size()) {
            usize index = 0;
            for (auto& [name, value] : states->states) {
                value.current = state.state_currents[index++];
            }
        }
    }
}

template <typename Record>
static void interpolate_transform(simulation::World& world, const Record& current,
                                  const Record* previous, f32 alpha) {
    auto* transform = world.transforms.get(current.entity_id);
    if (!transform) return;
    if (!previous) {
        transform->prev_position = transform->position =
            {current.x, current.y, current.z};
        transform->prev_facing = transform->facing = current.facing;
        return;
    }

    glm::vec3 position{
        previous->x + (current.x - previous->x) * alpha,
        previous->y + (current.y - previous->y) * alpha,
        previous->z + (current.z - previous->z) * alpha};
    glm::vec3 tick_delta{
        current.x - previous->x,
        current.y - previous->y,
        current.z - previous->z};
    transform->position = position;
    transform->prev_position = position - tick_delta;

    f32 difference = current.facing - previous->facing;
    while (difference > glm::pi<f32>()) difference -= glm::two_pi<f32>();
    while (difference < -glm::pi<f32>()) difference += glm::two_pi<f32>();
    transform->facing = previous->facing + difference * alpha;
    transform->prev_facing = transform->facing - difference;
}

bool GameClient::init_simulation(asset::AssetManager& assets) {
    if (!m_simulation.init(assets)) {
        log::error(TAG, "Simulation init failed");
        return false;
    }
    return true;
}

void GameClient::tick(f32 dt) {
    apply_interpolation();
    m_simulation.client_tick(dt);
}

void GameClient::shutdown() {
    m_view = nullptr;
    m_snapshots[0] = Snapshot{};
    m_snapshots[1] = Snapshot{};
    m_snap_idx = 0;
    m_has_two_snaps = false;
    m_simulation.shutdown();
}


void GameClient::spawn_entity(const MaterializeData& data, bool play_birth) {
    auto& world = m_simulation.world();
    if (world.handle_infos.has(data.entity_id)) return;
    simulation::SpawnOpts opts;
    opts.skip_birth = !play_birth;
    switch (data.category) {
    case MaterializeCategory::Unit: {
        const auto& payload = std::get<UnitMaterialize>(data.payload);
        simulation::spawn_unit_with_id(
            world, data.entity_id, data.type_id, simulation::Player{payload.owner},
            data.x, data.y, payload.facing, opts);
        break;
    }
    case MaterializeCategory::Destructable: {
        const auto& payload = std::get<DestructableMaterialize>(data.payload);
        simulation::spawn_destructable_with_id(
            world, data.entity_id, data.type_id, data.x, data.y,
            payload.facing, payload.variation);
        break;
    }
    case MaterializeCategory::Item:
        simulation::spawn_item_with_id(
            world, data.entity_id, data.type_id, data.x, data.y, opts);
        break;
    case MaterializeCategory::Projectile: {
        const auto& payload = std::get<ProjectileMaterialize>(data.payload);
        simulation::spawn_projectile_with_id(
            world, data.entity_id, data.type_id, data.x, data.y, payload.facing);
        auto* projectile = world.projectiles.get(data.entity_id);
        projectile->target = simulation::Widget{payload.target_id};
        projectile->is_attack = payload.is_attack;
        break;
    }
    }
}

void GameClient::destroy_entity(u32 entity_id) {
    simulation::remove_all_components(m_simulation.world(), entity_id);
}

void GameClient::apply_spawn(const MaterializeData& data, bool play_birth) {
    spawn_entity(data, play_birth);
    m_view->drop_snapshot(data.entity_id);
}

void GameClient::apply_hide(u32 entity_id) {
    auto& world = m_simulation.world();
    if (simulation::is_static_remembered_entity(world, entity_id)) {
        m_view->snapshot_from(world, entity_id);
    }
    destroy_entity(entity_id);
}

void GameClient::apply_destroy(u32 entity_id) {
    m_view->drop_snapshot(entity_id);
    destroy_entity(entity_id);
}

u32 GameClient::begin_snapshot_tick(u32 tick) {
    if (m_snapshots[m_snap_idx].tick == tick &&
        m_snapshots[m_snap_idx].receive_time > 0) {
        return m_snap_idx;
    }
    u32 older = m_snap_idx;
    u32 newer = 1 - m_snap_idx;
    m_snapshots[newer].tick = tick;
    m_snapshots[newer].receive_time = wall_time();
    m_snapshots[newer].units.clear();
    m_snapshots[newer].projectiles.clear();
    m_snap_idx = newer;
    if (!m_has_two_snaps && m_snapshots[older].receive_time > 0) {
        m_has_two_snaps = true;
    }
    return newer;
}

void GameClient::apply_unit_state(UnitStateData state) {
    u32 index = begin_snapshot_tick(state.tick);
    m_snapshots[index].units = std::move(state.units);
    apply_interpolation();
}

void GameClient::apply_projectile_state(ProjectileStateData state) {
    u32 index = begin_snapshot_tick(state.tick);
    m_snapshots[index].projectiles = std::move(state.projectiles);
    apply_interpolation();
}

void GameClient::apply_attack_start(const AnimEventData& event) {
    auto* combat = m_simulation.world().combats.get(event.entity_id);
    if (!combat) return;
    combat->target = simulation::Widget{event.target_id};
    combat->dmg_time = event.windup;
    combat->backsw_time = event.backswing;
    combat->dmg_pt = event.damage_point;
    combat->attack_speed = 1.0f;
    simulation::begin_swing(*combat);
}

std::optional<u32> GameClient::apply_projectile_dying(u32 entity_id) {
    auto& world = m_simulation.world();
    const auto* projectile = world.projectiles.get(entity_id);
    std::optional<u32> hit;
    if (projectile && projectile->is_attack &&
        simulation::is_non_null_handle(projectile->target)) {
        hit = projectile->target.id;
    }
    simulation::enter_projectile_dying(world, entity_id);
    return hit;
}

void GameClient::apply_cold(ColdData cold) {
    for (const auto& record : cold.records) {
        apply_cold_record(cold.entity_id, record);
    }
}

void GameClient::apply_interpolation() {
    auto& world = m_simulation.world();
    if (!m_has_two_snaps) {
        auto& snapshot = m_snapshots[m_snap_idx];
        for (const auto& state : snapshot.units) {
            interpolate_transform(world, state, static_cast<const UnitState*>(nullptr), 1.0f);
            apply_unit_state_scalars(world, state);
        }
        for (const auto& state : snapshot.projectiles) {
            interpolate_transform(
                world, state, static_cast<const ProjectileState*>(nullptr), 1.0f);
        }
        return;
    }

    auto& older = m_snapshots[1 - m_snap_idx];
    auto& newer = m_snapshots[m_snap_idx];
    f64 render_time = wall_time() - (1.0 / 32.0);
    f32 alpha = 0.0f;
    f64 span = newer.receive_time - older.receive_time;
    if (span > 0.0) {
        alpha = std::clamp(
            static_cast<f32>((render_time - older.receive_time) / span),
            0.0f, 1.0f);
    }

    std::unordered_map<u32, const UnitState*> old_units;
    for (const auto& state : older.units) old_units[state.entity_id] = &state;
    for (const auto& state : newer.units) {
        auto found = old_units.find(state.entity_id);
        interpolate_transform(
            world, state, found != old_units.end() ? found->second : nullptr, alpha);
        apply_unit_state_scalars(world, state);
    }

    std::unordered_map<u32, const ProjectileState*> old_projectiles;
    for (const auto& state : older.projectiles) {
        old_projectiles[state.entity_id] = &state;
    }
    for (const auto& state : newer.projectiles) {
        auto found = old_projectiles.find(state.entity_id);
        interpolate_transform(
            world, state,
            found != old_projectiles.end() ? found->second : nullptr, alpha);
    }
}


void GameClient::apply_cold_record(u32 entity_id, const ColdRecord& rec) {
    auto& world = m_simulation.world();
    using network::ColdKind;
    switch (rec.kind) {
    case ColdKind::Health: {
        auto* h = world.healths.get(entity_id);
        if (h) { h->current = rec.value; h->max = rec.value2; }
        break;
    }
    case ColdKind::Attribute: {
        auto* attrs = world.attribute_blocks.get(entity_id);
        if (!attrs) {
            world.attribute_blocks.add(entity_id, simulation::AttributeBlock{});
            attrs = world.attribute_blocks.get(entity_id);
        }
        attrs->numeric[rec.key] = rec.value;
        break;
    }
    case ColdKind::StringAttribute: {
        auto* attrs = world.attribute_blocks.get(entity_id);
        if (!attrs) {
            world.attribute_blocks.add(entity_id, simulation::AttributeBlock{});
            attrs = world.attribute_blocks.get(entity_id);
        }
        attrs->string_attrs[rec.key] = rec.str_value;
        break;
    }
    case ColdKind::State: {
        auto* states = world.state_blocks.get(entity_id);
        if (!states) {
            world.state_blocks.add(entity_id, simulation::StateBlock{});
            states = world.state_blocks.get(entity_id);
        }
        auto& state = states->states[rec.key];
        state.current = rec.value;
        state.max = rec.value2;
        break;
    }
    case ColdKind::AbilityAdd: {
        // Mirror server-side add_ability: populate active_modifiers /
        // active_flags from the def, bump per-flag refcounts, and run
        // recalculate_modifiers so the client's renderable visual_alpha
        // (and any other derived values) match the server. Without
        // this, passive_flag buffs like windwalk_invisible would have
        // no effect on the client — the unit wouldn't appear invisible
        // or carry its alpha modifier even though it does on the host.
        auto* aset = world.ability_sets.get(entity_id);
        if (!aset) {
            world.ability_sets.add(entity_id, simulation::AbilitySet{});
            aset = world.ability_sets.get(entity_id);
        }
        const simulation::AbilityDef* def =
            world.abilities ? world.abilities->get(rec.key) : nullptr;

        simulation::AbilitySourceKind source_kind =
            static_cast<simulation::AbilitySourceKind>(rec.byte_value);
        simulation::AbilitySource source;
        if (source_kind == simulation::AbilitySourceKind::Item) {
            source.value = simulation::ItemAbilitySource{};
        } else if (source_kind == simulation::AbilitySourceKind::Applied) {
            source.value = simulation::AppliedAbilitySource{};
        } else {
            source.value = simulation::InnateAbilitySource{};
        }
        source.remaining_duration = def
            ? def->level_data(rec.uint_value).duration
            : -1.0f;

        simulation::AbilityInstance* instance = nullptr;
        if (def && !def->stackable) {
            for (auto& candidate : aset->abilities) {
                if (candidate.ability_id == rec.key) {
                    instance = &candidate;
                    break;
                }
            }
        }

        if (instance) {
            if (source_kind == simulation::AbilitySourceKind::Item) {
                instance->sources.push_back(source);
            } else {
                auto existing = std::find_if(
                    instance->sources.begin(), instance->sources.end(),
                    [&](const simulation::AbilitySource& candidate) {
                        return simulation::ability_source_kind(candidate) == source_kind;
                    });
                if (existing != instance->sources.end()) {
                    existing->remaining_duration = source.remaining_duration;
                } else {
                    instance->sources.push_back(source);
                }
            }
        } else {
            simulation::AbilityInstance created;
            created.ability_id = rec.key;
            created.level = rec.uint_value;
            created.sources.push_back(source);
            if (def) {
                auto& lvl = def->level_data(rec.uint_value);
                created.active_modifiers = lvl.modifiers;
                created.active_flags     = lvl.flags;
            }
            auto flags_snapshot = created.active_flags;
            aset->abilities.push_back(std::move(created));
            simulation::flag_refcount_delta(world, entity_id, flags_snapshot, +1);
        }
        simulation::recalculate_modifiers(world, entity_id);
        break;
    }
    case ColdKind::AbilityRemove: {
        auto* aset = world.ability_sets.get(entity_id);
        if (aset) {
            simulation::AbilitySourceKind source_kind =
                static_cast<simulation::AbilitySourceKind>(rec.byte_value);
            auto& abilities = aset->abilities;
            for (auto instance = abilities.begin();
                 instance != abilities.end(); ) {
                if (instance->ability_id != rec.key) {
                    ++instance;
                    continue;
                }

                if (rec.bool_value) {
                    simulation::flag_refcount_delta(
                        world, entity_id, instance->active_flags, -1);
                    instance = abilities.erase(instance);
                    continue;
                }

                auto source = std::find_if(
                    instance->sources.begin(), instance->sources.end(),
                    [&](const simulation::AbilitySource& candidate) {
                        return simulation::ability_source_kind(candidate) == source_kind;
                    });
                if (source == instance->sources.end()) {
                    ++instance;
                    continue;
                }
                instance->sources.erase(source);
                if (instance->sources.empty()) {
                    simulation::flag_refcount_delta(
                        world, entity_id, instance->active_flags, -1);
                    abilities.erase(instance);
                }
                break;
            }
            simulation::recalculate_modifiers(world, entity_id);
        }
        break;
    }
    case ColdKind::Owner: {
        auto* owner = world.owners.get(entity_id);
        if (owner) owner->id = rec.uint_value;
        break;
    }
    case ColdKind::AbilityModifier: {
        // Mirror SetAbilityModifier: mutate the named modifier on every
        // matching instance and recalculate. Drives Lua-side tweens
        // like Wind Walk's fade-in.
        auto* aset = world.ability_sets.get(entity_id);
        if (!aset) break;
        bool changed = false;
        for (auto& a : aset->abilities) {
            if (a.ability_id == rec.key) {
                a.active_modifiers[rec.str_value] = rec.value;
                changed = true;
            }
        }
        if (changed) simulation::recalculate_modifiers(world, entity_id);
        break;
    }
    case ColdKind::Transform: {
        if (auto* t = world.transforms.get(entity_id)) {
            t->position = {rec.x, rec.y, rec.z};
            t->prev_position = t->position;
            t->facing = rec.facing;
            t->prev_facing = rec.facing;
        }
        break;
    }
    case ColdKind::Cooldown: {
        // Apply the cooldown to every instance of `ability_id` on the
        // unit — matches the server's SetAbilityCooldown / Reset
        // semantics (which iterate all matching instances).
        auto* aset = world.ability_sets.get(entity_id);
        if (aset) {
            for (auto& a : aset->abilities) {
                if (a.ability_id == rec.key) {
                    a.cooldown_remaining = std::max(0.0f, rec.value);
                }
            }
        }
        break;
    }
    case ColdKind::ItemCharges:
        simulation::set_charges(
            world, simulation::Item{entity_id}, static_cast<i32>(rec.uint_value));
        break;
    case ColdKind::ItemLevel:
        simulation::set_level(
            world, simulation::Item{entity_id}, static_cast<i32>(rec.uint_value));
        break;
    case ColdKind::Inventory: {
        // Item pickup / drop mirrored onto the client. entity_id = carrier.
        //   item_id != INVALID → PICKUP into `slot`: put the item in the
        //     carrier's Inventory slot, mark its Carriable, hide ground render.
        //   item_id == INVALID → (shouldn't happen; kept for symmetry).
        //   slot == UINT32_MAX  → DROP: find item_id in the carrier's slots,
        //     clear it + Carriable, restore ground render.
        u32 carrier_id = entity_id;
        u32 slot       = rec.uint_value;
        u32 item_id    = rec.uint_value2;

        if (slot == UINT32_MAX) {
            // Drop: locate the item in the carrier's slots and clear it.
            if (auto* inv = world.inventories.get(carrier_id)) {
                for (auto& s : inv->slots) {
                    if (s.id == item_id) { s = simulation::Item{}; break; }
                }
            }
            if (auto* car = world.carriables.get(item_id)) car->carried_by = simulation::Unit{};
            // Place the item at its drop position (rides the drop event now that
            // items are off the per-tick snapshot) and restore the ground render.
            // Without the position the client would re-show the item at its stale
            // spawn transform (the "item teleports back to where it was born" bug).
            if (auto* t = world.transforms.get(item_id)) {
                t->position = {rec.x, rec.y, rec.z};
                t->prev_position = t->position;
            }
            if (auto* r = world.renderables.get(item_id)) r->visible = true;
            break;
        }

        // Pickup: ensure the carrier has a sized Inventory, then set the slot.
        auto* inv = world.inventories.get(carrier_id);
        if (!inv) {
            simulation::Inventory fresh;
            // Size from the unit type def's inventory_size (fallback: grow to fit).
            if (world.types) {
                if (const auto* hi = world.handle_infos.get(carrier_id)) {
                    if (const auto* ud = world.types->get_unit_type(hi->type_id)) {
                        fresh.max_slots = ud->inventory_size;
                        fresh.slots.resize(ud->inventory_size);
                    }
                }
            }
            world.inventories.add(carrier_id, std::move(fresh));
            inv = world.inventories.get(carrier_id);
        }
        if (inv) {
            if (slot >= inv->slots.size()) inv->slots.resize(slot + 1);
            inv->slots[slot] = simulation::Item{item_id};
        }
        // Mark the item carried (lazy-add Carriable if the ground item lacked it).
        if (world.handle_infos.has(item_id)) {
            if (auto* car = world.carriables.get(item_id)) {
                car->carried_by = simulation::Unit{carrier_id};
            } else {
                world.carriables.add(item_id, simulation::Carriable{simulation::Unit{carrier_id}});
            }
            // Hide the ground render immediately (don't wait for S_UNIT_STATE — the
            // host stops shipping the item's state once it's carried).
            if (auto* r = world.renderables.get(item_id)) r->visible = false;
        }
        break;
    }
    case ColdKind::Anim: {
        world.anim_queues.remove(entity_id);
        if (!rec.clip.empty()) {
            simulation::AnimQueue queue;
            queue.clips.push_back(rec.clip);
            queue.looping = true;
            world.anim_queues.add(entity_id, std::move(queue));
        }
        break;
    }
    case ColdKind::Construction: {
        // Under-construction state for the client's time-scaled birth clip. While
        // building, add/update the mirror Construction so derive_anim_state plays
        // the stretched birth (seeded to build_progress on materialize). On finish
        // (under_construction=false) drop it → the building falls through to Idle.
        if (rec.bool_value) {
            simulation::Construction c;
            c.under_construction = true;
            c.build_time_total   = rec.value;
            c.build_progress     = rec.value2;
            if (auto* existing = world.constructions.get(entity_id)) *existing = c;
            else world.constructions.add(entity_id, std::move(c));
        } else {
            world.constructions.remove(entity_id);
        }
        break;
    }
    }
}

} // namespace uldum::network
