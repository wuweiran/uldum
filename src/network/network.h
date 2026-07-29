#pragma once

#include "network/protocol.h"
#include "simulation/world.h"
#include "simulation/world_view.h"
#include "simulation/vision.h"
#include "simulation/entity_types.h"
#include "simulation/command.h"

#include <glm/vec3.hpp>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace uldum::simulation { class Simulation; class TypeRegistry; class AbilityRegistry; class CommandSystem; }
namespace uldum::map    { class MapManager; }
namespace uldum::script { class ScriptEngine; }
namespace uldum::hud    { class Hud; }

namespace uldum::network {

class Transport;

// Real-time simulation tick rate (Hz) and interval (seconds).
inline constexpr f32 SIM_TICK_RATE = 32.0f;
inline constexpr f32 SIM_TICK_DT   = 1.0f / SIM_TICK_RATE;

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
                   simulation::CommandSystem& commands);
    bool init_client(std::string_view address, u16 port);

    void shutdown();

    // Called every frame: polls transport, processes incoming messages.
    // dt = frame delta time (used for disconnect timeout countdown).
    void update(f32 dt = 0);

    // Host: broadcast S_UNIT_STATE for all connected clients. Call once per sim tick.
    void host_broadcast_tick(u32 tick);

    Mode mode() const { return m_mode; }
    bool is_connected() const { return m_connected; }
    Phase phase() const { return m_phase; }

    // Host: has the game started? (all expected players connected)
    bool is_game_started() const { return m_game_started; }

    // Host: signal game over. Broadcasts S_END to all clients. A game need
    // not have a winner (draw, or an abandoned session that ended because
    // everyone left) — omit winning_team for that. The no-winner value is the
    // invalid sentinel (UINT32_MAX) on the wire.
    void host_end_game(u32 winning_team = UINT32_MAX, std::string_view stats_json = "");

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

    // Host: has EndGame been called? (mirrors the flag set by host_end_game
    // / received by clients via S_END). Workers poll this to detect when
    // they should write the result to stdout and exit.
    bool is_game_ended() const { return m_game_ended; }
    const EndData& end_data() const { return m_end_data; }

    // ── Client API ──────────────────────────────────────────────────────
    // Send a command to the server (called instead of local CommandSystem).
    void send_order(const simulation::GameCommand& cmd);

    // Raw access to the client mirror World — the World owned by GameClient's
    // Simulation (injected via set_mirror). This is REPLICATED TRUTH: the wire
    // handlers + interpolation write entities here. NOT what gets rendered —
    // that's active_view() (a fog-gated LocalView projected OVER this). Exposed
    // so spawn_client_entity + the wire handlers can materialize entities into
    // it. Client-only: null on host/offline/worker (nothing reads it there — the
    // LocalView projects over the authoritative world instead).
    simulation::World& mirror_world() { return *m_mirror; }

    // Inject the client mirror World (GameClient's Simulation world). Set once in
    // enter_lobby on the client, before connect, so an early S_WELCOME can't
    // null-deref. Replaces the old owned mirror World + Simulation world-override.
    void set_mirror(simulation::World* w) { m_mirror = w; }

    // The IWorldView the renderer / picker / HUD read: the LocalView projection
    // in EVERY mode. Host/offline project over the auth world; the client
    // projects over its network mirror (mirror_world() + its own fog) — same
    // project_local_view call, same Live/Memory membership. This is a VIEW over
    // the mirror, not a second world: it holds fog membership + snapshots + per-
    // viewer render scratch, with .source pointing at the world it projects.
    // Wired on the first projection, so this is only read after a session plays.
    simulation::IWorldView& active_view() { return m_local_view_impl; }

    // Project the authoritative world into the view-world for the local player,
    // applying the fog-of-war gate. Runs once per sim tick on the host/offline
    // (see App's tick loop). `sim` is passed explicitly so it works in Offline
    // mode (where m_simulation isn't wired).
    void project_local_view(const simulation::Simulation& sim,
                            simulation::Player local, u32 placement_count);

    // Wipe the local view-world + its membership sets. Called in lockstep with
    // the authoritative world's clear_entities on scene switch / end_session.
    void reset_local_view();

    // Assigned player ID (valid after S_WELCOME).
    simulation::Player local_player() const { return m_local_player; }

    // HUD sync plumbing (16c-v).
    // - On host: set_hud() + set_script() install handlers so client-side
    //   C_NODE_EVENT can fire server-side triggers, and host_hud_sync can
    //   route outgoing state deltas.
    // - On client: set_hud_message_fn() installs a callback that gets each
    //   raw S_HUD_* payload; the App routes it to hud::apply_network_message.
    //   Keeping the HUD dispatch out of network/ lets headless builds (the
    //   server) drop the hud library entirely.
    using HudMessageFn = std::function<void(std::span<const u8>)>;
    void set_hud_message_fn(HudMessageFn fn) { m_hud_message_fn = std::move(fn); }
    void set_script(script::ScriptEngine* scr)  { m_script = scr; }
    // Host-side: the authoritative HUD model to replay to a joining/reconnecting
    // peer (persistent nodes + permanent text tags it missed). Set on host
    // (Engine's m_hud) and worker (its headless Hud); host_send_spawn_burst calls
    // its emit_state_to per peer, filtered by each node/tag's players_mask.
    void set_hud_replay_source(hud::Hud* h) { m_hud_replay = h; }

    // Host: route a packet built by Hud's sync_fn to the matching peer(s).
    // `players_mask` is a bitmask of player ids that should receive the
    // packet (UINT32_MAX = broadcast).
    void host_hud_sync(const std::vector<u8>& packet, u32 players_mask);

    // Client: report a HUD node event (button press, etc.) to the host.
    void send_node_event(std::string_view node_id, NodeEventKind kind);


    // Set the map's script-hash (SHA-256) for join verification.
    // Client and server compute the same digest from the map's .lua
    // files; mismatch on C_JOIN is a hard reject (RejectReason::WrongMap).
    void set_map_hash(const std::array<u8, 32>& hash) { m_map_hash = hash; }

    // Client-side: bytes to present in C_JOIN. Engine doesn't interpret
    // them. Empty (default) = LAN / dev. Production clients call this
    // after receiving a session token from their game backend.
    void set_auth_token(std::vector<u8> token) { m_auth_token = std::move(token); }

    // Host-side: validator invoked on every incoming C_JOIN. Returns true
    // to admit, false to reject with S_REJECT(Unauthorized). Default (no
    // callback installed) accepts every join — preserves LAN / dev
    // ergonomics. Production hosts wire in their token-table check here.
    using AuthCallback = std::function<bool(std::span<const u8> token,
                                            std::string_view peer_name)>;
    void set_auth_callback(AuthCallback fn) { m_auth_callback = std::move(fn); }

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

    // Host: broadcast an on-change S_COLD to all clients that can see this entity.
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

    // The allocator boundary between PREPLACED entities (ids [0, N), built
    // locally on both host and client from placements.bin) and DYNAMIC ones
    // (ids >= N, host-created via Lua/projectiles → shipped over S_SPAWN).
    // The allocator is reset to 0 at every scene load, so `id < N ⟺ preplaced`
    // for all live entities in the current scene. The app captures N right
    // after load_placements (before Lua main); the host ships N to each peer
    // in S_WELCOME as a determinism guard, and uses it to exempt preplaced
    // entities from the fog-leave S_DESTROY (the client owns those).
    void set_placement_count(u32 n) { m_placement_count = n; }
    u32  placement_count() const { return m_placement_count; }

    // Host: every peer (self + remotes) has finished loading — broadcast
    // S_WELCOME + S_SPAWN burst per peer, then S_START. Phase → Playing.
    void host_finish_start();

    // Host loading barrier, poll form: finish the start once every peer has
    // acked (Loading, not mid-scene-switch, all_peers_loaded). Safe to call
    // every frame — self-guards on phase. Returns true only on the frame the
    // transition fires. Shared by uldum_dev and uldum_worker so the barrier
    // condition lives in one place.
    bool try_host_finish_start();

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
    // WC3-style camera commands. Each takes a single player; App routes
    // a `players_mask` by iterating set bits and calling these per peer
    // (host's own slot applies locally instead).
    bool host_send_camera_apply_setup(u32 player_id,
                                       f32 tx, f32 ty, f32 tz, f32 distance,
                                       f32 pitch_rad, f32 yaw_rad, f32 duration);
    bool host_send_camera_set_target_position(u32 player_id,
                                                f32 x, f32 y, f32 z, f32 duration);
    bool host_send_camera_set_source_distance(u32 player_id,
                                                f32 distance, f32 duration);
    bool host_send_camera_shake(u32 player_id, f32 intensity, f32 duration);
    bool host_send_camera_set_target_controller(u32 player_id, u32 entity_id);

    // Host: set the Action-preset controlled unit for a player. Stores the id
    // per player (for the join / scene-switch spawn-burst replay) and sends
    // S_SET_CONTROLLED_UNIT to that player's peer now. No-op for the host's own
    // slot (no peer) and when not in Host mode; the host applies locally via the
    // wire_host_broadcasts chain. entity_id UINT32_MAX clears.
    bool host_send_set_controlled_unit(u32 player_id, u32 entity_id);

    // Client: registered by App to apply incoming camera commands to
    // the local CameraController.
    using CameraApplySetupRecvFn         = std::function<void(f32 tx, f32 ty, f32 tz,
                                                               f32 distance,
                                                               f32 pitch_rad, f32 yaw_rad,
                                                               f32 duration)>;
    using CameraSetTargetPositionRecvFn  = std::function<void(f32 x, f32 y, f32 z, f32 duration)>;
    using CameraSetSourceDistanceRecvFn  = std::function<void(f32 distance, f32 duration)>;
    using CameraShakeRecvFn              = std::function<void(f32 intensity, f32 duration)>;
    using CameraSetTargetControllerRecvFn = std::function<void(u32 entity_id)>;
    void set_camera_apply_setup_recv_fn        (CameraApplySetupRecvFn fn)        { m_camera_apply_setup_recv_fn         = std::move(fn); }
    void set_camera_set_target_position_recv_fn(CameraSetTargetPositionRecvFn fn) { m_camera_set_target_position_recv_fn = std::move(fn); }
    void set_camera_set_source_distance_recv_fn(CameraSetSourceDistanceRecvFn fn) { m_camera_set_source_distance_recv_fn = std::move(fn); }
    void set_camera_shake_recv_fn              (CameraShakeRecvFn fn)             { m_camera_shake_recv_fn               = std::move(fn); }
    void set_camera_set_target_controller_recv_fn(CameraSetTargetControllerRecvFn fn) { m_camera_set_target_controller_recv_fn = std::move(fn); }

    // Client: registered by App to apply an incoming controlled-unit lock to the
    // local SelectionState (the Action-preset hero). entity_id UINT32_MAX clears.
    using SetControlledUnitRecvFn = std::function<void(u32 entity_id)>;
    void set_set_controlled_unit_recv_fn(SetControlledUnitRecvFn fn) { m_set_controlled_unit_recv_fn = std::move(fn); }

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
    // Number of connected remote peers (never counts the host/worker itself,
    // which is not in m_peers). The worker's start countdown needs >0 so an
    // empty lobby doesn't count down the instant it boots.
    u32  connected_peer_count() const { return static_cast<u32>(m_peers.size()); }

    // ── Callbacks ───────────────────────────────────────────────────────
    std::function<void(std::string_view path, glm::vec3 pos)> on_sound;
    // Script-initiated audio (Lua's PlaySound2D / PlayMusic / StopMusic /
    // PlayAmbientLoop / StopAmbientLoop). Each fires the matching
    // engine call on the client's AudioEngine. Ambient start/stop pass
    // the host-assigned handle; the App-level wiring maintains the
    // host_id → client_audio_id map.
    std::function<void(std::string_view path)>                                                 on_sound_2d;
    std::function<void(std::string_view path, f32 fade_in)>                                    on_music_play;
    std::function<void(f32 fade_out)>                                                          on_music_stop;
    std::function<void(u32 host_handle, std::string_view path, f32 x, f32 y)>                  on_ambient_start;
    std::function<void(u32 host_handle, f32 fade_out)>                                         on_ambient_stop;
    // Environment.
    std::function<void(f32 x, f32 y, f32 z)>                                                   on_set_sun_direction;
    // CreateEffect — persistent effect with stable handle.
    std::function<void(u32 server_id, std::string_view name, simulation::Unit entity,
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
    std::array<u8, 32> m_map_hash{};
    EndData m_end_data;
    std::string m_player_name;   // this process's display name

    // Client-side: token to present in C_JOIN. Host-side: validator
    // installed by the worker after reading its stdin config.
    std::vector<u8> m_auth_token;
    AuthCallback    m_auth_callback;

    // Lobby snapshot. On host: authoritative copy, pushed to all peers on
    // every mutation. On client: mirror of the host's snapshot.
    LobbyState m_lobby;
    u32        m_client_peer_id = UINT32_MAX;  // client-side: my peer_id
    bool       m_self_loaded = false;          // host-side: host's own map is loaded

    // ── Host-side ───────────────────────────────────────────────────────
    simulation::Simulation* m_simulation = nullptr;
    simulation::CommandSystem* m_commands = nullptr;

    struct PeerInfo {
        u32 peer_id;
        simulation::Player player;
        std::string player_name;     // from C_JOIN, shown in lobby + surfaced to Lua
        bool loaded = false;         // Loading-phase: peer sent C_LOAD_DONE
        // This peer's wire-projection membership: the ids currently materialized
        // in its world, so the host knows what it has already shipped and what to
        // spawn/hide/destroy this tick. Rides reconnect. (The host's own local
        // player uses m_local_discovered instead — it materializes in-process,
        // not over the wire.)
        std::unordered_set<u32> known;
        // Auth token presented at first C_JOIN. Stored so that a later
        // C_JOIN carrying the same token can be matched back to this
        // slot — that's what makes reconnect-after-blip work without
        // shuffling roles when multiple peers drop.
        std::vector<u8> auth_token;
    };
    std::vector<PeerInfo> m_peers;
    std::unordered_set<u32> m_prev_tick_entities;
    // Count of entities created by load_placements for the current scene (see
    // set_placement_count). They occupy ids [0, placement_count); the client
    // builds them locally, so an id < placement_count is client-built and an
    // id >= it is a runtime spawn shipped over the wire.
    u32 m_placement_count = 0;

    // Disconnected players awaiting reconnect
    struct DisconnectedPlayer {
        simulation::Player player;
        std::unordered_set<u32> known;           // preserved known set across the blip
        f32 timer = 0;                           // seconds remaining
        std::vector<u8> auth_token;              // preserved from PeerInfo for reconnect matching
        std::string player_name;                 // preserved so the new peer keeps the lobby display
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
    CameraApplySetupRecvFn          m_camera_apply_setup_recv_fn;
    CameraSetTargetPositionRecvFn   m_camera_set_target_position_recv_fn;
    CameraSetSourceDistanceRecvFn   m_camera_set_source_distance_recv_fn;
    CameraShakeRecvFn               m_camera_shake_recv_fn;
    CameraSetTargetControllerRecvFn m_camera_set_target_controller_recv_fn;

    // Client: apply an incoming controlled-unit lock to the local selection.
    SetControlledUnitRecvFn         m_set_controlled_unit_recv_fn;

    // Host: current Action-preset controlled unit per player id, so a client
    // that joins (or reloads on a scene switch) after main() ran gets the hero
    // lock replayed in its spawn burst. Cleared on scene switch (ids reset).
    std::unordered_map<u32, u32>    m_controlled_unit_by_player;

    void host_on_connect(u32 peer_id);
    void host_on_disconnect(u32 peer_id);
    void host_on_receive(u32 peer_id, std::span<const u8> data);
    PeerInfo* find_peer(u32 peer_id);
    const PeerInfo* find_peer(u32 peer_id) const;
    void host_send_spawn(PeerInfo& peer, u32 entity_id,
                         const simulation::HandleInfo& info,
                         bool newly_created);
    void host_send_show(PeerInfo& peer, u32 entity_id,
                        const simulation::HandleInfo& info);
    // MATERIALIZE cold-state batch (S_COLD, N records) — sent right after
    // S_SPAWN/S_SHOW for any stateful entity. No-op if the entity has no records.
    void host_send_cold_batch(PeerInfo& peer, u32 entity_id);
    void host_send_spawn_burst(PeerInfo& peer);
    void host_update_disconnected(f32 dt);
    void host_broadcast_pause_state();
    bool is_visible_to(u32 entity_id, simulation::Player player) const;

    // The fog-projection gate for the LOCAL player (project_local_view). Defines
    // "does the local player keep this entity in view this tick" and, if so,
    // whether it's live or remembered — so its snapshot/live decision routes
    // through the same static/mobile fog rule the client applies to what the host
    // ships (they can't drift). Updates `discovered` (the per-view sighting set,
    // H3) as a side effect. The wire path does NOT call this — the host ships
    // live-only (is_visible_to) and the client owns its own memory.
    struct ViewGate { bool keep; bool live_vis; bool remembered; };
    ViewGate view_gate(const simulation::World& world, const simulation::Simulation& sim,
                       u32 id, simulation::Player player, u32 placement_count,
                       std::unordered_set<u32>& discovered) const;

    // ── This process's view-world ───────────────────────────────────────
    // The network CLIENT's mirror world — owned by GameClient's Simulation,
    // injected via set_mirror. Filled from S_SPAWN/S_SHOW/S_UNIT_STATE/S_HIDE/
    // S_DESTROY + interpolation. Client-only: null on host/offline/worker (they
    // never set it — nothing reads it there). active_view() never returns this
    // raw World; it returns m_local_view_impl (a LocalView) whose .source is this
    // mirror on the client, the auth world on host/offline.
    simulation::World* m_mirror = nullptr;
    // The one IWorldView the renderer / picker / HUD read, in EVERY mode. On
    // host/offline project_local_view() drives it over the authoritative world;
    // on the client over the mirror — same LocalView, same snapshot/visible
    // membership. Its .source is wired on the first project_local_view() call.
    simulation::LocalView m_local_view_impl;
    simulation::Player m_local_player{UINT32_MAX};

    // HUD sync plumbing — set by App during start_session. Host uses
    // m_script to dispatch C_NODE_EVENT; client uses m_hud_message_fn
    // to forward S_HUD_* messages to hud::apply_network_message.
    HudMessageFn            m_hud_message_fn;
    script::ScriptEngine*   m_script = nullptr;
    hud::Hud*               m_hud_replay = nullptr;  // host-side join-replay source

    // Snapshot buffer for interpolation (two most recent). Units and projectiles
    // ride separate HOT packets (S_UNIT_STATE / S_PROJECTILE_STATE) and land in
    // separate arrays; both interpolate position/facing, only units carry the
    // scalar half (health/flags/states). A projectile snapshot may arrive on a
    // different tick than the unit one — each array tracks its own newest.
    struct Snapshot {
        u32 tick = 0;
        f64 receive_time = 0;
        std::vector<UnitState> units;
        std::vector<ProjectileState> projectiles;
    };
    Snapshot m_snapshots[2];
    u32 m_snap_idx = 0;      // write index (flips between 0 and 1)
    bool m_has_two_snaps = false;

    void client_on_connect(u32 peer_id);
    void client_on_disconnect(u32 peer_id);
    void client_on_receive(u32 peer_id, std::span<const u8> data);
    void client_handle_welcome(std::span<const u8> data);
    void client_handle_spawn(std::span<const u8> data);
    void client_handle_show(std::span<const u8> data);
    void client_handle_hide(std::span<const u8> data);
    void client_handle_destroy(std::span<const u8> data);
    void client_handle_unit_state(std::span<const u8> data);
    void client_handle_projectile_state(std::span<const u8> data);
    // Advance the double-buffer to `tick`; returns the snapshot index to fill.
    // Shared by the unit + projectile HOT handlers (same-tick packets fill one
    // snapshot; a new tick opens a fresh one).
    u32 client_begin_snapshot_tick(u32 tick);
    void client_handle_sound(std::span<const u8> data);
    void client_handle_effect_create(std::span<const u8> data);
    void client_handle_effect_destroy(std::span<const u8> data);
    void client_handle_projectile_dying(std::span<const u8> data);
    void client_handle_cold(std::span<const u8> data);         // S_COLD: 1 record (on-change) or N (materialize)
    void client_apply_interpolation();

    void spawn_client_entity(simulation::World& world, u32 entity_id,
                             std::string_view type_id,
                             u8 owner, f32 x, f32 y, f32 facing,
                             bool newly_created,
                             u8 variation = 0);
    void destroy_client_entity(u32 entity_id);

    // The host/offline local player's fog-projection scratch. project_local_view
    // runs view_gate for the local player and materializes the result in-process
    // (into m_local_view_impl) instead of over the wire: "local game =
    // server↔client with the network calls replaced by function calls". Only the
    // `discovered` sighting set survives across ticks (view_gate's H3 gate: static
    // ids seen LIVE at least once this scene, so a pre-explored-but-never-scouted
    // static doesn't surface from memory). A remote client has no counterpart —
    // it's a pure consumer of what the host ships.
    std::unordered_set<u32> m_local_discovered;
};

} // namespace uldum::network
