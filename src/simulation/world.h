#pragma once

#include "simulation/handle_types.h"
#include "simulation/handle_allocator.h"
#include "simulation/sparse_set.h"
#include "simulation/components.h"
#include "simulation/order.h"

#include <glm/vec3.hpp>
#include <functional>
#include <string_view>

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
    using AbilityEffectCallback = std::function<void(Unit caster, std::string_view ability_id,
                                                      Unit target_unit, glm::vec3 target_pos)>;
    AbilityEffectCallback on_ability_effect;

    // Sound callback — fired when a unit event needs a sound.
    // Parameters: sound path, position. Empty path = no sound.
    using SoundCallback = std::function<void(std::string_view path, glm::vec3 position)>;
    SoundCallback on_sound;

    // Called when a pathing blocker is removed (unblock runtime vertices).
    using PathingUnblockCallback = std::function<void(const std::vector<glm::ivec2>& verts)>;
    PathingUnblockCallback on_pathing_unblock;

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

} // namespace uldum::simulation
