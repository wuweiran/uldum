#include "simulation/world.h"
#include "simulation/type_registry.h"
#include "simulation/ability_def.h"
#include "core/log.h"

namespace uldum::simulation {

static constexpr const char* TAG = "World";

// ── Helper: remove all components for a handle ID ──────────────────────────

static void remove_all_components(World& world, u32 id) {
    world.transforms.remove(id);
    world.handle_infos.remove(id);
    world.healths.remove(id);
    world.state_blocks.remove(id);
    world.attribute_blocks.remove(id);
    world.selectables.remove(id);
    world.owners.remove(id);
    world.movements.remove(id);
    world.combats.remove(id);
    world.visions.remove(id);
    world.order_queues.remove(id);
    world.ability_sets.remove(id);
    world.classifications.remove(id);
    world.inventories.remove(id);
    world.buildings.remove(id);
    world.constructions.remove(id);
    world.destructables.remove(id);
    world.pathing_blockers.remove(id);
    world.item_infos.remove(id);
    world.carriables.remove(id);
    world.projectiles.remove(id);
    world.scale_pulses.remove(id);
    world.dead_states.remove(id);
    world.renderables.remove(id);
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
        ab.numeric = def->attributes_numeric;
        ab.string_attrs = def->attributes_string;
        world.attribute_blocks.add(id, std::move(ab));
    }

    // Unit
    world.owners.add(id, Owner{owner});
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
    world.visions.add(id, Vision{def->sight_range_day, def->sight_range_night});
    world.order_queues.add(id, OrderQueue{});
    world.ability_sets.add(id, AbilitySet{});
    world.classifications.add(id, UnitClassificationComp{def->classifications});

    // Renderable
    if (!def->model_path.empty()) {
        world.renderables.add(id, Renderable{def->model_path, true});
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

    world.transforms.add(id, Transform{{x, y, 0.0f}, facing, 1.0f, {x, y, 0.0f}, facing});
    world.handle_infos.add(id, HandleInfo{std::string(type_id), Category::Destructable, h.generation});
    world.healths.add(id, Health{def->max_health, def->max_health, 0});
    world.selectables.add(id, Selectable{1.0f, 50.0f, 1});
    world.destructables.add(id, DestructableComp{std::string(type_id), variation});

    if (!def->attributes_numeric.empty() || !def->attributes_string.empty()) {
        AttributeBlock ab;
        ab.numeric = def->attributes_numeric;
        ab.string_attrs = def->attributes_string;
        world.attribute_blocks.add(id, std::move(ab));
    }

    if (!def->model_path.empty()) {
        world.renderables.add(id, Renderable{def->model_path, true});
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

    world.transforms.add(id, Transform{{x, y, 0.0f}, 0, 1.0f, {x, y, 0.0f}, 0});
    world.handle_infos.add(id, HandleInfo{std::string(type_id), Category::Item, h.generation});
    world.healths.add(id, Health{1, 1, 0});
    world.selectables.add(id, Selectable{0.5f, 20.0f, 1});
    world.item_infos.add(id, ItemInfo{std::string(type_id), def->charges, def->cooldown, 0});
    world.carriables.add(id, Carriable{});

    Item item;
    item.id = h.id;
    item.generation = h.generation;

    log::trace(TAG, "Created item '{}' (id={})", type_id, id);
    return item;
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

// ── Unit API ───────────────────────────────────────────────────────────────

void deal_damage(World& world, Unit source, Unit target, f32 amount, std::string_view damage_type) {
    auto* hp = world.healths.get(target.id);
    if (!hp || hp->current <= 0) return;

    // Let script engine intercept (fire on_damage event, may modify amount)
    if (world.on_damage) {
        world.on_damage(source, target, amount, damage_type);
    }

    if (amount <= 0) return;

    hp->current -= amount;
    if (hp->current < 0) hp->current = 0;

    // Fight-back: if target is idle (no order, no current target), attack the source
    if (source.is_valid() && world.validate(source)) {
        auto* oq = world.order_queues.get(target.id);
        auto* combat = world.combats.get(target.id);
        if (oq && combat && !oq->current && !combat->target.is_valid()) {
            combat->target = source;
        }
    }
}

void issue_order(World& world, Unit unit, Order order) {
    if (!world.validate(unit)) return;
    auto* oq = world.order_queues.get(unit.id);
    if (!oq) return;

    if (order.queued) {
        oq->queued.push_back(std::move(order));
    } else {
        oq->queued.clear();
        oq->current = std::move(order);

        // Clear pathfinding + approach state so the movement system re-paths immediately
        auto* mov = world.movements.get(unit.id);
        if (mov) {
            mov->corridor.clear();
            mov->has_waypoint = false;
            mov->approach_target = Unit{};
            mov->approach_range = 0;
        }

        // Clear combat target so the unit stops fighting and obeys the new order
        auto* combat = world.combats.get(unit.id);
        if (combat) {
            combat->target = Unit{};
            combat->attack_state = AttackState::Idle;
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
    return o ? o->player : Player{};
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

// ── Ability API ───────────────────────────────────────────────────────────

bool add_ability(World& world, const AbilityRegistry& reg, Unit unit, std::string_view ability_id, u32 level) {
    if (!world.validate(unit)) return false;
    auto* aset = world.ability_sets.get(unit.id);
    if (!aset) return false;

    const auto* def = reg.get(ability_id);

    // Non-stackable: check if already present
    if (def && !def->stackable) {
        for (auto& a : aset->abilities) {
            if (a.ability_id == ability_id) {
                return false;  // already has it
            }
        }
    }

    AbilityInstance inst;
    inst.ability_id = std::string(ability_id);
    inst.level = level;
    inst.remaining_duration = -1.0f;  // permanent (innate)

    // Apply modifiers from def
    if (def) {
        auto& lvl = def->level_data(level);
        inst.active_modifiers = lvl.modifiers;
    }

    aset->abilities.push_back(std::move(inst));
    recalculate_modifiers(world, unit.id);

    log::trace(TAG, "AddAbility: unit {} + '{}' (level {})", unit.id, ability_id, level);
    return true;
}

bool remove_ability(World& world, Unit unit, std::string_view ability_id) {
    if (!world.validate(unit)) return false;
    auto* aset = world.ability_sets.get(unit.id);
    if (!aset) return false;

    auto it = std::find_if(aset->abilities.begin(), aset->abilities.end(),
        [&](const AbilityInstance& a) { return a.ability_id == ability_id; });

    if (it == aset->abilities.end()) return false;

    aset->abilities.erase(it);
    recalculate_modifiers(world, unit.id);

    log::trace(TAG, "RemoveAbility: unit {} - '{}'", unit.id, ability_id);
    return true;
}

bool apply_passive_ability(World& world, const AbilityRegistry& reg, Unit target,
                           std::string_view ability_id, Unit source, f32 duration) {
    if (!world.validate(target)) return false;
    auto* aset = world.ability_sets.get(target.id);
    if (!aset) return false;

    const auto* def = reg.get(ability_id);

    // Non-stackable: refresh duration if already present
    if (def && !def->stackable) {
        for (auto& a : aset->abilities) {
            if (a.ability_id == ability_id) {
                a.remaining_duration = duration;
                a.source = source;
                return true;  // refreshed
            }
        }
    }

    AbilityInstance inst;
    inst.ability_id = std::string(ability_id);
    inst.level = 1;
    inst.source = source;
    inst.remaining_duration = duration;

    if (def) {
        auto& lvl = def->level_data(1);
        inst.active_modifiers = lvl.modifiers;
    }

    aset->abilities.push_back(std::move(inst));
    recalculate_modifiers(world, target.id);

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

void recalculate_modifiers(World& world, u32 id) {
    auto* aset = world.ability_sets.get(id);
    auto* attrs = world.attribute_blocks.get(id);
    if (!aset || !attrs) return;

    // Note: we only recalculate modifier-affected attributes here.
    // Base attributes are set at entity creation; modifiers are additive.
    // For now, we store the sum of all modifiers in a separate map
    // that systems can query. Full base+modifier calculation happens
    // when systems read effective values.

    // This is a placeholder — the full modifier system will be needed
    // when Lua scripts start applying buffs with modifiers.
}

} // namespace uldum::simulation
