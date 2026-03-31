#include "network/network.h"
#include "simulation/simulation.h"
#include "core/log.h"

namespace uldum::network {

static constexpr const char* TAG = "Network";
static bool s_first_update = true;

bool NetworkManager::init(simulation::Simulation& simulation) {
    m_simulation = &simulation;
    m_mode = Mode::Offline;
    log::info(TAG, "NetworkManager initialized (stub) — mode=Offline, ENet transport pending");
    return true;
}

void NetworkManager::shutdown() {
    m_simulation = nullptr;
    log::info(TAG, "NetworkManager shut down (stub)");
}

void NetworkManager::update() {
    if (s_first_update) {
        log::trace(TAG, "update (stub) — will poll ENet, process commands, sync state here");
        s_first_update = false;
    }
}

} // namespace uldum::network
