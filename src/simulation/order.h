#pragma once

#include "simulation/handle_types.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <variant>
#include <vector>

namespace uldum::simulation {

namespace orders {
    // Unified movement order. Three flavors fall out of the same struct:
    //   • Point click (Move):     target_unit invalid, target = point,    range = 0
    //   • Smart-click ally:       target_unit = ally,  range = follow-cluster radius
    //   • Hold position-of-X:     target_unit = X,     range = stop radius
    // Termination policy:
    //   • Point + range==0  → ends on arrival OR stuck-timeout
    //   • Unit  + range>0   → never ends on arrival (keep tracking),
    //                          ends only when target dies / leaves vision
    //                          or a new order replaces it
    // Per-tick the simulation re-resolves the goal from `target_unit` when
    // it's valid, otherwise from `target`. This is what makes "Follow" a
    // free emergent behavior of the same primitive.
    struct Move          {
        glm::vec3 target;            // used when target_unit is invalid
        Unit      target_unit;       // when valid → follow this unit each tick
        f32       range = 0.0f;      // 0 = exact arrival; >0 = stop within radius
    };
    struct AttackMove    { glm::vec3 target; };
    struct Attack        { Unit target; };
    struct Stop          {};
    struct HoldPosition  {};
    struct Patrol        { std::vector<glm::vec3> waypoints; u32 current = 0; };
    struct Cast          { std::string ability_id; Unit target_unit; glm::vec3 target_pos;
                           // If the cast originated from an item slot,
                           // this carries the item handle so the
                           // simulation can surface it via the
                           // on_ability_effect callback (and Lua's
                           // GetTriggerItem). Default-constructed
                           // (invalid) for non-item casts.
                           Item source_item; };
    struct Train         { std::string unit_type_id; };
    struct Research      { std::string research_id; };
    struct Build         { std::string building_type_id; glm::vec3 pos; };
    struct PickupItem    { Item item; };
    struct DropItem      { Item item; glm::vec3 pos; };
    // Swap two inventory slots on a single carrier. No-op if either
    // index is out of range. Used by the HUD inventory composite to
    // commit drag-swap reorders through the order pipeline so MP
    // clients route the change through the host.
    struct SwapInventorySlot { i32 slot_a; i32 slot_b; };
    // Action-preset continuous directional move. `dir` is a 2D vector
    // (usually normalized; magnitude <= 1 clamps speed). The unit keeps
    // trying to move along `dir` every tick until the order is replaced
    // or cleared — no pathfinding, no destination. Collisions slide
    // axis-aligned: into a vertical wall only the Y component applies,
    // into a horizontal wall only X applies, into a corner neither.
    struct MoveDirection { glm::vec2 dir; };
}

using OrderPayload = std::variant<
    orders::Move,
    orders::AttackMove,
    orders::Attack,
    orders::Stop,
    orders::HoldPosition,
    orders::Patrol,
    orders::Cast,
    orders::Train,
    orders::Research,
    orders::Build,
    orders::PickupItem,
    orders::DropItem,
    orders::SwapInventorySlot,
    orders::MoveDirection
>;

struct Order {
    OrderPayload payload;
    bool         queued = false;
};

} // namespace uldum::simulation
