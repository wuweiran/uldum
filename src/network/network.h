#pragma once

#include "network/protocol.h"
#include "simulation/world.h"
#include "simulation/vision.h"
#include "simulation/handle_types.h"
#include "input/command.h"

#include <glm/vec3.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace uldum::simulation { class Simulation; class TypeRegistry; class AbilityRegistry; }
namespace uldum::input  { class CommandSystem; }
namespace uldum::map    { class MapManager; }
namespace uldum::hud    { class Hud; }
namespace uldum::script { class ScriptEngine; }

namespace uldum::network {

class Transport;

enum class Mode {
    Offline,          // Single player — local in-process server
    Host,             // Multiplayer — this instance hosts the server
    Client,           // Multiplayer — connected to a remote host
};

// Lifecycle phase. A networked session goes Lobby → Loading → Playing:
//   Lobby    peers connecting, claiming slots. Only manifest is loaded.
//   Loading  host committed start; every peer is loading map content
//            (terrain, preplaced units, renderer setup). No sim ticking yet.
//   Playing  S_START broadcast; sim is ticking.
// Offline sessions skip Lobby + Loading's handshake (still do the load work,
// just no sync).
enum class Phase {
    None,
    Lobby,
    Loading,
    Playing,
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
    Phase phase() const { return m_phase; }

    // Host: has the game started? (all expected players connected)
    bool is_game_started() const { return m_game_started; }

    // Host: signal game over. Broadcasts S_END to all clients.
    void host_end_game(u32 winner_id, std::string_view stats_json);

    // This process's player name. Carried in C_JOIN so the host can label
    // the peer's lobby row, and surfaced to Lua via GetPlayerName(). Set
    // before init_client() / init_host(); immutable afterward.
    void set_player_name(std::string_view name) { m_player_name = std::string{name}; }
    const std::string& player_name() const { return m_player_name; }

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

    // Set the ability registry so the client's S_UPDATE handlers for
    // AbilityAdd / AbilityRemove can populate modifiers + flags and
    // run recalculate_modifiers — exactly what the server-side
    // simulation::add_ability path does.
    void set_ability_registry(const simulation::AbilityRegistry* abilities) {
        m_client_abilities = abilities;
    }

    // HUD sync plumbing (16c-v).
    // - On host: set_hud() + set_script() install handlers so client-side
    //   C_NODE_EVENT can fire server-side triggers, and host_hud_sync can
    //   route outgoing state deltas.
    // - On client: set_hud() installs the target HUD that S_HUD_* messages
    //   apply to locally.
    void set_hud(hud::Hud* hud)                 { m_hud = hud; }
    void set_script(script::ScriptEngine* scr)  { m_script = scr; }

    // Host: route a packet built by Hud's sync_fn to the matching peer(s).
    // `players_mask` is a bitmask of player ids that should receive the
    // packet (UINT32_MAX = broadcast).
    void host_hud_sync(const std::vector<u8>& packet, u32 players_mask);

    // Client: report a HUD node event (button press, etc.) to the host.
    void send_node_event(std::string_view node_id, NodeEventKind kind);


    // Initialize client-side fog of war from map data.
    void init_client_fog(const map::TerrainData& terrain, const map::MapManager& map,
                         const simulation::Simulation& sim);

    // Update client fog of war visuals (call per frame). Returns visual grid or nullptr.
    const f32* update_client_fog(f32 dt);
    simulation::Vision& client_vision() { return m_client_vision; }

    // Set the map hash for join verification.
    void set_map_hash(u32 hash) { m_map_hash = hash; }

    // Configure reconnect behavior (call after init_host).
    void set_disconnect_timeout(f32 seconds) { m_disconnect_timeout = seconds; }
    void set_pause_on_disconnect(bool pause) { m_pause_on_disconnect = pause; }

    // Host: is the game paused due to a disconnected player?
    bool is_paused() const { return m_paused; }

    // Snapshot of the waiting-for-reconnect queue. On the host this is
    // derived from m_disconnected each frame; on the client it's the
    // most recent S_PAUSE_STATE broadcast. UIs read this for the
    // "Player X disconnected, Ns remaining" dialog.
    const std::vector<DisconnectedView>& disconnected_view() const { return m_disconnected_view; }
    bool pause_view_active() const { return m_pause_view_active; }

    // Host: broadcast an S_UPDATE to all clients that can see this entity.
    void host_broadcast_update(u32 entity_id, std::span<const u8> update_packet);

    // Host: broadcast an arbitrary packet to every connected peer.
    // Used for non-entity-scoped sync (free-position PlayEffect, future
    // global notifications). No visibility filtering.
    void host_broadcast(std::span<const u8> packet);

    // Host: send to the peer claiming a specific player slot. No-op if
    // no such peer exists (e.g. the host's own player). Used by the
    // fog-aware effect dispatcher to push deliveries per-player.
    void host_send_to_player(u32 player_id, std::span<const u8> packet);

    // ── Lobby API ───────────────────────────────────────────────────────
    // Both sides read/write `lobby_state()`; host is authoritative.
    //
    //   Host: set_lobby_state() publishes a new snapshot (broadcasts). Call
    //     after local edits (claim/release/occupant change).
    //   Client: send_claim_slot / send_release_slot / send_set_slot_occupant
    //     asks the host to change the state; server applies and broadcasts.
    //
    // on_lobby_state_changed fires whenever lobby_state() is updated from
    // the wire (client: any change; host: only on own apply, but typically
    // the caller mutates then pushes).
    LobbyState&       lobby_state()       { return m_lobby; }
    const LobbyState& lobby_state() const { return m_lobby; }
    void host_broadcast_lobby_state();

    // Client: this client's peer-id as known to the host. Valid once the
    // host has sent S_LOBBY_ASSIGN. Before that, returns UINT32_MAX.
    u32 client_peer_id() const { return m_client_peer_id; }

    void send_claim_slot(u32 slot);
    void send_release_slot(u32 slot);

    // Host: lobby is set up and stable — broadcast S_LOBBY_COMMIT, flip
    // phase → Loading. Host and all peers now load map content in parallel;
    // each one acks with C_LOAD_DONE. When everyone is loaded, the host
    // calls `host_finish_start()` to send S_WELCOME + S_SPAWN burst + S_START.
    // Must be called after `lobby_ready_to_start(lobby_state())`.
    void host_commit_start();

    // Host: every peer (self + remotes) has finished loading — broadcast
    // S_WELCOME + S_SPAWN burst per peer, then S_START. Phase → Playing.
    void host_finish_start();

    // Host: enter the scene-switch barrier. Resets self_loaded + every
    // peer's loaded flag, sets phase → Loading, and broadcasts
    // S_SCENE_SWITCH(name) so each client tears down its scene state
    // and acks via the existing C_LOAD_DONE path. Caller is responsible
    // for the host's own local teardown immediately after, then calls
    // mark_self_loaded() once that's done.
    void host_broadcast_scene_switch(std::string_view scene_name);

    // Host: barrier is satisfied (all peers acked) and the host has
    // already loaded the new scene's entities + run main(). Bursts
    // S_SPAWN to every peer for the new scene's entities and flips
    // phase → Playing so ticks resume. Doesn't re-send S_WELCOME or
    // S_START — those are first-load primitives.
    void host_finish_scene_switch();

    // True iff the host is sitting in the scene-switch barrier — sim
    // ticks must skip while this is true (entities + Lua aren't yet
    // bound to the new scene). Used by App's should_tick gate.
    bool is_scene_switching() const { return m_scene_switching; }

    // Client: registered by App during start_session for client mode.
    // Fires when S_SCENE_SWITCH arrives; the App handler tears the
    // local scene state down (terrain swap, entity wipe, HUD/picker
    // reset, camera re-pose). NetworkManager calls send_load_done()
    // automatically right after the callback returns.
    using SceneSwitchRecvFn = std::function<void(std::string_view scene_name)>;
    void set_scene_switch_recv_fn(SceneSwitchRecvFn fn) { m_scene_switch_recv_fn = std::move(fn); }

    // ── Scripted-camera routing ─────────────────────────────────────
    // Host: route a camera command to a player. If the player is the
    // host's local slot the caller has already applied locally and we
    // skip; otherwise we send to the matching peer's transport id.
    // Returns false if the player id has no matching peer (logged).
    bool host_send_camera_set_position(u32 player_id, f32 x, f32 y);
    bool host_send_camera_pan(u32 player_id, f32 x, f32 y, f32 duration);
    bool host_send_camera_zoom(u32 player_id, f32 z);
    bool host_send_camera_shake(u32 player_id, f32 intensity, f32 duration);
    bool host_send_camera_lock_unit(u32 player_id, u32 entity_id);

    // Client: registered by App to apply incoming camera commands to
    // the local CameraController.
    using CameraSetPositionRecvFn = std::function<void(f32 x, f32 y)>;
    using CameraPanRecvFn         = std::function<void(f32 x, f32 y, f32 duration)>;
    using CameraZoomRecvFn        = std::function<void(f32 z)>;
    using CameraShakeRecvFn       = std::function<void(f32 intensity, f32 duration)>;
    using CameraLockUnitRecvFn    = std::function<void(u32 entity_id)>;
    void set_camera_set_position_recv_fn(CameraSetPositionRecvFn fn) { m_camera_set_position_recv_fn = std::move(fn); }
    void set_camera_pan_recv_fn         (CameraPanRecvFn fn)         { m_camera_pan_recv_fn          = std::move(fn); }
    void set_camera_zoom_recv_fn        (CameraZoomRecvFn fn)        { m_camera_zoom_recv_fn         = std::move(fn); }
    void set_camera_shake_recv_fn       (CameraShakeRecvFn fn)       { m_camera_shake_recv_fn        = std::move(fn); }
    void set_camera_lock_unit_recv_fn   (CameraLockUnitRecvFn fn)    { m_camera_lock_unit_recv_fn    = std::move(fn); }

    // Client: this client has finished loading — tell the host. No-op on
    // the host (host tracks self-loaded via mark_self_loaded).
    void send_load_done();

    // Host: mark the host's own process as loaded. Separate from send_load_done
    // because the host doesn't C_LOAD_DONE itself over the wire.
    void mark_self_loaded();

    // Host: are all peers (including self) done loading?
    bool all_peers_loaded() const;

    // Host: is every connected peer seated in a slot? Starting the game
    // with seatless peers leaves them as zombie clients (no S_WELCOME,
    // no spawn burst, empty world). The Start button is gated on this,
    // and host_commit_start() refuses if it's false.
    bool all_connected_peers_seated() const;
    // Number of connected peers that haven't claimed a slot yet. For UI.
    u32  seatless_peer_count() const;

    // ── Callbacks ───────────────────────────────────────────────────────
    std::function<void(std::string_view path, glm::vec3 pos)> on_sound;
    // Effect spawn received from host. `entity_id == UINT32_MAX` =
    // free-position effect; otherwise attach to the named entity
    // (`attach_point` may be empty for unit-pivot).
    std::function<void(std::string_view name, u32 entity_id, glm::vec3 pos,
                       std::string_view attach_point)> on_effect;
    // CreateEffect — persistent effect with stable handle.
    std::function<void(u32 server_id, std::string_view name, u32 entity_id,
                       glm::vec3 pos, std::string_view attach_point)> on_effect_create;
    // DestroyEffect — destroy a previously-Create'd instance.
    std::function<void(u32 server_id)> on_effect_destroy;
    std::function<void(u32 player_id)> on_player_disconnected;  // player lost connection
    std::function<void(u32 player_id)> on_player_dropped;       // timeout expired, player removed
    std::function<void()> on_lobby_state_changed;               // lobby snapshot updated
    std::function<void()> on_lobby_commit;                      // host committed — clients should enter Loading
    std::function<void()> on_lobby_start;                       // S_START received — begin Playing
    std::function<void()> on_pause_state_changed;               // pause snapshot updated (host+client)

private:
    Mode m_mode = Mode::Offline;
    Phase m_phase = Phase::None;
    bool m_connected = false;
    bool m_game_started = false;
    bool m_game_ended = false;
    std::unique_ptr<Transport> m_transport;
    u32 m_map_hash = 0;
    EndData m_end_data;
    std::string m_player_name;   // this process's display name

    // Lobby snapshot. On host: authoritative copy, pushed to all peers on
    // every mutation. On client: mirror of the host's snapshot.
    LobbyState m_lobby;
    u32        m_client_peer_id = UINT32_MAX;  // client-side: my peer_id
    bool       m_self_loaded = false;          // host-side: host's own map is loaded

    // ── Host-side ───────────────────────────────────────────────────────
    simulation::Simulation* m_simulation = nullptr;
    input::CommandSystem* m_commands = nullptr;

    struct PeerInfo {
        u32 peer_id;
        simulation::Player player;
        std::string player_name;     // from C_JOIN, shown in lobby + surfaced to Lua
        bool loaded = false;         // Loading-phase: peer sent C_LOAD_DONE
        std::unordered_set<u32> known_entities;
    };
    std::vector<PeerInfo> m_peers;
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

    // Pause view — authoritative on host (rebuilt from m_disconnected each
    // broadcast), mirrored on client from S_PAUSE_STATE.
    std::vector<DisconnectedView> m_disconnected_view;
    bool m_pause_view_active = false;
    f32  m_pause_broadcast_timer = 0.0f;  // host: time since last broadcast

    // Scene-switch barrier. Host sets true on host_broadcast_scene_switch
    // and clears it in host_finish_scene_switch. App's should_tick gate
    // reads it via is_scene_switching().
    bool m_scene_switching = false;
    // Cached during the barrier so a peer reconnecting mid-switch can
    // be re-routed onto the scene-load path (vs the normal Playing
    // reconnect that would burst stale entities).
    std::string m_in_flight_scene_name;

    // Client: callback into App that tears down the client's local
    // scene state when S_SCENE_SWITCH arrives.
    SceneSwitchRecvFn m_scene_switch_recv_fn;

    // Client: scripted-camera apply callbacks.
    CameraSetPositionRecvFn m_camera_set_position_recv_fn;
    CameraPanRecvFn         m_camera_pan_recv_fn;
    CameraZoomRecvFn        m_camera_zoom_recv_fn;
    CameraShakeRecvFn       m_camera_shake_recv_fn;
    CameraLockUnitRecvFn    m_camera_lock_unit_recv_fn;

    void host_on_connect(u32 peer_id);
    void host_on_disconnect(u32 peer_id);
    void host_on_receive(u32 peer_id, std::span<const u8> data);
    void host_send_spawn_burst(PeerInfo& peer);
    void host_update_disconnected(f32 dt);
    void host_broadcast_pause_state();
    bool is_visible_to(u32 entity_id, simulation::Player player) const;
    bool is_visible_or_remembered_to(u32 entity_id, simulation::Player player) const;

    // ── Client-side ─────────────────────────────────────────────────────
    simulation::World m_client_world;
    simulation::Player m_local_player{UINT32_MAX};
    const simulation::TypeRegistry* m_client_types = nullptr;
    const simulation::AbilityRegistry* m_client_abilities = nullptr;

    // HUD sync plumbing — set by App during start_session. Host uses
    // m_script to dispatch C_NODE_EVENT; client uses m_hud to apply
    // S_HUD_* messages.
    hud::Hud*               m_hud    = nullptr;
    script::ScriptEngine*   m_script = nullptr;

    // Vision (client computes fog locally from received entities; the
    // server already filtered out anything this client can't see)
    simulation::Vision m_client_vision;
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
    void client_handle_effect(std::span<const u8> data);
    void client_handle_effect_create(std::span<const u8> data);
    void client_handle_effect_destroy(std::span<const u8> data);
    void client_handle_projectile_dying(std::span<const u8> data);
    void client_handle_update(std::span<const u8> data);
    void client_apply_interpolation();

    void spawn_client_entity(u32 entity_id, std::string_view type_id,
                             u8 owner, f32 x, f32 y, f32 facing,
                             bool newly_created,
                             std::string_view model_path_override = {});
    void destroy_client_entity(u32 entity_id);
};

} // namespace uldum::network
