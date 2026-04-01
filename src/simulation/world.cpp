#include "simulation/world.h"
#include "simulation/type_registry.h"
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
    world.heroes.remove(id);
    world.inventories.remove(id);
    world.buildings.remove(id);
    world.constructions.remove(id);
    world.destructables.remove(id);
    world.pathing_blockers.remove(id);
    world.item_infos.remove(id);
    world.carriables.remove(id);
    world.projectiles.remove(id);
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
    world.transforms.add(id, Transform{{x, y, 0.0f}, facing, 1.0f});
    world.handle_infos.add(id, HandleInfo{std::string(type_id), Category::Unit, h.generation});

    // Widget — HP is engine built-in
    world.healths.add(id, Health{def->max_health, def->max_health, def->health_regen});
    world.selectables.add(id, Selectable{def->selection_radius, def->selection_priority});

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
    world.movements.add(id, Movement{def->move_speed, def->turn_rate, def->move_type, {}, false});
    world.combats.add(id, Combat{def->damage, def->attack_range, def->attack_cooldown, 0, {}});
    world.visions.add(id, Vision{def->sight_range_day, def->sight_range_night});
    world.order_queues.add(id, OrderQueue{});
    world.ability_sets.add(id, AbilitySet{});
    world.classifications.add(id, UnitClassificationComp{def->classifications});

    // Renderable
    if (!def->model_path.empty()) {
        world.renderables.add(id, Renderable{def->model_path, true});
    }

    // Hero (if type has "hero" classification)
    if (has_classification(def->classifications, "hero")) {
        HeroComp hero;
        hero.primary_attr    = def->hero_primary_attr;
        hero.attr_per_level  = def->hero_attr_per_level;
        world.heroes.add(id, std::move(hero));
        world.inventories.add(id, Inventory{});
    }

    // Building (if type has "structure" classification)
    if (has_classification(def->classifications, "structure")) {
        world.buildings.add(id, BuildingComp{});
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

    world.transforms.add(id, Transform{{x, y, 0.0f}, facing, 1.0f});
    world.handle_infos.add(id, HandleInfo{std::string(type_id), Category::Destructable, h.generation});
    world.healths.add(id, Health{def->max_health, def->max_health, 0});
    world.selectables.add(id, Selectable{1.0f, 1});
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

    world.transforms.add(id, Transform{{x, y, 0.0f}, 0, 1.0f});
    world.handle_infos.add(id, HandleInfo{std::string(type_id), Category::Item, h.generation});
    world.healths.add(id, Health{1, 1, 0});
    world.selectables.add(id, Selectable{0.5f, 1});
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

void issue_order(World& world, Unit unit, Order order) {
    if (!world.validate(unit)) return;
    auto* oq = world.order_queues.get(unit.id);
    if (!oq) return;

    if (order.queued) {
        oq->queued.push_back(std::move(order));
    } else {
        oq->queued.clear();
        oq->current = std::move(order);
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

bool is_hero(const World& world, Unit unit) {
    return world.validate(unit) && world.heroes.has(unit.id);
}

bool is_building(const World& world, Unit unit) {
    return world.validate(unit) && world.buildings.has(unit.id);
}

// ── Hero API ───────────────────────────────────────────────────────────────

void hero_add_xp(World& world, Unit hero, u32 xp) {
    if (!world.validate(hero)) return;
    auto* h = world.heroes.get(hero.id);
    if (!h) return;

    auto* attrs = world.attribute_blocks.get(hero.id);

    h->xp += xp;
    while (h->xp >= h->xp_to_next) {
        h->xp -= h->xp_to_next;
        h->level++;
        // Apply per-level attribute growth
        if (attrs) {
            for (auto& [attr, growth] : h->attr_per_level) {
                attrs->numeric[attr] += growth;
            }
        }
        h->xp_to_next = h->level * 200;
        log::info(TAG, "Hero leveled up to {}", h->level);
    }
}

u32 hero_get_level(const World& world, Unit hero) {
    if (!world.validate(hero)) return 0;
    auto* h = world.heroes.get(hero.id);
    return h ? h->level : 0;
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

} // namespace uldum::simulation
