#pragma once

#include "network/protocol.h"
#include "simulation/world.h"
#include "simulation/fog_of_war.h"
#include "simulation/handle_types.h"
#include "input/command.h"

#include <glm/vec3.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace uldum::simulation { class Simulation; class TypeRegistry; }
namespace uldum::input { class CommandSystem; }
namespace uldum::map { class MapManager; }

namespace uldum::network {

class Transport;

enum class Mode {
    Offline,          // Single player — local in-process server
    Host,             // Multiplayer — this instance hosts the server
    Client,           // Multiplayer — connected to a remote host
};

class NetworkManager {
public:
    NetworkManager();
    ~NetworkManager();

    // ── Initialization (pick one) ────────────────────────────────────────
    bool init_offline();
    bool init_host(u16 port, u32 max_players,
                   simulation::Simulation& simulation,
                   input::CommandSystem& commands);
    bool init_client(std::string_view address, u16 port);

    void shutdown();

    // Called every frame: polls transport, processes incoming messages.
    void update();

    // Host: broadcast S_STATE for all connected clients. Call once per sim tick.
    void host_broadcast_tick(u32 tick);

    Mode mode() const { return m_mode; }
    bool is_connected() const { return m_connected; }

    // ── Client API ──────────────────────────────────────────────────────
    // Send a command to the server (called instead of local CommandSystem).
    void send_order(const input::GameCommand& cmd);

    // The World populated from network snapshots (for rendering).
    simulation::World& client_world() { return m_client_world; }

    // Assigned player ID (valid after S_WELCOME).
    simulation::Player local_player() const { return m_local_player; }

    // Set the type registry for spawning entities on the client.
    void set_type_registry(const simulation::TypeRegistry* types) { m_client_types = types; }

    // Initialize client-side fog of war from map data.
    void init_client_fog(const map::TerrainData& terrain, const map::MapManager& map,
                         const simulation::Simulation& sim);

    // Update client fog of war visuals (call per frame). Returns visual grid or nullptr.
    const f32* update_client_fog(f32 dt);
    simulation::FogOfWar& client_fog() { return m_client_fog; }

    // Set the map hash for join verification.
    void set_map_hash(u32 hash) { m_map_hash = hash; }

    // ── Callbacks ───────────────────────────────────────────────────────
    std::function<void(std::string_view path, glm::vec3 pos)> on_sound;

private:
    Mode m_mode = Mode::Offline;
    bool m_connected = false;
    std::unique_ptr<Transport> m_transport;
    u32 m_map_hash = 0;

    // ── Host-side ───────────────────────────────────────────────────────
    simulation::Simulation* m_simulation = nullptr;
    input::CommandSystem* m_commands = nullptr;

    struct PeerInfo {
        u32 peer_id;
        simulation::Player player;
        std::unordered_set<u32> known_entities;
    };
    std::vector<PeerInfo> m_peers;
    u32 m_next_player_slot = 1;  // 0 = host local player

    void host_on_connect(u32 peer_id);
    void host_on_disconnect(u32 peer_id);
    void host_on_receive(u32 peer_id, std::span<const u8> data);
    void host_send_spawn_burst(PeerInfo& peer);
    bool is_visible_to(u32 entity_id, simulation::Player player) const;

    // ── Client-side ─────────────────────────────────────────────────────
    simulation::World m_client_world;
    simulation::Player m_local_player{UINT32_MAX};
    const simulation::TypeRegistry* m_client_types = nullptr;

    // Fog of war (client computes locally from received entities)
    simulation::FogOfWar m_client_fog;
    const simulation::Simulation* m_client_sim_ref = nullptr;  // for shared vision queries

    // Snapshot buffer for interpolation (two most recent)
    struct Snapshot {
        u32 tick = 0;
        f64 receive_time = 0;
        std::vector<EntityState> entities;
    };
    Snapshot m_snapshots[2];
    u32 m_snap_idx = 0;      // write index (flips between 0 and 1)
    bool m_has_two_snaps = false;

    void client_on_connect(u32 peer_id);
    void client_on_disconnect(u32 peer_id);
    void client_on_receive(u32 peer_id, std::span<const u8> data);
    void client_handle_welcome(std::span<const u8> data);
    void client_handle_spawn(std::span<const u8> data);
    void client_handle_destroy(std::span<const u8> data);
    void client_handle_state(std::span<const u8> data);
    void client_handle_sound(std::span<const u8> data);
    void client_apply_interpolation();

    void spawn_client_entity(u32 entity_id, std::string_view type_id,
                             u8 owner, f32 x, f32 y, f32 facing);
    void destroy_client_entity(u32 entity_id);
};

} // namespace uldum::network
