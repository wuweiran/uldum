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

    // If set, commands are sent to network instead of executed locally (client mode).
    using NetworkSendFn = std::function<void(const GameCommand& cmd)>;
    void set_network_send(NetworkSendFn fn) { m_network_send = std::move(fn); }

private:
    simulation::World* m_world = nullptr;
    OrderFilter m_filter;
    NetworkSendFn m_network_send;
};

} // namespace uldum::input
