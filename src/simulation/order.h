#pragma once

#include "simulation/entity_types.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <variant>
#include <vector>

namespace uldum::simulation {

namespace orders {
    // Move / Follow. target_widget invalid → move to `target` point. Valid →
    // follow it while visible (host refreshes `target` to its live pos); on
    // fog/removal, seek that last-seen point and end on arrival (anti-leak:
    // never read a hidden entity's live transform). range: 0 = exact arrival.
    struct Move          {
        glm::vec3 target;
        Widget    target_widget;
        f32       range = 0.0f;
    };
    // Attack toward a point, preferring a widget — sibling of Move. Same
    // last-seen seek on fog/death. target_widget is a Widget (a crate is a
    // first-class attack target).
    struct Attack {
        glm::vec3 target{0.0f};
        Widget    target_widget;
        Attack() = default;
        Attack(glm::vec3 pt, Widget w = {}) : target(pt), target_widget(w) {}
        explicit Attack(Widget w) : target_widget(w) {}
    };
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
