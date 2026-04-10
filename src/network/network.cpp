#include "network/network.h"
#include "simulation/simulation.h"
#include "core/log.h"

namespace uldum::network {

static constexpr const char* TAG = "Network";
bool NetworkManager::init(simulation::Simulation& simulation) {
    m_simulation = &simulation;
    m_mode = Mode::Offline;
    log::info(TAG, "NetworkManager initialized — mode=Offline");
    return true;
}

void NetworkManager::shutdown() {
    m_simulation = nullptr;
}

void NetworkManager::update() {
}

} // namespace uldum::network
