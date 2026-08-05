#pragma once

#include "simulation/entity_types.h"
#include "simulation/entity_allocator.h"
#include "simulation/sparse_set.h"
#include "simulation/components.h"
#include "simulation/order.h"
#include "simulation/type_registry.h"

#include <glm/vec3.hpp>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace uldum::map { struct TerrainData; }

namespace uldum::simulation {

class AbilityRegistry;

struct World {
    // ── Component storage (handle.id-indexed) ──────────────────────────
    SparseSet<Transform>            transforms;
    SparseSet<HandleInfo>           handle_infos;
    SparseSet<Health>               healths;
    SparseSet<StateBlock>           state_blocks;
    SparseSet<AttributeBlock>       attribute_blocks;
    SparseSet<Selectable>           selectables;
    SparseSet<Player>               owners;   // unit id -> owning player id
    SparseSet<Movement>             movements;
    SparseSet<GuardPosition>        guard_positions;
    SparseSet<Pathing>              pathings; // host-only pathfinder scratch (see Pathing)
    SparseSet<Combat>               combats;
    SparseSet<Sight>                sights;
    SparseSet<OrderQueue>           order_queues;
    SparseSet<AbilitySet>           ability_sets;
    SparseSet<UnitClassificationComp> classifications;
    SparseSet<StatusFlags>          status_flags;
    SparseSet<TrueSightVisibility>  true_sight_vis;  // transient, rebuilt every tick by system_true_sight
    SparseSet<ForcedVisibility>     forced_vis;      // persistent, set by UnitReveal
    SparseSet<Inventory>            inventories;
    SparseSet<BuildingComp>         buildings;
    SparseSet<Construction>         constructions;
    SparseSet<DestructableComp>     destructables;
    SparseSet<DoodadComp>           doodads;
    SparseSet<PathingBlocker>       pathing_blockers;
    SparseSet<ItemInfo>             item_infos;
    SparseSet<Carriable>            carriables;
    SparseSet<ProjectileComp>       projectiles;
    SparseSet<Corpse>               corpses;
    SparseSet<Renderable>           renderables;
    SparseSet<AnimQueue>            anim_queues;  // script-driven animation override (Lua writes, renderer advances)

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
        // Last-tick unit ids inside this region. Diffed against the current
        // scan to derive enter / leave events.
        std::unordered_set<u32> contained;
    };
    std::unordered_map<u32, Region> regions;
    u32 next_region_id = 0;

    // Entity id allocator
    EntityAllocator entities;

    // Optional predicate: "can the local viewer see world position (x, y)
    // right now?" Set by the host/single-player app from its Vision +
    // local player; left null on the headless server and on the network
    // client. create_unit consults it to decide whether a newly-spawned
    // unit plays its birth clip — a unit born outside the viewer's sight
    // comes up already Idle (skip_birth), matching the client, whose
    // skip_birth arrives from the S_SPAWN newly_created flag. Null → birth
    // plays (nothing renders headless; the client path governs itself).
    std::function<bool(f32 x, f32 y)> spawn_visible_to_viewer;

    // Type registry (not owned — set during init)
    const TypeRegistry* types = nullptr;
    // Ability registry (not owned — set during init). Used by create_unit
    // to seed ability_set from the unit type's `abilities` list, and by
    // any helper that needs to look up an ability def from inside the
    // simulation without taking it as an argument.
    const AbilityRegistry* abilities = nullptr;

    // Terrain (not owned — set during init, on host AND client). Lets creation
    // (create_*/spawn_*_with_id) sample ground height so an entity's Z is set at
    // build time, one place, for every caller. Z is derived locally
    // from terrain, never synced. Null → Z defaults to 0 (pre-terrain spawns).
    const map::TerrainData* terrain = nullptr;

    // Damage callback — set by script engine to intercept damage events.
    // Parameters: source, target, amount (mutable), damage_type.
    // The callback may modify amount (e.g. to reduce or amplify damage).
    using DamageCallback = std::function<void(Unit source, Unit target, f32& amount, std::string_view damage_type)>;
    DamageCallback on_damage;

    // Death callback — set by script engine to fire on_death events.
    // Parameters: dying unit, killer (may be invalid if no killer).
    using DeathCallback = std::function<void(Unit dying, Unit killer)>;
    DeathCallback on_death;

    // Engine death-transition callback — fired the moment an entity enters
    // dead-state (health < threshold), for the NETWORK to signal death to clients.
    // Distinct from on_death (which is the SCRIPT trigger hook, and may
    // cancel/mutate). Needed because destructables no longer ride the per-tick
    // snapshot (only units/projectiles do), so their health→0 can't reach the
    // client that way — an on-change S_COLD{Health(0)} lets the client derive death
    // and play the death clip during the corpse window before S_DESTROY removes the
    // entity. Fires for the categories that left the per-tick path (destructables);
    // units still sync health (→ death) via the per-tick snapshot.
    using EntityDiedCallback = std::function<void(u32 entity_id)>;
    EntityDiedCallback on_entity_died;

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
    //                         interruption.
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

    // Ability lifecycle callbacks — fire when a source is added or removed.
    // Refreshing an existing source does not fire. Host mirrors source kind so
    // clients preserve item-only UI behavior and non-stackable provenance.
    using AbilityAddedCallback =
        std::function<void(Unit unit, std::string_view ability_id, u32 level,
                           const AbilitySource& source)>;
    using AbilityRemovedCallback =
        std::function<void(Unit unit, std::string_view ability_id,
                           const AbilitySource& source, bool all_instances)>;
    AbilityAddedCallback   on_ability_added;
    AbilityRemovedCallback on_ability_removed;

    // Fired when an ability's cooldown STARTS (a cast resolved and began its
    // cooldown). The host mirrors this to clients so the action-bar / item slot
    // greys out for `seconds` — clients don't re-simulate ability cooldowns
    // (only attack cadence), so without this the slot never shows the cooldown.
    using AbilityCooldownCallback =
        std::function<void(Unit unit, std::string_view ability_id, f32 seconds)>;
    AbilityCooldownCallback on_ability_cooldown_started;

    // Projectile event callbacks. `on_projectile_hit` fires per unit
    // hit (homing: once; linear: once per unit along the path).
    // `on_projectile_destroyed` fires once on every destroy path
    // (auto-destroy on homing hit, max_distance expiry, manual
    // DestroyProjectile, lifetime cap). The hit is always reported
    // BEFORE the destroyed event when both fire in the same tick.
    using ProjectileHitCallback       = std::function<void(Projectile projectile, Unit hit_unit)>;
    using ProjectileDestroyedCallback = std::function<void(Projectile projectile)>;
    ProjectileHitCallback       on_projectile_hit;
    ProjectileDestroyedCallback on_projectile_destroyed;

    // Resolve a clip's duration on a model. Installed by the renderer so
    // the simulation can size projectile death timers to the actual
    // animation length. Returns 0 if model or clip is missing; callers
    // fall back to a small default in that case.
    using ClipDurationCallback = std::function<f32(std::string_view model_path, std::string_view clip_name)>;
    ClipDurationCallback get_clip_duration;

    // Internal pathfinder bridge used when an individual blocker leaves the
    // live world. Bulk world clears reset pathfinding separately and do not
    // invoke it.
    using PathingUnblockCallback = std::function<void(i32 cx, i32 cy, u32 w, u32 h)>;
    PathingUnblockCallback unblock_pathing;

    // Item events — fired by system_items after a pickup / drop completes.
    // Map Lua hooks these via the trigger system to drive consumption,
    // drop-on-death, etc. The engine itself takes no further action.
    using ItemPickupCallback = std::function<void(Unit unit, Item item, i32 slot)>;
    using ItemDropCallback   = std::function<void(Unit unit, Item item)>;
    ItemPickupCallback on_item_picked_up;
    ItemDropCallback   on_item_dropped;

    // Fired when the engine changes an item's charge count on its own (a
    // charged item spending a charge). Lets the host push the new value to
    // clients — the per-tick entity delta doesn't carry charges. Not fired
    // for the destroy-at-0 case (S_DESTROY already syncs that) nor for Lua's
    // own SetItemCharges (which sends its own packet).
    using ItemChargesChangedCallback = std::function<void(Item item, i32 charges)>;
    ItemChargesChangedCallback on_item_charges_changed;

    // Construction events — fired by system_build. `on_construction_start`
    // when a worker reaches the site and the structure spawns (the builder
    // is passed so map Lua can decide worker fate: keep, consume, free).
    // `on_construction_finish` when build_progress reaches 1. `builder` may
    // be invalid on finish (the worker could have died / moved on). Map Lua
    // hooks these via the trigger system; the engine takes no further action.
    using ConstructionCallback =
        std::function<void(Unit structure, Unit builder)>;
    ConstructionCallback on_construction_start;
    ConstructionCallback on_construction_finish;

    // Fired when a Build order is abandoned before the structure spawns —
    // the worker walked to the site but couldn't build (no standable side,
    // or the footprint became unbuildable). Carries the worker (the structure
    // never existed) + a short reason ("blocked" / "no_space"). The app routes
    // it to the owning player's HUD error line; map Lua can also hook it.
    using ConstructionFailedCallback =
        std::function<void(Unit builder, std::string_view reason)>;
    ConstructionFailedCallback on_construction_failed;

    // Region events — fired by system_regions each tick when a unit
    // crosses into / out of a region's shape. Map Lua hooks these via
    // TriggerRegisterEnterRegion / LeaveRegion. Engine takes no
    // further action — just spatial detection + dispatch.
    using RegionEventCallback = std::function<void(u32 region_id, Unit unit)>;
    RegionEventCallback on_region_enter;
    RegionEventCallback on_region_leave;

    // Whether this entity id is currently present in the world.
    bool contains(Handle h) const {
        return is_non_null_handle(h) && handle_infos.has(h.id);
    }

    // Build a Unit handle from an id owned by current world state.
    Unit unit(u32 id) const { return Unit{id}; }
    // Build a Projectile handle from an id (projectiles are script-addressable).
    Projectile projectile(u32 id) const { return Projectile{id}; }

    // Clear all entities (keeps type registry, callbacks, and the monotonic
    // entity-id counter). If you add a component pool, add it both here and
    // in remove_all_components (world.cpp).
    void clear_entities() {
        transforms.clear(); handle_infos.clear(); healths.clear();
        state_blocks.clear(); attribute_blocks.clear(); selectables.clear();
        owners.clear(); movements.clear(); guard_positions.clear(); pathings.clear(); combats.clear(); sights.clear();
        order_queues.clear(); ability_sets.clear(); classifications.clear();
        inventories.clear(); buildings.clear();
        constructions.clear(); destructables.clear(); doodads.clear(); pathing_blockers.clear();
        item_infos.clear(); carriables.clear(); projectiles.clear();
        corpses.clear(); renderables.clear();
        status_flags.clear(); true_sight_vis.clear(); forced_vis.clear(); anim_queues.clear();
        regions.clear(); next_region_id = 0;
        entities.reset();
    }
};

// ── Creation ───────────────────────────────────────────────────────────────
// create_unit handles all unit subtypes (regular, hero, building).
// The type definition determines which extra components are attached.

Unit          create_unit(World& world, std::string_view type_id, Player owner, f32 x, f32 y, f32 facing = 0);
Destructable  create_destructable(World& world, std::string_view type_id, f32 x, f32 y, f32 facing = 0, u8 variation = 0);
Item          create_item(World& world, std::string_view type_id, f32 x, f32 y);
Doodad        create_doodad(World& world, std::string_view type_id, f32 x, f32 y, f32 facing = 0, u8 variation = 0);

// ── Network-client entity materialization ────────────────────────────────────
// The client builds entities from S_SPAWN/S_SHOW wire messages using the SAME
// construction code as the host — these entry points reserve the server-assigned
// id, then run the identical builder create_* uses. This replaces the old
// hand-copied NetworkManager::spawn_client_entity (which drifted repeatedly).
//
// SpawnOpts carries the only real host↔client deltas as DATA:
//   skip_birth — true → materialize without the birth clip (client passes
//                !newly_created; host derives from spawn_visible_to_viewer).
// (Model selection is by `variation` on the spawn_*_with_id calls, not here —
// the client resolves the model from type + variation just like the host.)
struct SpawnOpts {
    bool skip_birth = false;
};

// spawn_*_with_id: reserve `id`, then build via the shared create_* body. No-op
// (returns invalid) if `id` already exists. Type is resolved from world.types.
Unit          spawn_unit_with_id(World& world, u32 id, std::string_view type_id, Player owner, f32 x, f32 y, f32 facing, const SpawnOpts& opts);
Item          spawn_item_with_id(World& world, u32 id, std::string_view type_id, f32 x, f32 y, const SpawnOpts& opts);
// Destructables/doodads have no birth clip, so no SpawnOpts — just the variation
// that selects their model from the type's models[] list.
Doodad        spawn_doodad_with_id(World& world, u32 id, std::string_view type_id, f32 x, f32 y, f32 facing, u8 variation);
Destructable  spawn_destructable_with_id(World& world, u32 id, std::string_view type_id, f32 x, f32 y, f32 facing, u8 variation);
// Projectiles aren't type-registry entities; the client materializes one from
// the inline wire model. Minimal render-only entity (no health/combat/owner).
Projectile    spawn_projectile_with_id(World& world, u32 id, std::string_view model, f32 x, f32 y, f32 facing);

void          destroy(World& world, Unit unit);
void          destroy(World& world, Destructable d);
void          destroy(World& world, Item item);
void          destroy(World& world, Doodad d);

// Remove one entity's pathing footprint from both World and Pathfinder.
// Call this only for an individual live-world removal; bulk clears reset
// pathfinding separately.
void          release_pathing_blocker(World& world, u32 id);

// Canonical per-entity component teardown. Does not run gameplay callbacks.
void          remove_all_components(World& world, u32 id);

// Transform a unit into a different unit type in place — same handle,
// same position, same owner. Swaps every type-derived component (model,
// movement, combat, vision, classifications, etc.) and re-seeds health
// and states by percentage carry-over. The ability set is left untouched
// (map Lua manages abilities across the morph). Cancels in-flight cast /
// attack / movement. Returns false if the handle is stale or the type id
// is unknown.
bool morph_unit(World& world, Unit unit, std::string_view new_type_id);

// Render-only visual Z lift for a unit: fly_height for Air units, else 0. The
// sim position stays at ground Z; this is what the mesh renderer, selection
// ring, and click pick-test all add so the visual model, its ring, and its
// hit-volume agree.
f32 unit_fly_height(const World& world, u32 id);

// ── Unit API ───────────────────────────────────────────────────────────────

void     issue_order(World& world, Unit unit, Order order);

// Deal damage. Fires on_damage (units only). `target` is a Widget — crates take
// damage too; `source` is always a Unit.
void     deal_damage(World& world, Unit source, Widget target, f32 amount, std::string_view damage_type = "attack");

// ── Ability API ───────────────────────────────────────────────────────────

class AbilityRegistry;

// Add an innate or item-provided ability. Item sources carry the exact item
// so dropping one item only removes its own ownership source.
bool     add_ability(World& world, const AbilityRegistry& reg, Unit unit,
                     std::string_view ability_id, u32 level = 1,
                     Item granting_item = {});
// Remove every instance with this id. Item lifecycle uses the source-scoped
// helper below instead.
bool     remove_ability(World& world, Unit unit, std::string_view ability_id);
bool     remove_item_ability(World& world, Unit unit,
                             std::string_view ability_id, Item granting_item);
// Apply a passive ability from a source unit with a duration. Respects stackable flag.
bool     apply_passive_ability(World& world, const AbilityRegistry& reg, Unit target, std::string_view ability_id, Unit source, f32 duration);
bool     has_ability(const World& world, Unit unit, std::string_view ability_id);
u32      get_ability_stack_count(const World& world, Unit unit, std::string_view ability_id);
u32      get_ability_level(const World& world, Unit unit, std::string_view ability_id);
// Change the level of the first matching ability instance. Re-populates
// active modifiers / flags from the new level (with flag refcount
// bookkeeping) and re-runs `recalculate_modifiers`. Returns false when
// the unit doesn't have the ability.
bool     set_ability_level(World& world, const AbilityRegistry& reg, Unit unit,
                            std::string_view ability_id, u32 level);

// Ability cost (state cost = mana / energy / etc., plus an optional
// "health" key). `cost` maps state-name → amount. ability_can_afford
// returns true when every cost is satisfiable; health cost never
// reduces the caster below 1 HP (it is hard-capped, no suicide casts).
// When it returns false and `out_lacking` is non-null, `*out_lacking`
// is set to the first unpayable state's name (for reject messages).
// ability_pay_cost deducts; call it only after ability_can_afford
// returned true. Both no-op on an empty cost map.
bool     ability_can_afford(const World& world, u32 unit_id,
                            const std::map<std::string, f32>& cost);
void     ability_pay_cost(World& world, u32 unit_id,
                          const std::map<std::string, f32>& cost);

// Attack targeting handshake: can an attack with `target_mask` hit
// `target`? Destructables match on their widget bit; units match on the
// "Targeted As" class bit derived from their classifications (a ground-only
// attack can't hit a flyer, which is classified "air"). Shared by combat
// acquisition and the input layer's reject feedback. When it returns false and
// `out_specifier` is non-null, `*out_specifier` is set to the target's class
// name ("air"/"tree"/…) for a reject message.
bool     can_attack_target(const World& world, u8 target_mask, Widget target,
                           std::string* out_specifier = nullptr);
void     recalculate_modifiers(World& world, u32 id);

// Map a status flag name ("stunned", "no_acquire", ...) to its status::*
// bit. Returns 0 for an unknown name.
u32      parse_status_flag_name(std::string_view name);

// Increment / decrement per-flag refcounts on a unit's StatusFlags by
// `delta` for each named flag. Used by passive_flag ability lifecycle
// (add / remove / expiry). Names are status:: keys ("invisible",
// "no_acquire", etc.); unknown names are logged.
void     flag_refcount_delta(World& world, u32 id,
                             const std::vector<std::string>& flag_names,
                             i32 delta);

// ── Projectile API ────────────────────────────────────────────────────
// Two-stage: create allocates a handle + idle entity, emit configures
// path + speed + target and starts the flight. Between create and emit
// the projectile sits at the source point — Lua uses this window to
// attach triggers and side-table state.
Projectile create_projectile(World& world, Unit source, const std::string& model, glm::vec3 launch_local = glm::vec3{0.0f});
void emit_projectile_target(World& world, Projectile projectile, Widget target, f32 speed, f32 arc_height);
void emit_projectile_loc(World& world, Projectile projectile, glm::vec3 loc, f32 speed,
                         f32 hit_radius, f32 max_distance);
void destroy_projectile(World& world, Projectile projectile);
// Put a projectile into the Dying state (mark + size death_timer to its death
// clip + queue the clip), with no gameplay side effects — unlike
// destroy_projectile, which fires PROJECTILE_DESTROYED first. A network client
// calls this on S_PROJECTILE_DYING (it never runs system_projectile); the host
// reaches it via begin_destroy_projectile.
void enter_projectile_dying(World& world, u32 id);

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
// Is this handle a Unit (vs. Destructable / Item / Doodad / Projectile)?
bool     is_unit(const World& world, Handle h);

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
// Resolve an item's class from its type def. Returns Permanent if the item
// or its type is unknown (safe default).
ItemClass item_class(const World& world, Item item);

// Pickup → inventory slot transfer. Grants the item's abilities to the
// carrier, marks the Carriable, hides ground rendering, and returns the
// slot index. Returns -1 on failure (full inventory, invalid handles,
// item already carried, or a powerup — powerups never occupy a slot).
// On success, fires no event itself — the systems caller drives event
// emission.
i32      give_item_to_unit(World& world, Unit unit, Item item);
// Drop → place item at world position, remove from carrier inventory,
// revoke its abilities, restore ground rendering. Returns false if the
// item isn't currently carried by `unit`.
bool     drop_item_from_unit(World& world, Unit unit, i32 slot, glm::vec3 pos);

// Destroy an item, playing its "death" clip first if it's a visible ground
// item with a death clip (deferred via Corpse, sized to the clip; the
// corpse pipeline in system_death hides + frees it at clip end). A carried
// or already-hidden item (visible == false) is destroyed instantly — there's
// nothing on screen to animate. Use this instead of `destroy()` at any
// item-removal site that could be on the ground (powerup consume, RemoveItem).
void     kill_item(World& world, Item item);

} // namespace uldum::simulation
