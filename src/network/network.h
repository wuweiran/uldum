#pragma once

#include <string_view>

namespace uldum::simulation { class Simulation; }

namespace uldum::network {

enum class Mode {
    Offline,       // Single player — local in-process server
    Host,          // Multiplayer — this instance hosts
    Client,        // Multiplayer — connected to a remote host
    DedicatedServer,
};

class NetworkManager {
public:
    bool init(simulation::Simulation& simulation);
    void shutdown();
    void update();

    Mode mode() const { return m_mode; }

    // Future API:
    // bool host(u16 port, u32 max_players);
    // bool connect(std::string_view address, u16 port);
    // void disconnect();
    // void send_command(const PlayerCommand& cmd);
    // void broadcast_state_delta(const StateDelta& delta);

private:
    simulation::Simulation* m_simulation = nullptr;
    Mode m_mode = Mode::Offline;
};

} // namespace uldum::network
