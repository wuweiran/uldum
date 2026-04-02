#pragma once

#include "simulation/handle_types.h"
#include "simulation/handle_allocator.h"
#include "simulation/sparse_set.h"
#include "simulation/components.h"
#include "simulation/order.h"

#include <glm/vec3.hpp>

namespace uldum::simulation {

class TypeRegistry;

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
    SparseSet<HeroComp>            heroes;
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

    // Validate a typed handle
    bool validate(Handle h) const { return handles.is_valid(h); }
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
f32      get_health(const World& world, Unit unit);
void     set_health(World& world, Unit unit, f32 health);
glm::vec3 get_position(const World& world, Unit unit);
void     set_position(World& world, Unit unit, f32 x, f32 y);
Player   get_owner(const World& world, Unit unit);
bool     is_alive(const World& world, Unit unit);
bool     is_dead(const World& world, Unit unit);
bool     is_hero(const World& world, Unit unit);
bool     is_building(const World& world, Unit unit);

// ── Hero API ───────────────────────────────────────────────────────────────

void     hero_add_xp(World& world, Unit hero, u32 xp);
u32      hero_get_level(const World& world, Unit hero);

// ── Destructable API ───────────────────────────────────────────────────────

f32      get_health(const World& world, Destructable d);
void     kill(World& world, Destructable d);

// ── Item API ───────────────────────────────────────────────────────────────

i32      get_charges(const World& world, Item item);
void     set_charges(World& world, Item item, i32 charges);

} // namespace uldum::simulation
