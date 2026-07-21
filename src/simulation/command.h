#pragma once

#include "simulation/entity_types.h"
#include "simulation/order.h"

#include <vector>

namespace uldum::simulation {

// A GameCommand represents a player's intent: "player X orders units Y
// to do Z." All player interaction with the simulation flows through
// commands. The submitter is the only authority on which player issued
// the order; CommandSystem validates that the player owns the units
// before forwarding to the simulation.
struct GameCommand {
    Player              player;
    std::vector<Unit>   units;   // units to receive the order
    OrderPayload        order;
    bool                queued = false; // shift-queued
};

} // namespace uldum::simulation
