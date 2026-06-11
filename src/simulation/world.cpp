#include "simulation/world.h"
#include "simulation/type_registry.h"
#include "simulation/ability_def.h"
#include "core/log.h"

#include <algorithm>
#include <unordered_map>

namespace uldum::simulation {

static constexpr const char* TAG = "World";

// ── Canonical per-entity teardown ──────────────────────────────────────────
// Declared in world.h. Every destroy path routes through this so the
// "list of all component pools" lives in exactly one place. See the
// header comment for the contract (doesn't free the handle).

void remove_all_components(World& world, u32 id) {
    world.transforms.remove(id);
    world.handle_infos.remove(id);
    world.healths.remove(id);
    world.state_blocks.remove(id);
    world.attribute_blocks.remove(id);
    world.selectables.remove(id);
    world.owners.remove(id);
    world.movements.remove(id);
    world.combats.remove(id);
    world.sights.remove(id);
    world.order_queues.remove(id);
    world.ability_sets.remove(id);
    world.classifications.remove(id);
    world.inventories.remove(id);
    world.buildings.remove(id);
    world.constructions.remove(id);
    world.destructables.remove(id);
    world.doodads.remove(id);
    if (world.on_pathing_unblock) {
        auto* pb = world.pathing_blockers.get(id);
        if (pb) world.on_pathing_unblock(pb->cx, pb->cy, pb->w, pb->h);
    }
    world.pathing_blockers.remove(id);
    world.item_infos.remove(id);
    world.carriables.remove(id);
    world.projectiles.remove(id);
    world.dead_states.remove(id);
    world.renderables.remove(id);
    world.anim_queues.remove(id);
    world.status_flags.remove(id);
    world.true_sight_vis.remove(id);
    world.forced_vis.remove(id);
}

// ── Creation ───────────────────────────────────────────────────────────────

Unit create_unit(World& world, std::string_view type_id, Player owner, f32 x, f32 y, f32 facing) {
    assert(world.types);
    const auto* def = world.types->get_unit_type(type_id);
    if (!def) {
        log::error(TAG, "Unknown unit type '{}'", type_id);
        return {};
    }

    Handle h = world.handles.allocate();
    u32 id = h.id;

    // All game objects
    world.transforms.add(id, Transform{{x, y, 0.0f}, facing, def->model_scale, {x, y, 0.0f}, facing});
    world.handle_infos.add(id, HandleInfo{std::string(type_id), Category::Unit, h.generation});

    // Widget — HP is engine built-in
    world.healths.add(id, Health{def->max_health, def->max_health, def->health_regen});
    world.selectables.add(id, Selectable{def->selection_radius, def->model_scale * 2.0f, def->selection_priority});

    // Map-defined states (mana, energy, etc.)
    if (!def->states.empty()) {
        StateBlock sb;
        for (auto& [sid, sd] : def->states) {
            sb.states[sid] = StateValue{sd.max, sd.max, sd.regen};
        }
        world.state_blocks.add(id, std::move(sb));
    }

    // Map-defined attributes (armor, attack_type, strength, etc.)
    if (!def->attributes_numeric.empty() || !def->attributes_string.empty()) {
        AttributeBlock ab;
        ab.base         = def->attributes_numeric;
        ab.numeric      = def->attributes_numeric;  // effective starts equal to base
        ab.string_attrs = def->attributes_string;
        world.attribute_blocks.add(id, std::move(ab));
    }

    // Unit
    world.owners.add(id, Player{owner});
    {
        Movement m;
        m.speed = def->move_speed;
        m.turn_rate = def->turn_rate;
        m.collision_radius = def->collision_radius;
        m.type = def->move_type;
        world.movements.add(id, std::move(m));
    }
    {
        Combat combat;
        combat.damage           = def->damage;
        combat.range            = def->attack_range;
        combat.attack_cooldown  = def->attack_cooldown;
        combat.dmg_time         = def->dmg_time;
        combat.backsw_time      = def->backsw_time;
        combat.dmg_pt           = def->dmg_pt;
        combat.is_ranged        = def->is_ranged;
        combat.projectile_speed = def->projectile_speed;
        combat.acquire_range    = def->acquire_range;
        world.combats.add(id, std::move(combat));
    }
    world.sights.add(id, Sight{def->sight_range});
    world.order_queues.add(id, OrderQueue{});
    world.ability_sets.add(id, AbilitySet{});
    world.classifications.add(id, UnitClassificationComp{def->classifications});

    // Seed ability_set from the unit type's `abilities` list. Each id
    // is looked up in the AbilityRegistry; unknowns are logged once
    // and skipped so a typo'd ability id doesn't take a slot. Lua can
    // still add more via `AddAbility` for runtime / script-driven
    // abilities (e.g. ones whose mechanics live entirely in script).
    if (world.abilities && !def->abilities.empty()) {
        Unit u{ id, h.generation };
        for (const auto& ability_id : def->abilities) {
            if (!world.abilities->get(ability_id)) {
                log::warn(TAG, "Unit '{}' references unknown ability '{}'",
                          type_id, ability_id);
                continue;
            }
            add_ability(world, *world.abilities, u, ability_id, /*level=*/1);
        }
    }

    // Renderable
    if (!def->model_path.empty()) {
        Renderable r{def->model_path, true};
        // Birth clip plays only for a unit spawned in the local viewer's
        // sight. A unit born outside sight (or on a host with no viewer
        // predicate set) comes up Idle — matching the network client,
        // which derives the same from the S_SPAWN newly_created flag.
        if (world.spawn_visible_to_viewer && !world.spawn_visible_to_viewer(x, y)) {
            r.skip_birth = true;
        }
        world.renderables.add(id, std::move(r));
    }

    // Inventory (if type has inventory_size > 0)
    if (def->inventory_size > 0) {
        Inventory inv;
        inv.max_slots = def->inventory_size;
        inv.slots.resize(def->inventory_size);
        world.inventories.add(id, std::move(inv));
    }

    // Building (if type has "structure" classification)
    if (has_classification(def->classifications, "structure")) {
        world.buildings.add(id, BuildingComp{});
    }

    // Play birth sound
    if (world.on_sound && !def->sound_birth.empty()) {
        auto* t = world.transforms.get(id);
        if (t) world.on_sound(def->sound_birth, t->position);
    }

    Unit unit;
    unit.id = h.id;
    unit.generation = h.generation;

    log::trace(TAG, "Created unit '{}' (id={}, owner={})", type_id, id, owner.id);
    return unit;
}

Destructable create_destructable(World& world, std::string_view type_id, f32 x, f32 y, f32 facing, u8 variation) {
    assert(world.types);
    const auto* def = world.types->get_destructable_type(type_id);
    if (!def) {
        log::error(TAG, "Unknown destructable type '{}'", type_id);
        return {};
    }

    Handle h = world.handles.allocate();
    u32 id = h.id;

    world.transforms.add(id, Transform{{x, y, 0.0f}, facing, def->model_scale, {x, y, 0.0f}, facing});
    world.handle_infos.add(id, HandleInfo{std::string(type_id), Category::Destructable, h.generation});
    world.healths.add(id, Health{def->max_health, def->max_health, 0});
    world.selectables.add(id, Selectable{1.0f, 50.0f, 1});
    world.destructables.add(id, DestructableComp{std::string(type_id), variation});

    if (!def->attributes_numeric.empty() || !def->attributes_string.empty()) {
        AttributeBlock ab;
        ab.base         = def->attributes_numeric;
        ab.numeric      = def->attributes_numeric;
        ab.string_attrs = def->attributes_string;
        world.attribute_blocks.add(id, std::move(ab));
    }

    if (!def->models.empty()) {
        u32 idx = (def->models.size() > 0) ? (variation % static_cast<u32>(def->models.size())) : 0;
        world.renderables.add(id, Renderable{def->models[idx], true});
    }

    // Movement-with-speed-0 carries the collision radius that combat
    // range checks read via world.movements.get(target.id). Destructables
    // never tick movement (no OrderQueue), so the Movement component is
    // inert apart from this field.
    if (def->collision_radius > 0) {
        Movement m{};
        m.speed = 0;
        m.turn_rate = 0;
        m.collision_radius = def->collision_radius;
        m.type = MoveType::Ground;
        world.movements.add(id, std::move(m));
    }

    Destructable d;
    d.id = h.id;
    d.generation = h.generation;

    log::trace(TAG, "Created destructable '{}' (id={})", type_id, id);
    return d;
}

Item create_item(World& world, std::string_view type_id, f32 x, f32 y) {
    assert(world.types);
    const auto* def = world.types->get_item_type(type_id);
    if (!def) {
        log::error(TAG, "Unknown item type '{}'", type_id);
        return {};
    }

    Handle h = world.handles.allocate();
    u32 id = h.id;

    world.transforms.add(id, Transform{{x, y, 0.0f}, 0, def->model_scale, {x, y, 0.0f}, 0});
    world.handle_infos.add(id, HandleInfo{std::string(type_id), Category::Item, h.generation});
    world.selectables.add(id, Selectable{32.0f, 24.0f, 1});
    world.item_infos.add(id, ItemInfo{std::string(type_id), def->initial_charges, def->initial_level});
    world.carriables.add(id, Carriable{});
    if (!def->model_path.empty()) {
        world.renderables.add(id, Renderable{def->model_path, true, true});
    }

    Item item;
    item.id = h.id;
    item.generation = h.generation;

    log::trace(TAG, "Created item '{}' (id={})", type_id, id);
    return item;
}

Doodad create_doodad(World& world, std::string_view type_id, f32 x, f32 y, f32 facing, u8 variation) {
    assert(world.types);
    const auto* def = world.types->get_doodad_type(type_id);
    if (!def) {
        log::error(TAG, "Unknown doodad type '{}'", type_id);
        return {};
    }

    Handle h = world.handles.allocate();
    u32 id = h.id;

    world.transforms.add(id, Transform{{x, y, 0.0f}, facing, def->model_scale, {x, y, 0.0f}, facing});
    world.handle_infos.add(id, HandleInfo{std::string(type_id), Category::Doodad, h.generation});
    world.doodads.add(id, DoodadComp{variation});
    if (!def->models.empty()) {
        u32 idx = variation % static_cast<u32>(def->models.size());
        world.renderables.add(id, Renderable{def->models[idx], true});
    }

    Doodad d;
    d.id = h.id;
    d.generation = h.generation;

    log::trace(TAG, "Created doodad '{}' (id={})", type_id, id);
    return d;
}

// ── Destruction ────────────────────────────────────────────────────────────

static void destroy_handle(World& world, Handle h) {
    if (!world.validate(h)) return;
    remove_all_components(world, h.id);
    world.handles.free(h);
}

void destroy(World& world, Unit unit)         { destroy_handle(world, unit); }
void destroy(World& world, Destructable d)    { destroy_handle(world, d); }
void destroy(World& world, Item item)         { destroy_handle(world, item); }
void destroy(World& world, Doodad d)          { destroy_handle(world, d); }

bool morph_unit(World& world, Unit unit, std::string_view new_type_id) {
    if (!world.validate(unit) || !world.types) return false;
    auto* hi = world.handle_infos.get(unit.id);
    if (!hi) return false;

    const auto* new_def = world.types->get_unit_type(new_type_id);
    if (!new_def) {
        log::error(TAG, "morph_unit: unknown type '{}'", std::string(new_type_id));
        return false;
    }
    const auto* old_def = world.types->get_unit_type(hi->type_id);
    // old_def may be null if the unit was created from a type that's
    // since been unregistered — still allow morph in that case, just
    // skip the "in-old-list" suspension step.

    const u32 id = unit.id;

    // Snapshot HP / state percentages for carry-over.
    f32 hp_pct = 1.0f;
    if (auto* h = world.healths.get(id); h && h->max > 0.0f) {
        hp_pct = std::clamp(h->current / h->max, 0.0f, 1.0f);
    }
    std::unordered_map<std::string, f32> state_pct;
    if (auto* sb = world.state_blocks.get(id)) {
        for (auto& [k, v] : sb->states) {
            if (v.max > 0.0f) state_pct[k] = std::clamp(v.current / v.max, 0.0f, 1.0f);
        }
    }

    // ── Component swap ─────────────────────────────────────────────────
    // type_id, scale.
    hi->type_id = std::string(new_type_id);
    if (auto* t = world.transforms.get(id)) {
        t->scale = new_def->model_scale;
    }

    // Health: re-seed max/regen, carry HP percentage.
    if (world.healths.has(id)) world.healths.remove(id);
    {
        Health h{};
        h.max     = new_def->max_health;
        h.current = new_def->max_health * hp_pct;
        h.regen_per_sec = new_def->health_regen;
        world.healths.add(id, std::move(h));
    }

    // Selectable.
    if (world.selectables.has(id)) world.selectables.remove(id);
    world.selectables.add(id, Selectable{new_def->selection_radius,
                                         new_def->model_scale * 2.0f,
                                         new_def->selection_priority});

    // StateBlock: re-seed; shared ids carry percentage, new ids start
    // at full, old-only ids are gone.
    if (world.state_blocks.has(id)) world.state_blocks.remove(id);
    if (!new_def->states.empty()) {
        StateBlock sb;
        for (auto& [sid, sd] : new_def->states) {
            f32 pct = 1.0f;
            if (auto it = state_pct.find(sid); it != state_pct.end()) pct = it->second;
            sb.states[sid] = StateValue{sd.max * pct, sd.max, sd.regen};
        }
        world.state_blocks.add(id, std::move(sb));
    }

    // AttributeBlock: replaced wholesale from new type def. base is
    // seeded from the type; numeric will be re-summed by
    // recalculate_modifiers below (passive ability modifiers stack on
    // top of base).
    if (world.attribute_blocks.has(id)) world.attribute_blocks.remove(id);
    if (!new_def->attributes_numeric.empty() || !new_def->attributes_string.empty()) {
        AttributeBlock ab;
        ab.base         = new_def->attributes_numeric;
        ab.numeric      = new_def->attributes_numeric;
        ab.string_attrs = new_def->attributes_string;
        world.attribute_blocks.add(id, std::move(ab));
    }

    // Movement: swap fields in place to preserve the component slot
    // (so other systems holding pointers aren't invalidated). Clears
    // in-flight path / approach state.
    if (auto* m = world.movements.get(id)) {
        m->speed            = new_def->move_speed;
        m->turn_rate        = new_def->turn_rate;
        m->collision_radius = new_def->collision_radius;
        m->type             = new_def->move_type;
        m->moving           = false;
        m->waypoint         = {0.0f, 0.0f};
        m->corridor.clear();
        m->approach_target  = Unit{};
        m->approach_goal    = {0.0f, 0.0f};
        m->approach_range   = 0.0f;
    }

    // Combat: swap, cancel any in-flight attack.
    if (auto* c = world.combats.get(id)) {
        c->damage           = new_def->damage;
        c->range            = new_def->attack_range;
        c->attack_cooldown  = new_def->attack_cooldown;
        c->dmg_time         = new_def->dmg_time;
        c->backsw_time      = new_def->backsw_time;
        c->dmg_pt           = new_def->dmg_pt;
        c->is_ranged        = new_def->is_ranged;
        c->projectile_speed = new_def->projectile_speed;
        c->acquire_range    = new_def->acquire_range;
        c->target           = Unit{};
        c->attack_state     = AttackState::Idle;
    }

    // Sight (per-unit sight radius).
    if (auto* v = world.sights.get(id)) {
        v->sight_range = new_def->sight_range;
    }

    // Clear order queue — old orders likely don't apply to new type.
    if (auto* oq = world.order_queues.get(id)) {
        oq->current.reset();
        oq->queued.clear();
    }

    // Ability set is deliberately untouched by morph — the engine
    // swaps the body (model, attributes, combat, movement, …) and the
    // map controls what abilities live on the unit. A typical morph
    // helper in map Lua removes the old type's abilities, calls
    // MorphUnit, adds the new type's abilities, and (if it cares)
    // preserves cooldowns via GetAbilityCooldown / SetAbilityCooldown.
    //
    // The one exception is in-flight cast state, which is bound to the
    // old combat / movement parameters we just swapped — cancel it
    // cleanly so the next tick doesn't run with stale targets.
    if (auto* aset = world.ability_sets.get(id)) {
        aset->cast_state       = CastState::None;
        aset->cast_timer       = 0;
        aset->casting_id.clear();
        aset->cast_target_unit = Unit{};
        aset->cast_target_pos  = glm::vec3{0};
        aset->cast_source_item = Item{};
    }

    // Classifications.
    if (world.classifications.has(id)) world.classifications.remove(id);
    world.classifications.add(id, UnitClassificationComp{new_def->classifications});

    // Renderable / model.
    if (world.renderables.has(id)) world.renderables.remove(id);
    if (!new_def->model_path.empty()) {
        world.renderables.add(id, Renderable{new_def->model_path, true});
    }

    // BuildingComp follows the structure classification.
    bool is_structure = has_classification(new_def->classifications, "structure");
    if (is_structure && !world.buildings.has(id)) world.buildings.add(id, BuildingComp{});
    if (!is_structure &&  world.buildings.has(id)) world.buildings.remove(id);

    // The attribute_block just got replaced wholesale, so any modifiers
    // previously summed in from passive abilities on the unit are gone.
    // Re-run the modifier roll-up so any passive still on the ability
    // set (i.e., not removed by the map's morph helper) gets its
    // modifiers applied to the fresh block.
    recalculate_modifiers(world, id);

    log::trace(TAG, "Morphed unit {} → '{}'", id, std::string(new_type_id));
    return true;
}

// ── Unit API ───────────────────────────────────────────────────────────────

void deal_damage(World& world, Unit source, Unit target, f32 amount, std::string_view damage_type) {
    auto* hp = world.healths.get(target.id);
    if (!hp || hp->current <= 0) return;

    // Invulnerable short-circuits before the on_damage script hook
    // fires — no damage event observed for unhittable units.
    if (unit_has_status(world, target, status::Invulnerable)) return;

    // Let script engine intercept (fire on_damage event, may modify amount)
    if (world.on_damage) {
        world.on_damage(source, target, amount, damage_type);
    }

    if (amount <= 0) return;

    // Re-fetch hp after the hook: an on_damage handler that spawns
    // (healths.add → realloc) or removes (sparse-set swap-remove) any
    // unit invalidates the pointer cached above. Without this re-fetch
    // we'd write through a dangling pointer (heap corruption) or apply
    // damage to whichever unit's Health got swapped into the slot. The
    // handler may also have killed the target outright, so re-check.
    hp = world.healths.get(target.id);
    if (!hp || hp->current <= 0) return;

    hp->current -= amount;
    if (hp->current < 0) hp->current = 0;

    // Fight-back: if target is idle (no order, no current target), attack the source.
    // Only fight back against enemies (different owner).
    if (source.is_valid() && world.validate(source)) {
        auto* src_owner = world.owners.get(source.id);
        auto* tgt_owner = world.owners.get(target.id);
        bool same_owner = src_owner && tgt_owner && src_owner->id == tgt_owner->id;
        if (!same_owner) {
            auto* oq = world.order_queues.get(target.id);
            auto* combat = world.combats.get(target.id);
            if (oq && combat && !oq->current && !combat->target.is_valid()) {
                combat->target = source;
            }
        }
    }
}

void issue_order(World& world, Unit unit, Order order) {
    if (!world.validate(unit)) return;
    auto* oq = world.order_queues.get(unit.id);
    if (!oq) return;

    // Status-flag admission, caster side. Stunned rejects every order
    // (the unit is frozen in place); Silenced rejects casts; Muted
    // rejects item-related orders. Pause is a sim-tick gate, not an
    // order gate — paused units still accept queued orders so they
    // resume correctly on unpause.
    {
        auto* sf = world.status_flags.get(unit.id);
        u32 flags = sf ? sf->flags : 0;
        if (flags & status::Stunned) return;
        if (std::holds_alternative<orders::Cast>(order.payload) &&
            (flags & status::Silenced)) {
            return;
        }
        bool is_item_order =
            std::holds_alternative<orders::PickupItem>(order.payload) ||
            std::holds_alternative<orders::DropItem>(order.payload) ||
            std::holds_alternative<orders::SwapInventorySlot>(order.payload);
        if (is_item_order && (flags & status::Muted)) {
            return;
        }
    }

    // Status-flag admission, target side. Untargetable rejects all
    // attack and cast orders. Unattackable rejects attacks only. Magic
    // immunity rejects hostile casts unless the ability has
    // pierces_immune. Allied / self casts (e.g. healing your ally who's
    // magic-immune) pass through the immunity check.
    auto target_flags = [&](Unit t) -> u32 {
        if (!t.is_valid() || !world.validate(t)) return 0;
        auto* sf = world.status_flags.get(t.id);
        return sf ? sf->flags : 0;
    };
    auto is_hostile = [&](Unit caster, Unit target) -> bool {
        if (!world.validate(caster) || !world.validate(target)) return false;
        auto* a = world.owners.get(caster.id);
        auto* b = world.owners.get(target.id);
        if (!a || !b) return false;
        return a->id != b->id;
    };
    if (auto* atk = std::get_if<orders::Attack>(&order.payload)) {
        u32 tf = target_flags(atk->target);
        if (tf & (status::Untargetable | status::Unattackable)) return;
    } else if (auto* cast = std::get_if<orders::Cast>(&order.payload)) {
        if (cast->target_unit.is_valid()) {
            u32 tf = target_flags(cast->target_unit);
            if (tf & status::Untargetable) return;
            if (tf & status::MagicImmune) {
                const auto* def = world.abilities ? world.abilities->get(cast->ability_id) : nullptr;
                bool pierces = def && def->pierces_immune;
                if (!pierces && is_hostile(unit, cast->target_unit)) return;
            }
        }
    }

    // Per-order-kind admission checks. `order.payload` is a variant —
    // at most one of these branches ever matches; the `else if` chain
    // makes that explicit. Self-targeting rejections here are
    // simulation-level invariants, not UI-time errors: a multi-select
    // Move/Attack on one of the selected units (or a Lua-issued Cast
    // on an enemy-only ability targeting self) must not slip in.
    if (auto* cast = std::get_if<orders::Cast>(&order.payload)) {
        // Cooldown — keep the existing activity untouched.
        auto* aset = world.ability_sets.get(unit.id);
        if (aset) {
            for (auto& a : aset->abilities) {
                if (a.ability_id == cast->ability_id && a.cooldown_remaining > 0) {
                    return;
                }
            }
        }
        // Self-target rejection for casts whose filter forbids it.
        if (cast->target_unit.is_valid() && cast->target_unit.id == unit.id) {
            const auto* def = world.abilities ? world.abilities->get(cast->ability_id) : nullptr;
            if (def && !def->target_filter.self_) return;
        }
    } else if (auto* mv = std::get_if<orders::Move>(&order.payload)) {
        if (mv->target_unit.is_valid() && mv->target_unit.id == unit.id) return;
    } else if (auto* atk = std::get_if<orders::Attack>(&order.payload)) {
        if (atk->target.is_valid() && atk->target.id == unit.id) return;
    }

    // EVENT_UNIT_ISSUED_ORDER fires here — after admission, before the
    // order takes effect on the queue. Lua may inspect / count / chain.
    if (world.on_order) {
        world.on_order(unit, order);
    }

    if (order.queued) {
        oq->queued.push_back(std::move(order));
    } else {
        oq->queued.clear();
        oq->current = std::move(order);

        // Clear pathfinding + approach state and force a fresh repath
        // on the next system_movement tick. Setting repath_timer = 0 is
        // load-bearing: `need_repath` is gated by rp_drift / rp_timer
        // only, so without this an identical-goal re-issue (e.g. the
        // player spam-clicking the same spot) wouldn't trigger A* and
        // the unit would stand still until the previous 1.5s timer
        // expired.
        auto* mov = world.movements.get(unit.id);
        if (mov) {
            mov->corridor.clear();
            mov->has_waypoint = false;
            mov->approach_target = Unit{};
            mov->approach_range = 0;
            mov->repath_timer = 0;
        }

        // Clear combat target so the unit stops fighting and obeys the new order
        auto* combat = world.combats.get(unit.id);
        if (combat) {
            combat->target = Unit{};
            combat->attack_state = AttackState::Idle;
        }

        // Reset any in-flight ability cast — a new order semantically
        // supersedes whatever the unit was doing, including a half-
        // started spell that's still walking toward its old target.
        // Without this, an out-of-range Cast that the unit can't reach
        // pins `cast_state` non-None forever, blocking every future
        // Cast (the cast pump only starts a new one when cast_state ==
        // None). Cooldown is left alone — already-fired casts that
        // are mid-backswing get cancelled cleanly.
        auto* aset = world.ability_sets.get(unit.id);
        if (aset) {
            aset->cast_state = CastState::None;
            aset->casting_id.clear();
            aset->cast_target_unit = Unit{};
            aset->cast_target_pos  = glm::vec3{0};
            aset->cast_timer       = 0;
        }
    }
}

f32 get_health(const World& world, Unit unit) {
    if (!world.validate(unit)) return 0;
    auto* h = world.healths.get(unit.id);
    return h ? h->current : 0;
}

void set_health(World& world, Unit unit, f32 health) {
    if (!world.validate(unit)) return;
    auto* h = world.healths.get(unit.id);
    if (h) h->current = std::min(health, h->max);
}

glm::vec3 get_position(const World& world, Unit unit) {
    if (!world.validate(unit)) return {};
    auto* t = world.transforms.get(unit.id);
    return t ? t->position : glm::vec3{};
}

void set_position(World& world, Unit unit, f32 x, f32 y) {
    if (!world.validate(unit)) return;
    auto* t = world.transforms.get(unit.id);
    if (t) { t->position.x = x; t->position.y = y; }
}

Player get_owner(const World& world, Unit unit) {
    if (!world.validate(unit)) return {};
    auto* o = world.owners.get(unit.id);
    return o ? *o : Player{};
}

bool is_alive(const World& world, Unit unit) {
    if (!world.validate(unit)) return false;
    auto* h = world.healths.get(unit.id);
    return h && h->current > 0;
}

bool is_dead(const World& world, Unit unit) {
    return world.validate(unit) && world.dead_states.has(unit.id);
}

bool is_building(const World& world, Unit unit) {
    return world.validate(unit) && world.buildings.has(unit.id);
}

// ── Destructable API ───────────────────────────────────────────────────────

f32 get_health(const World& world, Destructable d) {
    if (!world.validate(d)) return 0;
    auto* h = world.healths.get(d.id);
    return h ? h->current : 0;
}

void kill(World& world, Destructable d) {
    if (!world.validate(d)) return;
    auto* h = world.healths.get(d.id);
    if (h) h->current = 0;
}

// ── Item API ───────────────────────────────────────────────────────────────

i32 get_charges(const World& world, Item item) {
    if (!world.validate(item)) return 0;
    auto* info = world.item_infos.get(item.id);
    return info ? info->charges : 0;
}

void set_charges(World& world, Item item, i32 charges) {
    if (!world.validate(item)) return;
    auto* info = world.item_infos.get(item.id);
    if (info) info->charges = charges;
}

i32 get_level(const World& world, Item item) {
    if (!world.validate(item)) return 0;
    auto* info = world.item_infos.get(item.id);
    return info ? info->level : 0;
}

void set_level(World& world, Item item, i32 level) {
    if (!world.validate(item)) return;
    auto* info = world.item_infos.get(item.id);
    if (info) info->level = level;
}

Unit get_item_owner(const World& world, Item item) {
    if (!world.validate(item)) return {};
    auto* c = world.carriables.get(item.id);
    return c ? c->carried_by : Unit{};
}

i32 give_item_to_unit(World& world, Unit unit, Item item) {
    if (!world.validate(unit) || !world.validate(item)) return -1;
    auto* inv = world.inventories.get(unit.id);
    if (!inv) return -1;
    auto* car = world.carriables.get(item.id);
    if (!car) return -1;
    if (car->carried_by.is_valid()) return -1;  // already carried

    // First free slot.
    i32 slot = -1;
    for (i32 i = 0; i < static_cast<i32>(inv->slots.size()); ++i) {
        if (!inv->slots[i].is_valid()) { slot = i; break; }
    }
    if (slot < 0) return -1;

    inv->slots[slot] = item;
    car->carried_by  = unit;

    // Grant the item's abilities to the carrier. Each ability is added
    // at level 1 (level scaling lives on AbilityInstance, not item).
    // Mark them `from_item` so the action_bar hides their icons —
    // item abilities surface only through the inventory composite.
    if (world.abilities && world.types) {
        if (auto* def = world.types->get_item_type(world.item_infos.get(item.id)->type_id)) {
            for (const auto& aid : def->abilities) {
                add_ability(world, *world.abilities, unit, aid, 1);
                if (auto* aset = world.ability_sets.get(unit.id)) {
                    for (auto& inst : aset->abilities) {
                        if (inst.ability_id == aid) { inst.from_item = true; break; }
                    }
                }
            }
        }
    }

    // Hide the ground rendering — item still exists as an entity, but
    // its model no longer draws while in inventory.
    if (auto* r = world.renderables.get(item.id)) r->visible = false;

    return slot;
}

bool drop_item_from_unit(World& world, Unit unit, i32 slot, glm::vec3 pos) {
    if (!world.validate(unit)) return false;
    auto* inv = world.inventories.get(unit.id);
    if (!inv || slot < 0 || slot >= static_cast<i32>(inv->slots.size())) return false;
    Item item = inv->slots[slot];
    if (!world.validate(item)) return false;

    auto* car = world.carriables.get(item.id);
    if (!car || car->carried_by.id != unit.id) return false;

    // Revoke the item's abilities from the carrier.
    if (world.types) {
        auto* info = world.item_infos.get(item.id);
        if (info) {
            if (auto* def = world.types->get_item_type(info->type_id)) {
                for (const auto& aid : def->abilities) {
                    remove_ability(world, unit, aid);
                }
            }
        }
    }

    inv->slots[slot] = Item{};
    car->carried_by  = Unit{};

    // Place on ground at requested position.
    if (auto* tf = world.transforms.get(item.id)) {
        tf->position = pos;
        tf->prev_position = pos;
    }
    if (auto* r = world.renderables.get(item.id)) r->visible = true;

    return true;
}

// ── Ability API ───────────────────────────────────────────────────────────

// Copy the level's payload (modifiers, flags, declared duration) onto a
// fresh AbilityInstance. Centralised so add_ability and apply_passive
// agree on what an instance carries.
static void populate_instance_from_def(AbilityInstance& inst,
                                       const AbilityDef* def, u32 level) {
    if (!def) return;
    auto& lvl = def->level_data(level);
    inst.active_modifiers = lvl.modifiers;
    inst.active_flags     = lvl.flags;
    // Default duration from def applies when caller didn't override.
    if (inst.remaining_duration < 0.0f && lvl.duration >= 0.0f) {
        inst.remaining_duration = lvl.duration;
    }
}

bool add_ability(World& world, const AbilityRegistry& reg, Unit unit, std::string_view ability_id, u32 level) {
    if (!world.validate(unit)) return false;
    auto* aset = world.ability_sets.get(unit.id);
    if (!aset) return false;

    const auto* def = reg.get(ability_id);

    // Non-stackable: refresh duration on the existing instance rather
    // than rejecting the call. Flags / modifiers are unchanged because
    // the level didn't change — only the timer resets.
    if (def && !def->stackable) {
        for (auto& a : aset->abilities) {
            if (a.ability_id == ability_id) {
                auto& lvl = def->level_data(a.level);
                a.remaining_duration = lvl.duration;  // -1 if permanent
                return true;
            }
        }
    }

    AbilityInstance inst;
    inst.ability_id = std::string(ability_id);
    inst.level = level;
    inst.remaining_duration = -1.0f;  // pre-set to "no override"; populate may overwrite from def.duration
    populate_instance_from_def(inst, def, level);

    // Capture flag list before move for refcount bookkeeping.
    auto flags_snapshot = inst.active_flags;
    aset->abilities.push_back(std::move(inst));

    flag_refcount_delta(world, unit.id, flags_snapshot, +1);
    recalculate_modifiers(world, unit.id);
    if (world.on_ability_added) world.on_ability_added(unit, ability_id, level);

    log::trace(TAG, "AddAbility: unit {} + '{}' (level {})", unit.id, ability_id, level);
    return true;
}

bool remove_ability(World& world, Unit unit, std::string_view ability_id) {
    if (!world.validate(unit)) return false;
    auto* aset = world.ability_sets.get(unit.id);
    if (!aset) return false;

    // Remove all instances of this id (RemoveAbility is the hammer; for
    // a single-instance drop, use the natural duration expiry path).
    bool removed_any = false;
    for (auto it = aset->abilities.begin(); it != aset->abilities.end(); ) {
        if (it->ability_id == ability_id) {
            flag_refcount_delta(world, unit.id, it->active_flags, -1);
            it = aset->abilities.erase(it);
            removed_any = true;
        } else {
            ++it;
        }
    }
    if (!removed_any) return false;

    recalculate_modifiers(world, unit.id);
    if (world.on_ability_removed) {
        world.on_ability_removed(unit, ability_id);
    }

    log::trace(TAG, "RemoveAbility: unit {} - '{}' (all instances)", unit.id, ability_id);
    return true;
}

bool set_ability_level(World& world, const AbilityRegistry& reg, Unit unit,
                        std::string_view ability_id, u32 level) {
    if (!world.validate(unit)) return false;
    auto* aset = world.ability_sets.get(unit.id);
    if (!aset) return false;
    const auto* def = reg.get(ability_id);
    if (!def) return false;
    if (level < 1)                     level = 1;
    if (level > def->max_level)        level = def->max_level;

    // Operates on the first matching instance — same convention the
    // rest of the engine uses for stackable abilities (e.g. cooldown
    // reads). Stack-aware level mutation would need a separate API.
    for (auto& inst : aset->abilities) {
        if (inst.ability_id != ability_id) continue;
        if (inst.level == level) return true;

        // Swap flag refcounts atomically: drop the old level's flag
        // contributions, install the new level's modifiers + flags,
        // bump refcounts for what the new level brought in.
        flag_refcount_delta(world, unit.id, inst.active_flags, -1);
        inst.level = level;
        populate_instance_from_def(inst, def, level);
        flag_refcount_delta(world, unit.id, inst.active_flags, +1);
        recalculate_modifiers(world, unit.id);
        return true;
    }
    return false;
}

bool apply_passive_ability(World& world, const AbilityRegistry& reg, Unit target,
                           std::string_view ability_id, Unit source, f32 duration) {
    if (!world.validate(target)) return false;
    auto* aset = world.ability_sets.get(target.id);
    if (!aset) return false;

    const auto* def = reg.get(ability_id);

    // Non-stackable: refresh duration if already present. Refcounts are
    // already counted on the existing instance, nothing to bump.
    if (def && !def->stackable) {
        for (auto& a : aset->abilities) {
            if (a.ability_id == ability_id) {
                a.remaining_duration = duration;
                a.source = source;
                return true;
            }
        }
    }

    AbilityInstance inst;
    inst.ability_id = std::string(ability_id);
    inst.level = 1;
    inst.source = source;
    inst.remaining_duration = duration;
    populate_instance_from_def(inst, def, 1);
    // Caller-provided duration wins over the def's declared duration.
    inst.remaining_duration = duration;

    auto flags_snapshot = inst.active_flags;
    aset->abilities.push_back(std::move(inst));

    flag_refcount_delta(world, target.id, flags_snapshot, +1);
    recalculate_modifiers(world, target.id);
    if (world.on_ability_added) world.on_ability_added(target, ability_id, 1);

    log::trace(TAG, "ApplyPassiveAbility: '{}' on unit {} (duration={:.1f}s)", ability_id, target.id, duration);
    return true;
}

bool has_ability(const World& world, Unit unit, std::string_view ability_id) {
    if (!world.validate(unit)) return false;
    auto* aset = world.ability_sets.get(unit.id);
    if (!aset) return false;
    for (auto& a : aset->abilities) {
        if (a.ability_id == ability_id) return true;
    }
    return false;
}

u32 get_ability_stack_count(const World& world, Unit unit, std::string_view ability_id) {
    if (!world.validate(unit)) return 0;
    auto* aset = world.ability_sets.get(unit.id);
    if (!aset) return 0;
    u32 count = 0;
    for (auto& a : aset->abilities) {
        if (a.ability_id == ability_id) count++;
    }
    return count;
}

u32 get_ability_level(const World& world, Unit unit, std::string_view ability_id) {
    if (!world.validate(unit)) return 0;
    auto* aset = world.ability_sets.get(unit.id);
    if (!aset) return 0;
    for (auto& a : aset->abilities) {
        if (a.ability_id == ability_id) return a.level;
    }
    return 0;
}

// Map a single-bit flag value (e.g. status::Invisible) to its refcount
// array index. Returns status::Count when the value isn't a single bit
// in range — callers should ignore it.
static u32 flag_bit_index(u32 flag) {
    for (u32 i = 0; i < status::Count; ++i) {
        if (flag == (1u << i)) return i;
    }
    return status::Count;
}

// Recompute the effective `flags` bitset from `manual_bits` and the
// refcount array. Cheap (12 iterations); called whenever either layer
// mutates so downstream readers always see a consistent OR-overlay.
static void recompute_effective_flags(StatusFlags& sf) {
    u32 effective = sf.manual_bits;
    for (u32 i = 0; i < status::Count; ++i) {
        if (sf.refcounts[i] > 0) effective |= (1u << i);
    }
    sf.flags = effective;
}

bool unit_has_status(const World& world, Unit unit, u32 flag) {
    if (!world.validate(unit)) return false;
    auto* sf = world.status_flags.get(unit.id);
    return sf && (sf->flags & flag) != 0;
}

void set_unit_status(World& world, Unit unit, u32 flag, bool on) {
    if (!world.validate(unit)) return;
    auto* sf = world.status_flags.get(unit.id);
    if (!sf) {
        if (!on) return;  // clearing a bit that doesn't exist: no-op
        world.status_flags.add(unit.id, StatusFlags{});
        sf = world.status_flags.get(unit.id);
        if (!sf) return;
    }
    if (on) sf->manual_bits |=  flag;
    else    sf->manual_bits &= ~flag;
    recompute_effective_flags(*sf);
}

void clear_all_unit_status(World& world, Unit unit) {
    if (!world.validate(unit)) return;
    auto* sf = world.status_flags.get(unit.id);
    if (sf) {
        sf->manual_bits = 0;
        recompute_effective_flags(*sf);
    }
}

// Increment / decrement refcount for each flag named in `flag_names`.
// Unknown names are logged and ignored. Adds a StatusFlags component on
// first touch.
void flag_refcount_delta(World& world, u32 id,
                         const std::vector<std::string>& flag_names,
                         i32 delta) {
    if (flag_names.empty() || delta == 0) return;
    auto* sf = world.status_flags.get(id);
    if (!sf) {
        world.status_flags.add(id, StatusFlags{});
        sf = world.status_flags.get(id);
        if (!sf) return;
    }
    auto parse = [](std::string_view s) -> u32 {
        if (s == "stunned")      return status::Stunned;
        if (s == "silenced")     return status::Silenced;
        if (s == "muted")        return status::Muted;
        if (s == "disarmed")     return status::Disarmed;
        if (s == "rooted")       return status::Rooted;
        if (s == "invulnerable") return status::Invulnerable;
        if (s == "magic_immune") return status::MagicImmune;
        if (s == "untargetable") return status::Untargetable;
        if (s == "unattackable") return status::Unattackable;
        if (s == "paused")       return status::Paused;
        if (s == "invisible")    return status::Invisible;
        if (s == "no_acquire")   return status::NoAcquire;
        return 0;
    };
    for (auto& name : flag_names) {
        u32 bit = parse(name);
        if (!bit) {
            log::warn(TAG, "flag_refcount: unknown flag '{}'", name);
            continue;
        }
        u32 idx = flag_bit_index(bit);
        if (idx >= status::Count) continue;
        if (delta > 0) {
            if (sf->refcounts[idx] < 0xFF) sf->refcounts[idx]++;
        } else {
            if (sf->refcounts[idx] > 0)    sf->refcounts[idx]--;
        }
    }
    recompute_effective_flags(*sf);
}

bool ability_can_afford(const World& world, u32 unit_id,
                        const std::map<std::string, f32>& cost) {
    if (cost.empty()) return true;
    for (const auto& [state_name, amount] : cost) {
        if (amount <= 0.0f) continue;
        if (state_name == "health") {
            // Health cost can never kill: require strictly more than the
            // cost so the caster stays at >= 1 HP. (No cost_can_kill /
            // suicide-cast opt-in — removed by design.)
            const auto* hp = world.healths.get(unit_id);
            if (!hp || hp->current <= amount) return false;
            continue;
        }
        const auto* sb = world.state_blocks.get(unit_id);
        if (!sb) return false;
        auto it = sb->states.find(state_name);
        if (it == sb->states.end() || it->second.current < amount) return false;
    }
    return true;
}

void ability_pay_cost(World& world, u32 unit_id,
                      const std::map<std::string, f32>& cost) {
    if (cost.empty()) return;
    for (const auto& [state_name, amount] : cost) {
        if (amount <= 0.0f) continue;
        if (state_name == "health") {
            if (auto* hp = world.healths.get(unit_id)) {
                hp->current -= amount;
                if (hp->current < 1.0f) hp->current = 1.0f;  // hard floor — never lethal
            }
            continue;
        }
        if (auto* sb = world.state_blocks.get(unit_id)) {
            auto it = sb->states.find(state_name);
            if (it != sb->states.end()) {
                it->second.current -= amount;
                if (it->second.current < 0.0f) it->second.current = 0.0f;
            }
        }
    }
}

void recalculate_modifiers(World& world, u32 id) {
    // Effective = (base + sum(bare-key)) * (1 + sum(*_mult)).
    // Bare keys are additive; only the `_mult` suffix is special.
    // `_mult` values are unit fractions (-0.5 = -50%, +0.25 = +25%) —
    // multiple sources sum before the (1 + sum) multiplier applies.
    //
    // flat/pct are built from ability_sets unconditionally because
    // they feed three independent passes: the AttributeBlock numeric
    // map (skipped if the unit has no attributes), the Movement speed,
    // and the Renderable visual alpha. The latter two need to keep
    // working on attribute-less units (e.g. summons with a slow / fade
    // buff but no JSON `attributes` block) — the old early-return on
    // missing AttributeBlock skipped both.
    std::unordered_map<std::string, f32> flat;
    std::unordered_map<std::string, f32> pct;

    auto strip_suffix = [](std::string_view k, std::string_view suffix) -> std::string {
        return std::string(k.substr(0, k.size() - suffix.size()));
    };

    if (auto* aset = world.ability_sets.get(id)) {
        for (const auto& inst : aset->abilities) {
            for (const auto& [k, delta] : inst.active_modifiers) {
                std::string_view kv{k};
                if (kv.size() > 5 && kv.substr(kv.size() - 5) == "_mult") {
                    pct[strip_suffix(kv, "_mult")] += delta;
                } else {
                    flat[k] += delta;
                }
            }
        }
    }

    if (auto* attrs = world.attribute_blocks.get(id)) {
        attrs->numeric = attrs->base;
        for (auto& [k, v] : flat) {
            attrs->numeric[k] += v;
        }
        for (auto& [k, p] : pct) {
            attrs->numeric[k] = attrs->numeric[k] * (1.0f + p);
        }
    }

    // Movement speed. Lives on Movement, not in the attribute block,
    // so it needs an explicit pass: rebuild from the type def's base
    // and re-apply move_speed (additive) / move_speed_mult every recalc.
    if (auto* mov = world.movements.get(id)) {
        f32 base_speed = 0;
        if (world.types) {
            if (auto* info = world.handle_infos.get(id)) {
                if (auto* def = world.types->get_unit_type(info->type_id)) {
                    base_speed = def->move_speed;
                }
            }
        }
        auto fit = flat.find("move_speed");
        auto pit = pct.find("move_speed");
        f32 fv = (fit != flat.end()) ? fit->second : 0.0f;
        f32 pv = (pit != pct.end())  ? pit->second  : 0.0f;
        mov->speed = std::max(0.0f, (base_speed + fv) * (1.0f + pv));
    }

    // Visual attributes (alpha, eventually scale / tint) live on
    // Renderable rather than the attribute block — the renderer reads
    // them every frame without an attribute lookup. Compose them here
    // so abilities, not Lua setters, drive the values. Base is the
    // multiplicative identity (1.0); buffs declare `visual_alpha_mult`
    // as a unit-fraction (e.g. -0.5 for a half-translucent ghost).
    // Multiple sources sum.
    if (auto* r = world.renderables.get(id)) {
        f32 alpha_mult_sum = 0.0f;
        if (auto* aset = world.ability_sets.get(id)) {
            for (const auto& inst : aset->abilities) {
                auto it = inst.active_modifiers.find("visual_alpha_mult");
                if (it != inst.active_modifiers.end()) alpha_mult_sum += it->second;
            }
        }
        r->visual_alpha = std::clamp(1.0f + alpha_mult_sum, 0.0f, 1.0f);
    }
}

} // namespace uldum::simulation
