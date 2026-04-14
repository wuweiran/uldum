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
    // dt = frame delta time (used for disconnect timeout countdown).
    void update(f32 dt = 0);

    // Host: broadcast S_STATE for all connected clients. Call once per sim tick.
    void host_broadcast_tick(u32 tick);

    Mode mode() const { return m_mode; }
    bool is_connected() const { return m_connected; }

    // Host: has the game started? (all expected players connected)
    bool is_game_started() const { return m_game_started; }

    // Host: set how many remote players to wait for before starting.
    void set_expected_players(u32 count) { m_expected_players = count; }

    // Host: signal game over. Broadcasts S_END to all clients.
    void host_end_game(u32 winner_id, std::string_view stats_json);

    // Client: has the game started? (S_START received)
    bool client_game_started() const { return m_game_started; }

    // Client: has the game ended? (S_END received)
    bool client_game_ended() const { return m_game_ended; }
    const EndData& client_end_data() const { return m_end_data; }

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

    // Configure reconnect behavior (call after init_host).
    void set_disconnect_timeout(f32 seconds) { m_disconnect_timeout = seconds; }
    void set_pause_on_disconnect(bool pause) { m_pause_on_disconnect = pause; }

    // Host: is the game paused due to a disconnected player?
    bool is_paused() const { return m_paused; }

    // Host: broadcast an S_UPDATE to all clients that can see this entity.
    void host_broadcast_update(u32 entity_id, std::span<const u8> update_packet);

    // ── Callbacks ───────────────────────────────────────────────────────
    std::function<void(std::string_view path, glm::vec3 pos)> on_sound;
    std::function<void(u32 player_id)> on_player_disconnected;  // player lost connection
    std::function<void(u32 player_id)> on_player_dropped;       // timeout expired, player removed

private:
    Mode m_mode = Mode::Offline;
    bool m_connected = false;
    bool m_game_started = false;
    bool m_game_ended = false;
    std::unique_ptr<Transport> m_transport;
    u32 m_map_hash = 0;
    u32 m_expected_players = 0;  // host: how many remote players to wait for
    EndData m_end_data;

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
    std::unordered_set<u32> m_prev_tick_entities;

    // Disconnected players awaiting reconnect
    struct DisconnectedPlayer {
        simulation::Player player;
        std::unordered_set<u32> known_entities;  // preserved from PeerInfo
        f32 timer = 0;                           // seconds remaining
    };
    std::vector<DisconnectedPlayer> m_disconnected;
    f32 m_disconnect_timeout = 60.0f;
    bool m_pause_on_disconnect = false;
    bool m_paused = false;

    void host_on_connect(u32 peer_id);
    void host_on_disconnect(u32 peer_id);
    void host_on_receive(u32 peer_id, std::span<const u8> data);
    void host_send_spawn_burst(PeerInfo& peer);
    void host_update_disconnected(f32 dt);
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
    void client_handle_update(std::span<const u8> data);
    void client_apply_interpolation();

    void spawn_client_entity(u32 entity_id, std::string_view type_id,
                             u8 owner, f32 x, f32 y, f32 facing,
                             bool newly_created);
    void destroy_client_entity(u32 entity_id);
};

} // namespace uldum::network
