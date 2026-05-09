#pragma once

#include "simulation/handle_types.h"
#include "simulation/handle_allocator.h"
#include "simulation/sparse_set.h"
#include "simulation/components.h"
#include "simulation/order.h"

#include <glm/vec3.hpp>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace uldum::simulation {

class TypeRegistry;
class AbilityRegistry;

struct World {
    // ── Component storage (handle.id-indexed) ──────────────────────────
    SparseSet<Transform>            transforms;
    SparseSet<HandleInfo>           handle_infos;
    SparseSet<Health>               healths;
    SparseSet<StateBlock>           state_blocks;
    SparseSet<AttributeBlock>       attribute_blocks;
    SparseSet<Selectable>           selectables;
    SparseSet<Owner>                owners;
    SparseSet<Movement>             movements;
    SparseSet<Combat>               combats;
    SparseSet<Vision>               visions;
    SparseSet<OrderQueue>           order_queues;
    SparseSet<AbilitySet>           ability_sets;
    SparseSet<UnitClassificationComp> classifications;
    SparseSet<Inventory>            inventories;
    SparseSet<BuildingComp>        buildings;
    SparseSet<Construction>         constructions;
    SparseSet<DestructableComp>    destructables;
    SparseSet<PathingBlocker>       pathing_blockers;
    SparseSet<ItemInfo>             item_infos;
    SparseSet<Carriable>            carriables;
    SparseSet<ProjectileComp>      projectiles;
    SparseSet<ScalePulse>          scale_pulses;
    SparseSet<DeadState>           dead_states;
    SparseSet<Renderable>           renderables;

    // Regions — Lua-authored zones used to fire enter/leave triggers.
    // Defined out of band (not per-unit), so it lives flat on World
    // instead of as a SparseSet. Scanned by `system_regions` each tick.
    struct RegionRect   { f32 x0, y0, x1, y1; };
    struct RegionCircle { f32 cx, cy, r; };
    struct Region {
        u32  id    = 0;
        bool alive = true;
        std::vector<RegionRect>   rects;
        std::vector<RegionCircle> circles;
        // Last-tick set of unit ids inside this region. Diffed against
        // the current scan to derive enter / leave events.
        std::unordered_set<u32>   contained;
    };
    std::unordered_map<u32, Region> regions;
    u32 next_region_id = 0;

    // Handle allocator
    HandleAllocator handles;

    // Type registry (not owned — set during init)
    const TypeRegistry* types = nullptr;
    // Ability registry (not owned — set during init). Used by create_unit
    // to seed ability_set from the unit type's `abilities` list, and by
    // any helper that needs to look up an ability def from inside the
    // simulation without taking it as an argument.
    const AbilityRegistry* abilities = nullptr;

    // Damage callback — set by script engine to intercept damage events.
    // Parameters: source, target, amount (mutable), damage_type.
    // The callback may modify amount (e.g. to reduce or amplify damage).
    using DamageCallback = std::function<void(Unit source, Unit target, f32& amount, std::string_view damage_type)>;
    DamageCallback on_damage;

    // Death callback — set by script engine to fire on_death events.
    // Parameters: dying unit, killer (may be invalid if no killer).
    using DeathCallback = std::function<void(Unit dying, Unit killer)>;
    DeathCallback on_death;

    // Ability effect callback — fired when a cast reaches its cast_point.
    // The script engine handles the actual effect (damage, heal, etc.).
    // `source_item` is the item the cast originated from (invalid when
    // the cast was hotkey / direct).
    using AbilityEffectCallback = std::function<void(Unit caster, std::string_view ability_id,
                                                      Unit target_unit, glm::vec3 target_pos,
                                                      Item source_item)>;
    AbilityEffectCallback on_ability_effect;

    // Sound callback — fired when a unit event needs a sound.
    // Parameters: sound path, position. Empty path = no sound.
    using SoundCallback = std::function<void(std::string_view path, glm::vec3 position)>;
    SoundCallback on_sound;

    // Called when a pathing blocker is removed (unblock runtime tiles).
    // Receives the tile rectangle (tx, ty) + (w, h) the blocker had
    // occupied; the simulation forwards it to Pathfinder::unblock_tiles.
    using PathingUnblockCallback = std::function<void(i32 tx, i32 ty, u32 w, u32 h)>;
    PathingUnblockCallback on_pathing_unblock;

    // Item events — fired by system_items after a pickup / drop completes.
    // Map Lua hooks these via the trigger system to drive consumption,
    // drop-on-death, etc. The engine itself takes no further action.
    using ItemPickupCallback = std::function<void(Unit unit, Item item, i32 slot)>;
    using ItemDropCallback   = std::function<void(Unit unit, Item item)>;
    ItemPickupCallback on_item_picked_up;
    ItemDropCallback   on_item_dropped;

    // Region events — fired by system_regions each tick when a unit
    // crosses into / out of a region's shape. Map Lua hooks these via
    // TriggerRegisterEnterRegion / LeaveRegion. Engine takes no
    // further action — just spatial detection + dispatch.
    using RegionEventCallback = std::function<void(u32 region_id, Unit unit)>;
    RegionEventCallback on_region_enter;
    RegionEventCallback on_region_leave;

    // Validate a typed handle
    bool validate(Handle h) const { return handles.is_valid(h); }

    // Clear all entities (keeps type registry and callbacks)
    void clear_entities() {
        transforms.clear(); handle_infos.clear(); healths.clear();
        state_blocks.clear(); attribute_blocks.clear(); selectables.clear();
        owners.clear(); movements.clear(); combats.clear(); visions.clear();
        order_queues.clear(); ability_sets.clear(); classifications.clear();
        inventories.clear(); buildings.clear();
        constructions.clear(); destructables.clear(); pathing_blockers.clear();
        item_infos.clear(); carriables.clear(); projectiles.clear();
        scale_pulses.clear(); dead_states.clear(); renderables.clear();
        regions.clear(); next_region_id = 0;
        handles = HandleAllocator{};
    }
};

// ── Creation ───────────────────────────────────────────────────────────────
// create_unit handles all unit subtypes (regular, hero, building).
// The type definition determines which extra components are attached.

Unit          create_unit(World& world, std::string_view type_id, Player owner, f32 x, f32 y, f32 facing = 0);
Destructable  create_destructable(World& world, std::string_view type_id, f32 x, f32 y, f32 facing = 0, u8 variation = 0);
Item          create_item(World& world, std::string_view type_id, f32 x, f32 y);

void          destroy(World& world, Unit unit);
void          destroy(World& world, Destructable d);
void          destroy(World& world, Item item);

// ── Unit API ───────────────────────────────────────────────────────────────

void     issue_order(World& world, Unit unit, Order order);

// Deal damage with type. Fires on_damage callback if set.
void     deal_damage(World& world, Unit source, Unit target, f32 amount, std::string_view damage_type = "attack");

// ── Ability API ───────────────────────────────────────────────────────────

class AbilityRegistry;

// Add an innate ability. Returns false if non-stackable and already present.
bool     add_ability(World& world, const AbilityRegistry& reg, Unit unit, std::string_view ability_id, u32 level = 1);
// Remove ability (reverts modifiers). Returns false if not found.
bool     remove_ability(World& world, Unit unit, std::string_view ability_id);
// Apply a passive ability from a source unit with a duration. Respects stackable flag.
bool     apply_passive_ability(World& world, const AbilityRegistry& reg, Unit target, std::string_view ability_id, Unit source, f32 duration);
bool     has_ability(const World& world, Unit unit, std::string_view ability_id);
u32      get_ability_stack_count(const World& world, Unit unit, std::string_view ability_id);
u32      get_ability_level(const World& world, Unit unit, std::string_view ability_id);

// Recalculate effective attributes from base + all active modifiers.
void     recalculate_modifiers(World& world, u32 id);
f32      get_health(const World& world, Unit unit);
void     set_health(World& world, Unit unit, f32 health);
glm::vec3 get_position(const World& world, Unit unit);
void     set_position(World& world, Unit unit, f32 x, f32 y);
Player   get_owner(const World& world, Unit unit);
bool     is_alive(const World& world, Unit unit);
bool     is_dead(const World& world, Unit unit);
bool     is_building(const World& world, Unit unit);

// ── Destructable API ───────────────────────────────────────────────────────

f32      get_health(const World& world, Destructable d);
void     kill(World& world, Destructable d);

// ── Item API ───────────────────────────────────────────────────────────────

i32      get_charges(const World& world, Item item);
void     set_charges(World& world, Item item, i32 charges);
i32      get_level(const World& world, Item item);
void     set_level(World& world, Item item, i32 level);
// Carrying-unit lookup. Returns invalid Unit if the item is on the
// ground (or not in any inventory).
Unit     get_item_owner(const World& world, Item item);
// Pickup → inventory slot transfer. Grants the item's abilities to the
// carrier, marks the Carriable, hides ground rendering, and returns the
// slot index. Returns -1 on failure (full inventory, invalid handles,
// item already carried). On success, fires no event itself — the
// systems caller drives event emission.
i32      give_item_to_unit(World& world, Unit unit, Item item);
// Drop → place item at world position, remove from carrier inventory,
// revoke its abilities, restore ground rendering. Returns false if the
// item isn't currently carried by `unit`.
bool     drop_item_from_unit(World& world, Unit unit, i32 slot, glm::vec3 pos);

} // namespace uldum::simulation
