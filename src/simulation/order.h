#pragma once

#include "simulation/handle_types.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <variant>
#include <vector>

namespace uldum::simulation {

namespace orders {
    struct Move          { glm::vec3 target; };
    struct AttackMove    { glm::vec3 target; };
    struct Attack        { Unit target; };
    struct Stop          {};
    struct HoldPosition  {};
    struct Patrol        { std::vector<glm::vec3> waypoints; u32 current = 0; };
    struct Cast          { std::string ability_id; Unit target_unit; glm::vec3 target_pos; };
    struct Train         { std::string unit_type_id; };
    struct Research      { std::string research_id; };
    struct Build         { std::string building_type_id; glm::vec3 pos; };
    struct PickupItem    { Item item; };
    struct DropItem      { Item item; glm::vec3 pos; };
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
    orders::MoveDirection
>;

struct Order {
    OrderPayload payload;
    bool         queued = false;
};

} // namespace uldum::simulation
