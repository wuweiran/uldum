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
    SparseSet<Sight>                sights;
    SparseSet<OrderQueue>           order_queues;
    SparseSet<AbilitySet>           ability_sets;
    SparseSet<UnitClassificationComp> classifications;
    SparseSet<StatusFlags>          status_flags;
    SparseSet<TrueSightVisibility>  true_sight_vis;  // transient, rebuilt every tick by system_true_sight
    SparseSet<ForcedVisibility>     forced_vis;      // persistent, set by UnitReveal
    SparseSet<Inventory>            inventories;
    SparseSet<BuildingComp>        buildings;
    SparseSet<Construction>         constructions;
    SparseSet<DestructableComp>    destructables;
    SparseSet<DoodadComp>           doodads;
    SparseSet<PathingBlocker>       pathing_blockers;
    SparseSet<ItemInfo>             item_infos;
    SparseSet<Carriable>            carriables;
    SparseSet<ProjectileComp>      projectiles;
    SparseSet<DeadState>           dead_states;
    SparseSet<Renderable>           renderables;
    SparseSet<AnimQueue>           anim_queues;  // script-driven animation override (Lua writes, renderer advances)

    // Regions — Lua-authored zones used to fire enter/leave triggers.
    // Defined out of band (not per-unit), so it lives flat on World
    // instead of as a SparseSet. Scanned by `system_regions` each tick.
    struct RegionRect   { f32 x0, y0, x1, y1; };
    struct RegionCircle { f32 cx, cy, r; };
    struct Region {
        u32  id    = 0;
        bool alive = true;
        // Editor-authored identifier. Empty for regions created at
        // runtime via CreateRegion(). GetRegion(id_str) looks regions
        // up by this field.
        std::string               id_str;
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

    // Order callback — fired by issue_order whenever an order survives
    // admission checks and is added to the unit's queue (or replaces
    // its current order). Lets triggers react to commands as they
    // arrive instead of inferring from state changes. The Order is
    // passed by const-ref so the script side can std::visit the
    // variant payload to extract target unit / point / ability id.
    using OrderCallback = std::function<void(Unit unit, const Order& order)>;
    OrderCallback on_order;

    // Dying callback — fired when a unit's HP first drops to 0, BEFORE
    // any reap or corpse-conversion runs. Handlers may heal the unit
    // back up (SetUnitHealth → Health.current > 0); the engine re-reads
    // the HP after the callback returns and, if positive, cancels the
    // death. This is the engine's only chance to intercept death for
    // Reincarnation / Phoenix Fire / Cheat Death-style mechanics —
    // on_death fires too late (the entity is already transitioning to
    // a corpse).
    using DyingCallback = std::function<void(Unit dying, Unit killer)>;
    DyingCallback on_dying;

    // Ability cast-lifecycle callbacks. All three share the same
    // signature (caster, ability id, target unit, target point, source
    // item — same context that's set at cast start). They fire at
    // different moments:
    //
    //   on_ability_channel  — channel_time > 0: fires the instant the
    //                         Channeling state begins (right after
    //                         Foreswing ends). Does NOT fire for non-
    //                         channeled abilities.
    //   on_ability_endcast  — channel_time > 0: fires when Channeling
    //                         ends, both natural completion AND
    //                         interruption. Mirrors WC3 SPELL_ENDCAST.
    //   on_ability_effect   — the spell's effect resolves. For non-
    //                         channeled abilities, fires at Foreswing
    //                         end. For channeled abilities, fires
    //                         AFTER on_ability_endcast on natural
    //                         completion only (interrupted channels
    //                         never reach effect).
    using AbilityEffectCallback = std::function<void(Unit caster, std::string_view ability_id,
                                                      Unit target_unit, glm::vec3 target_pos,
                                                      Item source_item)>;
    AbilityEffectCallback on_ability_channel;
    AbilityEffectCallback on_ability_endcast;
    AbilityEffectCallback on_ability_effect;

    // Sound callback — fired when a unit event needs a sound.
    // Parameters: sound path, position. Empty path = no sound.
    using SoundCallback = std::function<void(std::string_view path, glm::vec3 position)>;
    SoundCallback on_sound;

    // Ability lifecycle callbacks — fire from EVERY add / remove path
    // (add_ability, apply_passive_ability, remove_ability, natural
    // duration expiry in system_ability). Host wires these to broadcast
    // AbilityAdd / AbilityRemove updates so clients stay in lockstep
    // regardless of which engine path drove the change. Refresh
    // (non-stackable re-add) does NOT fire — the client's instance
    // already exists and only its duration would change.
    using AbilityAddedCallback   = std::function<void(Unit unit, std::string_view ability_id, u32 level)>;
    using AbilityRemovedCallback = std::function<void(Unit unit, std::string_view ability_id)>;
    AbilityAddedCallback   on_ability_added;
    AbilityRemovedCallback on_ability_removed;

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
        owners.clear(); movements.clear(); combats.clear(); sights.clear();
        order_queues.clear(); ability_sets.clear(); classifications.clear();
        inventories.clear(); buildings.clear();
        constructions.clear(); destructables.clear(); doodads.clear(); pathing_blockers.clear();
        item_infos.clear(); carriables.clear(); projectiles.clear();
        dead_states.clear(); renderables.clear();
        status_flags.clear(); true_sight_vis.clear(); forced_vis.clear(); anim_queues.clear();
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
Doodad        create_doodad(World& world, std::string_view type_id, f32 x, f32 y, f32 facing = 0, u8 variation = 0);

void          destroy(World& world, Unit unit);
void          destroy(World& world, Destructable d);
void          destroy(World& world, Item item);
void          destroy(World& world, Doodad d);

// Transform a unit into a different unit type in place — same handle,
// same position, same owner. Swaps every type-derived component (model,
// movement, combat, vision, classifications, etc.), re-seeds health and
// states by percentage carry-over, and rebuilds the ability set with
// the new type's abilities while *keeping* every existing instance so
// cooldowns continue to tick (an ability that's not in the new type
// just gets `available = false`). Cancels in-flight cast / attack /
// movement. Returns false if the handle is stale or the type id is
// unknown.
bool morph_unit(World& world, Unit unit, std::string_view new_type_id);

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

// Increment / decrement per-flag refcounts on a unit's StatusFlags by
// `delta` for each named flag. Used by passive_flag ability lifecycle
// (add / remove / expiry). Names are status:: keys ("invisible",
// "no_acquire", etc.); unknown names are logged.
void     flag_refcount_delta(World& world, u32 id,
                             const std::vector<std::string>& flag_names,
                             i32 delta);

// Status flag helpers. Read returns false for an invalid handle or
// when the unit has no StatusFlags component (treated as "no flags").
// set/clear lazy-add the component on first set; clear-all wipes the
// bitset but keeps the component. `flag` is a `status::*` bitmask
// value — single bit, not a combination.
bool     unit_has_status(const World& world, Unit unit, u32 flag);
void     set_unit_status(World& world, Unit unit, u32 flag, bool on);
void     clear_all_unit_status(World& world, Unit unit);
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
