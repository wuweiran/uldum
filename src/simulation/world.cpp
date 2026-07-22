#include "simulation/world.h"
#include "simulation/type_registry.h"
#include "simulation/ability_def.h"
#include "core/log.h"

#include <algorithm>
#include <unordered_map>

namespace uldum::simulation {

static constexpr const char* TAG = "World";

// Upper bound on shift-queued orders behind the in-progress one. Legitimate
// WC3-style waypoint/patrol chains are a handful deep; this ceiling only
// exists so a scripted or hostile client can't grow a unit's deque without
// bound (memory-exhaustion DoS). Orders past the cap are dropped.
static constexpr usize MAX_QUEUED_ORDERS = 64;

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

    Handle h = world.entities.allocate();
    u32 id = h.id;

    // All game objects
    world.transforms.add(id, Transform{{x, y, 0.0f}, facing, def->model_scale, {x, y, 0.0f}, facing});
    world.handle_infos.add(id, HandleInfo{std::string(type_id), Category::Unit});

    // Widget — HP is engine built-in
    world.healths.add(id, Health{def->max_health, def->max_health, def->health_regen});
    // Selection cylinder. radius/height carry the type's values, or 0 = AUTO —
    // the renderer back-fills auto entries from the model AABB once it loads.
    world.selectables.add(id, Selectable{def->selection_radius, def->selection_height, def->selection_priority});

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
    // Combat is opt-in: only units whose type declares a `weapon` get the
    // component. system_combat iterates world.combats.ids(), so a unit
    // without it is invisible to the entire combat loop — no auto-acquire,
    // no attack orders resolving, no fight-back. Buildings and other
    // non-combatants leave the weapon absent.
    if (def->weapon) {
        const auto& w = *def->weapon;
        Combat combat;
        combat.damage           = w.damage;
        combat.range            = w.attack_range;
        combat.attack_cooldown  = w.attack_cooldown;
        combat.dmg_time         = w.dmg_time;
        combat.backsw_time      = w.backsw_time;
        combat.dmg_pt           = def->dmg_pt;
        combat.projectile       = w.projectile;
        combat.acquire_range    = def->acquire_range;
        combat.target_mask      = w.target_mask;
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
        Unit u{id};
        for (const auto& ability_id : def->abilities) {
            if (!world.abilities->get(ability_id)) {
                log::warn(TAG, "Unit '{}' references unknown ability '{}'",
                          type_id, ability_id);
                continue;
            }
            add_ability(world, *world.abilities, u, ability_id, /*level=*/1);
            if (!world.contains(u)) return {};
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

    Unit unit{h.id};
    return unit;
}

Destructable create_destructable(World& world, std::string_view type_id, f32 x, f32 y, f32 facing, u8 variation) {
    assert(world.types);
    const auto* def = world.types->get_destructable_type(type_id);
    if (!def) {
        log::error(TAG, "Unknown destructable type '{}'", type_id);
        return {};
    }

    Handle h = world.entities.allocate();
    u32 id = h.id;

    world.transforms.add(id, Transform{{x, y, 0.0f}, facing, def->model_scale, {x, y, 0.0f}, facing});
    world.handle_infos.add(id, HandleInfo{std::string(type_id), Category::Destructable});
    world.healths.add(id, Health{def->max_health, def->max_health, 0});
    // Selection auto-sizes from the model AABB (renderer back-fill), same as
    // units. 0/0 = auto; the crate/tree mesh drives the click cylinder.
    world.selectables.add(id, Selectable{0.0f, 0.0f, 1});
    world.destructables.add(id, DestructableComp{std::string(type_id), variation, def->target_bit, def->selectable});

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

    // No Movement component: a destructable is not a unit and takes no part
    // in unit collision. Its pathing_footprint (stamped as a PathingBlocker
    // by the caller) is what keeps units clear of it — collision_radius would
    // be redundant with that footprint and only made the crate a spurious
    // push participant. Combat range/targeting against a destructable reads
    // a missing Movement gracefully (target_radius 0, layer defaults Ground).

    Destructable d{h.id};
    return d;
}

Item create_item(World& world, std::string_view type_id, f32 x, f32 y) {
    assert(world.types);
    const auto* def = world.types->get_item_type(type_id);
    if (!def) {
        log::error(TAG, "Unknown item type '{}'", type_id);
        return {};
    }

    Handle h = world.entities.allocate();
    u32 id = h.id;

    world.transforms.add(id, Transform{{x, y, 0.0f}, 0, def->model_scale, {x, y, 0.0f}, 0});
    world.handle_infos.add(id, HandleInfo{std::string(type_id), Category::Item});
    // Defaults (radius/height = 0) = AUTO: renderer fits the click cylinder to
    // the model AABB × scale, same as units. priority is unused for items.
    world.selectables.add(id, Selectable{});
    world.item_infos.add(id, ItemInfo{std::string(type_id), def->initial_charges, def->initial_level});
    world.carriables.add(id, Carriable{});
    if (!def->model_path.empty()) {
        Renderable r{def->model_path, true};
        // Birth ("materialize") plays only for an item dropped/created in the
        // local viewer's sight — same rule units use. A preplaced item, or one
        // created out of view (or on a host with no viewer predicate), comes up
        // already Idle so it doesn't replay birth on map load / reveal.
        if (world.spawn_visible_to_viewer && !world.spawn_visible_to_viewer(x, y)) {
            r.skip_birth = true;
        }
        world.renderables.add(id, std::move(r));
    }

    Item item{h.id};

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

    Entity e = world.entities.allocate();
    u32 id = e.id;

    world.transforms.add(id, Transform{{x, y, 0.0f}, facing, def->model_scale, {x, y, 0.0f}, facing});
    world.handle_infos.add(id, HandleInfo{std::string(type_id), Category::Doodad});
    world.doodads.add(id, DoodadComp{variation});
    if (!def->models.empty()) {
        u32 idx = variation % static_cast<u32>(def->models.size());
        world.renderables.add(id, Renderable{def->models[idx], true});
    }

    Doodad d{e.id};
    return d;
}

// ── Destruction ────────────────────────────────────────────────────────────

void release_pathing_blocker(World& world, u32 id) {
    auto* blocker = world.pathing_blockers.get(id);
    if (!blocker) return;

    i32 cx = blocker->cx;
    i32 cy = blocker->cy;
    u32 width = blocker->w;
    u32 height = blocker->h;
    world.pathing_blockers.remove(id);
    if (world.unblock_pathing) world.unblock_pathing(cx, cy, width, height);
}

static void destroy_handle(World& world, Handle h) {
    if (!world.contains(h)) return;
    release_pathing_blocker(world, h.id);
    remove_all_components(world, h.id);
}

void destroy(World& world, Unit unit)         { destroy_handle(world, unit); }
void destroy(World& world, Destructable d)    { destroy_handle(world, d); }
void destroy(World& world, Item item)         { destroy_handle(world, item); }
void destroy(World& world, Doodad d)          { destroy_handle(world, d); }

bool morph_unit(World& world, Unit unit, std::string_view new_type_id) {
    if (!world.contains(unit) || !world.types) return false;
    auto* hi = world.handle_infos.get(unit.id);
    if (!hi) return false;

    const auto* new_def = world.types->get_unit_type(new_type_id);
    if (!new_def) {
        log::error(TAG, "morph_unit: unknown type '{}'", std::string(new_type_id));
        return false;
    }
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
                                         new_def->selection_height,
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

    // Combat: presence tracks the new type. Morphing into a unit with a
    // weapon adds the component; morphing into a weaponless type (e.g. a
    // unit → building) removes it so it drops out of the combat loop;
    // otherwise update fields in place and cancel any in-flight attack.
    if (new_def->weapon) {
        const auto& w = *new_def->weapon;
        auto* c = world.combats.get(id);
        if (!c) {
            world.combats.add(id, Combat{});
            c = world.combats.get(id);
        }
        c->damage           = w.damage;
        c->range            = w.attack_range;
        c->attack_cooldown  = w.attack_cooldown;
        c->dmg_time         = w.dmg_time;
        c->backsw_time      = w.backsw_time;
        c->dmg_pt           = new_def->dmg_pt;
        c->projectile       = w.projectile;
        c->acquire_range    = new_def->acquire_range;
        c->target_mask      = w.target_mask;
        c->target           = Unit{};
        c->attack_state     = AttackState::Idle;
    } else {
        world.combats.remove(id);
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
    if (!world.contains(target)) return;
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
    if (!world.contains(target)) return;
    hp = world.healths.get(target.id);
    if (!hp || hp->current <= 0) return;

    hp->current -= amount;
    if (hp->current < 0) hp->current = 0;
    if (hp->current == 0) {
        hp->killer = world.contains(source) ? source : Unit{};
    }
    if (damage_type == "attack") ++hp->hit_count;   // normal-attack flinch only (WC3 Stand Hit)

    // Fight-back: if target is idle (no order, no current target), attack the source.
    // Only fight back against enemies (different owner).
    if (is_non_null_handle(source) && world.contains(source)) {
        auto* src_owner = world.owners.get(source.id);
        auto* tgt_owner = world.owners.get(target.id);
        bool same_owner = src_owner && tgt_owner && src_owner->id == tgt_owner->id;
        if (!same_owner) {
            auto* oq = world.order_queues.get(target.id);
            auto* combat = world.combats.get(target.id);
            if (oq && combat && !oq->current && is_null_handle(combat->target)) {
                combat->target = source;
            }
        }
    }
}

void issue_order(World& world, Unit unit, Order order) {
    if (!world.contains(unit)) return;
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
        if (is_null_handle(t) || !world.contains(t)) return 0;
        auto* sf = world.status_flags.get(t.id);
        return sf ? sf->flags : 0;
    };
    auto is_hostile = [&](Unit caster, Unit target) -> bool {
        if (!world.contains(caster) || !world.contains(target)) return false;
        auto* a = world.owners.get(caster.id);
        auto* b = world.owners.get(target.id);
        if (!a || !b) return false;
        return a->id != b->id;
    };
    if (auto* atk = std::get_if<orders::Attack>(&order.payload)) {
        u32 tf = target_flags(atk->target);
        if (tf & (status::Untargetable | status::Unattackable)) return;
    } else if (auto* cast = std::get_if<orders::Cast>(&order.payload)) {
        if (is_non_null_handle(cast->target_unit)) {
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
        // The unit must actually have the ability. A Cast for an ability
        // the unit doesn't own is rejected here — WITHOUT it, the order
        // would fall through and replace oq->current, cancelling whatever
        // the unit was doing (move/attack) for a cast that can never run.
        // This bit anyone: a mis-fanned selection item-use, a Lua
        // IssueOrder(u,"cast",X) on a unit lacking X, etc.
        auto* aset = world.ability_sets.get(unit.id);
        const AbilityInstance* inst = nullptr;
        if (aset) {
            for (auto& a : aset->abilities) {
                if (a.ability_id == cast->ability_id) { inst = &a; break; }
            }
        }
        if (!inst) return;

        // Cooldown + affordability — keep the existing activity untouched.
        // WC3: an order the unit can't act on (on cooldown, or not enough
        // mana) is rejected at issue time and does NOT interrupt what the unit
        // is already doing. Both gates live here so a spammed cast order (e.g.
        // an auto-cast trigger) can't cancel the caster's current attack/move
        // when it could never proceed. (Mana is still only *spent* at the
        // effect point — this is a start gate, not the deduction.)
        if (inst->cooldown_remaining > 0) return;
        const auto* def = world.abilities ? world.abilities->get(cast->ability_id) : nullptr;
        if (def && !ability_can_afford(world, unit.id, def->level_data(inst->level).cost)) {
            return;
        }

        // Self-target rejection for casts whose filter forbids it.
        if (is_non_null_handle(cast->target_unit) && cast->target_unit.id == unit.id) {
            if (def && !def->target_filter.self_) return;
        }
    } else if (auto* mv = std::get_if<orders::Move>(&order.payload)) {
        if (is_non_null_handle(mv->target_unit) && mv->target_unit.id == unit.id) return;
    } else if (auto* atk = std::get_if<orders::Attack>(&order.payload)) {
        if (is_non_null_handle(atk->target) && atk->target.id == unit.id) return;
    }

    if (order.queued && oq->current && oq->queued.size() >= MAX_QUEUED_ORDERS) return;

    // EVENT_UNIT_ISSUED_ORDER fires here — after admission, before the
    // order takes effect on the queue. Lua may inspect / count / chain.
    if (world.on_order) {
        world.on_order(unit, order);
    }

    if (!world.contains(unit)) return;
    oq = world.order_queues.get(unit.id);
    if (!oq) return;

    if (order.queued && oq->current) {
        oq->queued.push_back(std::move(order));
    } else {
        // Fresh (non-shift) order, OR a shift-order onto an IDLE unit (no
        // current order). The idle-shift case lands here so it starts NOW —
        // in WC3, shift-queuing an idle unit executes immediately; it doesn't
        // leave the unit standing still. Only a non-shift order wipes the
        // pending queue; the idle-shift case keeps it (empty anyway).
        if (!order.queued) oq->queued.clear();
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

f32 unit_fly_height(const World& world, u32 id) {
    const auto* mov = world.movements.get(id);
    if (!mov || mov->type != MoveType::Air || !world.types) return 0.0f;
    const auto* hi = world.handle_infos.get(id);
    if (!hi) return 0.0f;
    const auto* def = world.types->get_unit_type(hi->type_id);
    return def ? def->fly_height : 0.0f;
}

f32 get_health(const World& world, Unit unit) {
    if (!world.contains(unit)) return 0;
    auto* h = world.healths.get(unit.id);
    return h ? h->current : 0;
}

void set_health(World& world, Unit unit, f32 health) {
    if (!world.contains(unit)) return;
    auto* h = world.healths.get(unit.id);
    if (!h) return;
    h->current = std::clamp(health, 0.0f, h->max);
    if (h->current > 0.0f) h->killer = {};
}

glm::vec3 get_position(const World& world, Unit unit) {
    if (!world.contains(unit)) return {};
    auto* t = world.transforms.get(unit.id);
    return t ? t->position : glm::vec3{};
}

void set_position(World& world, Unit unit, f32 x, f32 y) {
    if (!world.contains(unit)) return;
    auto* t = world.transforms.get(unit.id);
    if (t) { t->position.x = x; t->position.y = y; }
}

Player get_owner(const World& world, Unit unit) {
    if (!world.contains(unit)) return {};
    auto* o = world.owners.get(unit.id);
    return o ? *o : Player{};
}

bool is_alive(const World& world, Unit unit) {
    if (!world.contains(unit)) return false;
    auto* h = world.healths.get(unit.id);
    return h && h->current > 0;
}

bool is_dead(const World& world, Unit unit) {
    return world.contains(unit) && world.dead_states.has(unit.id);
}

bool is_building(const World& world, Unit unit) {
    return world.contains(unit) && world.buildings.has(unit.id);
}

// ── Destructable API ───────────────────────────────────────────────────────

f32 get_health(const World& world, Destructable d) {
    if (!world.contains(d)) return 0;
    auto* h = world.healths.get(d.id);
    return h ? h->current : 0;
}

void kill(World& world, Destructable d) {
    if (!world.contains(d)) return;
    auto* h = world.healths.get(d.id);
    if (h) h->current = 0;
}

// ── Item API ───────────────────────────────────────────────────────────────

i32 get_charges(const World& world, Item item) {
    if (!world.contains(item)) return 0;
    auto* info = world.item_infos.get(item.id);
    return info ? info->charges : 0;
}

void set_charges(World& world, Item item, i32 charges) {
    if (!world.contains(item)) return;
    auto* info = world.item_infos.get(item.id);
    if (info) info->charges = charges;
}

ItemClass item_class(const World& world, Item item) {
    if (!world.contains(item) || !world.types) return ItemClass::Permanent;
    const auto* info = world.item_infos.get(item.id);
    if (!info) return ItemClass::Permanent;
    const auto* def = world.types->get_item_type(info->type_id);
    return def ? def->item_class : ItemClass::Permanent;
}

i32 get_level(const World& world, Item item) {
    if (!world.contains(item)) return 0;
    auto* info = world.item_infos.get(item.id);
    return info ? info->level : 0;
}

void set_level(World& world, Item item, i32 level) {
    if (!world.contains(item)) return;
    auto* info = world.item_infos.get(item.id);
    if (info) info->level = level;
}

Unit get_item_owner(const World& world, Item item) {
    if (!world.contains(item)) return {};
    auto* c = world.carriables.get(item.id);
    return c ? c->carried_by : Unit{};
}

i32 give_item_to_unit(World& world, Unit unit, Item item) {
    if (!world.contains(unit) || !world.contains(item)) return -1;
    // A dying item (playing its death clip on the ground) can't be grabbed.
    if (world.dead_states.has(item.id)) return -1;
    // Powerups are consumed on pickup and never occupy a slot — the
    // walk-over path handles them directly. Refuse here so nothing can
    // slot one accidentally (also covers Lua's GiveItem).
    if (item_class(world, item) == ItemClass::Powerup) return -1;
    auto* inventory = world.inventories.get(unit.id);
    auto* carriable = world.carriables.get(item.id);
    if (!inventory || !carriable || is_non_null_handle(carriable->carried_by)) return -1;

    i32 slot = -1;
    for (i32 i = 0; i < static_cast<i32>(inventory->slots.size()); ++i) {
        if (is_null_handle(inventory->slots[i])) { slot = i; break; }
    }
    if (slot < 0) return -1;

    inventory->slots[slot] = item;
    carriable->carried_by = unit;

    std::vector<std::string> abilities;
    if (world.abilities && world.types) {
        const auto* info = world.item_infos.get(item.id);
        const auto* def = info ? world.types->get_item_type(info->type_id) : nullptr;
        if (def) abilities = def->abilities;
    }
    for (const auto& ability_id : abilities) {
        add_ability(world, *world.abilities, unit, ability_id, 1, item);
        if (!world.contains(unit) || !world.contains(item)) break;
    }

    if (!world.contains(unit)) {
        if (world.contains(item)) {
            if (auto* current = world.carriables.get(item.id)) current->carried_by = {};
            if (auto* renderable = world.renderables.get(item.id)) {
                renderable->visible = true;
            }
        }
        return -1;
    }
    if (!world.contains(item)) {
        if (auto* current = world.inventories.get(unit.id);
            current && slot < static_cast<i32>(current->slots.size()) &&
            current->slots[slot] == item) {
            current->slots[slot] = {};
        }
        for (const auto& ability_id : abilities) {
            remove_item_ability(world, unit, ability_id, item);
            if (!world.contains(unit)) break;
        }
        return -1;
    }

    if (auto* renderable = world.renderables.get(item.id)) {
        renderable->visible = false;
    }
    return slot;
}

bool drop_item_from_unit(World& world, Unit unit, i32 slot, glm::vec3 pos) {
    if (!world.contains(unit)) return false;
    auto* inventory = world.inventories.get(unit.id);
    if (!inventory || slot < 0 || slot >= static_cast<i32>(inventory->slots.size())) {
        return false;
    }
    Item item = inventory->slots[slot];
    if (!world.contains(item)) return false;

    auto* carriable = world.carriables.get(item.id);
    if (!carriable || carriable->carried_by != unit) return false;

    std::vector<std::string> abilities;
    if (world.types) {
        auto* info = world.item_infos.get(item.id);
        if (info) {
            if (auto* def = world.types->get_item_type(info->type_id)) {
                abilities = def->abilities;
            }
        }
    }

    inventory->slots[slot] = {};
    carriable->carried_by = {};
    if (auto* transform = world.transforms.get(item.id)) {
        transform->position = pos;
        transform->prev_position = pos;
    }
    if (auto* renderable = world.renderables.get(item.id)) renderable->visible = true;

    for (const auto& ability_id : abilities) {
        if (!world.contains(unit)) break;
        remove_item_ability(world, unit, ability_id, item);
    }
    return true;
}

void kill_item(World& world, Item item) {
    if (!world.contains(item)) return;

    // Only a visible item on the ground has anything to animate. A carried /
    // already-hidden item (or one whose death is already running) is removed
    // outright.
    auto* r = world.renderables.get(item.id);
    bool on_ground = r && r->visible;
    if (!on_ground || world.dead_states.has(item.id)) {
        destroy(world, item);
        return;
    }

    // Size the corpse window to the actual "death" clip so the item hides +
    // frees exactly when the animation ends. No clip (or no resolver, e.g. a
    // headless server) → instant destroy; there's no visual to hold for.
    f32 clip = 0.0f;
    if (world.get_clip_duration && !r->model_path.empty()) {
        clip = world.get_clip_duration(r->model_path, "death");
    }
    if (clip <= 0.0f) {
        destroy(world, item);
        return;
    }

    // Reuse the unit corpse pipeline: derive_anim_state plays Death while a
    // DeadState is present, and system_death Phase 2 hides then frees the item
    // at cleanup_delay. Per-instance durations, so the fixed unit defaults are
    // untouched. Clients learn the death via the S_STATE 0x08 flag.
    DeadState d{};
    d.corpse_duration = clip;
    d.cleanup_delay   = clip;
    world.dead_states.add(item.id, std::move(d));
}

// ── Ability API ───────────────────────────────────────────────────────────

static void populate_instance_from_def(AbilityInstance& instance,
                                       const AbilityDef* def, u32 level) {
    if (!def) return;
    auto& level_def = def->level_data(level);
    instance.active_modifiers = level_def.modifiers;
    instance.active_flags = level_def.flags;
}

static f32 default_ability_duration(const AbilityDef* def, u32 level) {
    return def ? def->level_data(level).duration : -1.0f;
}

static bool same_ability_source(const AbilitySource& source,
                                const AbilitySource& candidate) {
    if (source.value.index() != candidate.value.index()) return false;
    if (auto* item = std::get_if<ItemAbilitySource>(&source.value)) {
        return item->item == std::get<ItemAbilitySource>(candidate.value).item;
    }
    return true;
}

bool add_ability(World& world, const AbilityRegistry& reg, Unit unit,
                 std::string_view ability_id, u32 level, Item granting_item) {
    if (!world.contains(unit)) return false;
    auto* aset = world.ability_sets.get(unit.id);
    if (!aset) return false;

    const auto* def = reg.get(ability_id);
    AbilitySource source;
    source.value = is_non_null_handle(granting_item)
        ? decltype(source.value){ItemAbilitySource{granting_item}}
        : decltype(source.value){InnateAbilitySource{}};
    source.remaining_duration = default_ability_duration(def, level);

    if (def && !def->stackable) {
        for (auto& instance : aset->abilities) {
            if (instance.ability_id != ability_id) continue;

            for (auto& existing : instance.sources) {
                if (!same_ability_source(source, existing)) continue;
                existing.remaining_duration = source.remaining_duration;
                return true;
            }

            instance.sources.push_back(source);
            if (world.on_ability_added) {
                world.on_ability_added(unit, ability_id, level, source);
            }
            return true;
        }
    }

    AbilityInstance instance;
    instance.ability_id = std::string(ability_id);
    instance.level = level;
    instance.sources.push_back(source);
    populate_instance_from_def(instance, def, level);

    auto flags_snapshot = instance.active_flags;
    aset->abilities.push_back(std::move(instance));

    flag_refcount_delta(world, unit.id, flags_snapshot, +1);
    recalculate_modifiers(world, unit.id);
    if (world.on_ability_added) {
        world.on_ability_added(unit, ability_id, level, source);
    }

    log::trace(TAG, "AddAbility: unit {} + '{}' (level {})", unit.id, ability_id, level);
    return true;
}

bool remove_ability(World& world, Unit unit, std::string_view ability_id) {
    if (!world.contains(unit)) return false;
    auto* aset = world.ability_sets.get(unit.id);
    if (!aset) return false;

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
        AbilitySource source;
        source.value = InnateAbilitySource{};
        world.on_ability_removed(unit, ability_id, source, true);
    }

    log::trace(TAG, "RemoveAbility: unit {} - '{}' (all instances)", unit.id, ability_id);
    return true;
}

bool remove_item_ability(World& world, Unit unit,
                         std::string_view ability_id, Item granting_item) {
    if (!world.contains(unit)) return false;
    auto* aset = world.ability_sets.get(unit.id);
    if (!aset) return false;

    for (auto instance = aset->abilities.begin();
         instance != aset->abilities.end(); ++instance) {
        if (instance->ability_id != ability_id) continue;

        auto source = std::find_if(
            instance->sources.begin(), instance->sources.end(),
            [&](const AbilitySource& candidate) {
                auto* item = std::get_if<ItemAbilitySource>(&candidate.value);
                return item && item->item == granting_item;
            });
        if (source == instance->sources.end()) continue;

        instance->sources.erase(source);
        if (instance->sources.empty()) {
            flag_refcount_delta(world, unit.id, instance->active_flags, -1);
            aset->abilities.erase(instance);
            recalculate_modifiers(world, unit.id);
        }
        if (world.on_ability_removed) {
            AbilitySource source;
            source.value = ItemAbilitySource{granting_item};
            world.on_ability_removed(unit, ability_id, source, false);
        }
        return true;
    }
    return false;
}

bool set_ability_level(World& world, const AbilityRegistry& reg, Unit unit,
                        std::string_view ability_id, u32 level) {
    if (!world.contains(unit)) return false;
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
                           std::string_view ability_id, Unit applier, f32 duration) {
    if (!world.contains(target)) return false;
    auto* aset = world.ability_sets.get(target.id);
    if (!aset) return false;

    const auto* def = reg.get(ability_id);
    AbilitySource source;
    source.value = AppliedAbilitySource{applier};
    source.remaining_duration = duration;

    if (def && !def->stackable) {
        for (auto& instance : aset->abilities) {
            if (instance.ability_id != ability_id) continue;

            auto existing = std::find_if(
                instance.sources.begin(), instance.sources.end(),
                [](const AbilitySource& candidate) {
                    return std::holds_alternative<AppliedAbilitySource>(
                        candidate.value);
                });
            if (existing != instance.sources.end()) {
                *existing = source;
                return true;
            }

            instance.sources.push_back(source);
            if (world.on_ability_added) {
                world.on_ability_added(target, ability_id, 1, source);
            }
            return true;
        }
    }

    AbilityInstance instance;
    instance.ability_id = std::string(ability_id);
    instance.level = 1;
    instance.sources.push_back(source);
    populate_instance_from_def(instance, def, 1);

    auto flags_snapshot = instance.active_flags;
    aset->abilities.push_back(std::move(instance));

    flag_refcount_delta(world, target.id, flags_snapshot, +1);
    recalculate_modifiers(world, target.id);
    if (world.on_ability_added) {
        world.on_ability_added(target, ability_id, 1, source);
    }

    log::trace(TAG, "ApplyPassiveAbility: '{}' on unit {} (duration={:.1f}s)", ability_id, target.id, duration);
    return true;
}

bool has_ability(const World& world, Unit unit, std::string_view ability_id) {
    if (!world.contains(unit)) return false;
    auto* aset = world.ability_sets.get(unit.id);
    if (!aset) return false;
    for (auto& a : aset->abilities) {
        if (a.ability_id == ability_id) return true;
    }
    return false;
}

u32 get_ability_stack_count(const World& world, Unit unit, std::string_view ability_id) {
    if (!world.contains(unit)) return 0;
    auto* aset = world.ability_sets.get(unit.id);
    if (!aset) return 0;
    u32 count = 0;
    for (auto& a : aset->abilities) {
        if (a.ability_id == ability_id) count++;
    }
    return count;
}

u32 get_ability_level(const World& world, Unit unit, std::string_view ability_id) {
    if (!world.contains(unit)) return 0;
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
    if (!world.contains(unit)) return false;
    auto* sf = world.status_flags.get(unit.id);
    return sf && (sf->flags & flag) != 0;
}

void set_unit_status(World& world, Unit unit, u32 flag, bool on) {
    if (!world.contains(unit)) return;
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
    if (!world.contains(unit)) return;
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
        if (s == "phased")       return status::Phased;
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
                        const std::map<std::string, f32>& cost,
                        std::string* out_lacking) {
    if (cost.empty()) return true;
    for (const auto& [state_name, amount] : cost) {
        if (amount <= 0.0f) continue;
        if (state_name == "health") {
            // Health cost can never kill: require strictly more than the
            // cost so the caster stays at >= 1 HP. (No cost_can_kill /
            // suicide-cast opt-in — removed by design.)
            const auto* hp = world.healths.get(unit_id);
            if (!hp || hp->current <= amount) { if (out_lacking) *out_lacking = state_name; return false; }
            continue;
        }
        const auto* sb = world.state_blocks.get(unit_id);
        if (!sb) { if (out_lacking) *out_lacking = state_name; return false; }
        auto it = sb->states.find(state_name);
        if (it == sb->states.end() || it->second.current < amount) {
            if (out_lacking) *out_lacking = state_name;
            return false;
        }
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

    // Movement speed. Lives on Movement, not in the attribute block, so
    // it needs an explicit pass: rebuild the base from the per-unit
    // override (what SetUnitAttribute last wrote to base["move_speed"])
    // or, absent one, the type def, then re-apply move_speed (additive) /
    // move_speed_mult every recalc.
    if (auto* mov = world.movements.get(id)) {
        f32 base_speed = 0;
        if (world.types) {
            if (auto* info = world.handle_infos.get(id)) {
                if (auto* def = world.types->get_unit_type(info->type_id)) {
                    base_speed = def->move_speed;
                }
            }
        }
        if (auto* attrs = world.attribute_blocks.get(id)) {
            auto it = attrs->base.find("move_speed");
            if (it != attrs->base.end()) base_speed = it->second;
        }
        auto fit = flat.find("move_speed");
        auto pit = pct.find("move_speed");
        f32 fv = (fit != flat.end()) ? fit->second : 0.0f;
        f32 pv = (pit != pct.end())  ? pit->second  : 0.0f;
        mov->speed = std::max(0.0f, (base_speed + fv) * (1.0f + pv));
    }

    // Attack damage. Like move_speed, it lives on its own component
    // (Combat), not in the attribute block, so it needs an explicit pass:
    // rebuild the base from the per-unit override (base["damage"]) or the
    // type def, then re-apply damage (additive) / damage_mult. Without
    // this, a passive `modifiers: { damage: N }` writes into the attribute
    // block's numeric map — which the attack path never reads — and the
    // buff silently does nothing.
    if (auto* combat = world.combats.get(id)) {
        f32 base_damage = 0;
        if (world.types) {
            if (auto* info = world.handle_infos.get(id)) {
                if (auto* def = world.types->get_unit_type(info->type_id)) {
                    if (def->weapon) base_damage = def->weapon->damage;
                }
            }
        }
        if (auto* attrs = world.attribute_blocks.get(id)) {
            auto it = attrs->base.find("damage");
            if (it != attrs->base.end()) base_damage = it->second;
        }
        auto fit = flat.find("damage");
        auto pit = pct.find("damage");
        f32 fv = (fit != flat.end()) ? fit->second : 0.0f;
        f32 pv = (pit != pct.end())  ? pit->second  : 0.0f;
        combat->damage = std::max(0.0f, (base_damage + fv) * (1.0f + pv));
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
