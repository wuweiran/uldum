#pragma once

#include "simulation/handle_types.h"
#include "simulation/order.h"

#include <vector>

namespace uldum::input {

// A GameCommand represents a player's intent: "player X orders units Y to do Z."
// All player interaction with the simulation flows through commands.
struct GameCommand {
    simulation::Player      player;
    std::vector<simulation::Unit> units;   // units to receive the order
    simulation::OrderPayload order;
    bool                    queued = false; // shift-queued
};

} // namespace uldum::input
