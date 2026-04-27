#include "input/command_system.h"
#include "simulation/world.h"
#include "core/log.h"

namespace uldum::input {

static constexpr const char* TAG = "Command";

void CommandSystem::submit(const GameCommand& cmd) {
    // Client mode: send to server instead of executing locally
    if (m_network_send) {
        m_network_send(cmd);
        return;
    }

    if (!m_world) return;

    // Issue order to each unit. Cast validity (target filter, range,
    // cooldown, cost) is enforced upstream by the ability system at
    // arming time, not here — submit is a "do it" call, not a "should
    // I do it" call. Ownership validation is the single guard kept
    // here because it spans player boundaries and isn't visible to
    // the upstream caller.
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

    // Post-issuance observer. Pure notification — fires once per
    // submit after all units have received the order. Map scripts
    // hook in here to react (count orders, play SFX, update HUD,
    // etc.); they cannot cancel from this hook by design.
    if (m_observer) m_observer(cmd);
}

} // namespace uldum::input
