#include "input/command_system.h"
#include "simulation/world.h"
#include "core/log.h"

namespace uldum::input {

static constexpr const char* TAG = "Command";

void CommandSystem::submit(const GameCommand& cmd) {
    if (!m_world) return;

    // Filter: let Lua (or other hooks) cancel the command
    if (m_filter && !m_filter(cmd)) return;

    // Issue order to each unit
    for (auto& unit : cmd.units) {
        if (!m_world->validate(unit)) continue;

        // Validate ownership: unit must belong to the commanding player
        auto* owner = m_world->owners.get(unit.id);
        if (!owner || owner->player.id != cmd.player.id) continue;

        simulation::Order order;
        order.payload = cmd.order;
        order.queued  = cmd.queued;
        simulation::issue_order(*m_world, unit, std::move(order));
    }
}

} // namespace uldum::input
