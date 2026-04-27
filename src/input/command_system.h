#pragma once

#include "input/command.h"
#include "simulation/world.h"

#include <functional>

namespace uldum::input {

// Notification fired after a command has been issued to its targets.
// Pure observation — handlers may not cancel the command, may not mutate
// the command. Side effects (logging, scoring, SFX, state mutation) are
// allowed. Cast validity is decided by ability `target_filter` data,
// which both client and server evaluate against the same synced state;
// this hook is for "an order *was* issued" reactions.
using OrderObserver = std::function<void(const GameCommand& cmd)>;

class CommandSystem {
public:
    void init(simulation::World* world) { m_world = world; }

    // Submit a command. Validates ownership, issues orders to each
    // valid unit, then fires the observer (if set).
    void submit(const GameCommand& cmd);

    // Set a post-issuance observer (e.g., Lua's on_order hook). Only
    // one active at a time. Called once per submit, after all per-unit
    // issue_order calls have completed.
    void set_order_observer(OrderObserver observer) { m_observer = std::move(observer); }

    // If set, commands are sent to network instead of executed locally (client mode).
    using NetworkSendFn = std::function<void(const GameCommand& cmd)>;
    void set_network_send(NetworkSendFn fn) { m_network_send = std::move(fn); }

private:
    simulation::World* m_world = nullptr;
    OrderObserver m_observer;
    NetworkSendFn m_network_send;
};

} // namespace uldum::input
