#pragma once

#include "simulation/handle_types.h"

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
    orders::DropItem
>;

struct Order {
    OrderPayload payload;
    bool         queued = false;
};

} // namespace uldum::simulation
