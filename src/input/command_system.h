#pragma once

#include "input/command.h"
#include "simulation/world.h"

#include <functional>

namespace uldum::input {

// Callback fired before a command executes. Return false to cancel.
using OrderFilter = std::function<bool(const GameCommand& cmd)>;

class CommandSystem {
public:
    void init(simulation::World* world) { m_world = world; }

    // Submit a command. Validates ownership, fires filter, then issues orders.
    void submit(const GameCommand& cmd);

    // Set a filter (e.g., Lua on_order hook). Only one active at a time.
    void set_order_filter(OrderFilter filter) { m_filter = std::move(filter); }

private:
    simulation::World* m_world = nullptr;
    OrderFilter m_filter;
};

} // namespace uldum::input
