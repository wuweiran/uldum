#include "network/network.h"
#include "network/game_client.h"
#include "network/game_server.h"
#include "network/transport.h"
#include "network/enet_transport.h"
#include "network/protocol.h"
#include "core/log.h"

#include <algorithm>

namespace uldum::network {

static constexpr const char* TAG = "Network";

NetworkManager::NetworkManager() = default;
NetworkManager::~NetworkManager() = default;

bool NetworkManager::init_offline() {
    m_mode = Mode::Offline;
    m_phase = Phase::Lobby;   // so the slot-claim handler accepts edits
    m_connected = false;
    log::info(TAG, "NetworkManager initialized — mode=Offline");
    return true;
}

// ── Host ─────────────────────────────────────────────────────────────────

bool NetworkManager::init_host(u16 port, u32 max_players, GameServer& server) {
    m_server = &server;

    auto transport = std::make_unique<ENetTransport>();
    if (!transport->host(port, max_players)) return false;

    transport->on_connect = [this](u32 id) { host_on_connect(id); };
    transport->on_disconnect = [this](u32 id) { host_on_disconnect(id); };
    transport->on_receive = [this](u32 id, std::span<const u8> d) { host_on_receive(id, d); };

    m_transport = std::move(transport);
    m_mode = Mode::Host;
    m_connected = true;
    m_phase = Phase::Lobby;   // lobby-first: host waits for Start click
    log::info(TAG, "NetworkManager initialized — mode=Host, port={} (lobby)", port);
    return true;
}

void NetworkManager::host_on_connect(u32 peer_id) {
    log::info(TAG, "Peer {} connected, awaiting C_JOIN", peer_id);
}

NetworkManager::PeerInfo* NetworkManager::find_peer(u32 peer_id) {
    auto it = std::find_if(m_peers.begin(), m_peers.end(),
                           [&](const PeerInfo& peer) { return peer.peer_id == peer_id; });
    return it != m_peers.end() ? &*it : nullptr;
}

const NetworkManager::PeerInfo* NetworkManager::find_peer(u32 peer_id) const {
    auto it = std::find_if(m_peers.begin(), m_peers.end(),
                           [&](const PeerInfo& peer) { return peer.peer_id == peer_id; });
    return it != m_peers.end() ? &*it : nullptr;
}

void NetworkManager::host_on_disconnect(u32 peer_id) {
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        if (it->peer_id != peer_id) continue;

        // Lobby-phase disconnect: just release any claimed slot and drop
        // the peer. No reconnect bookkeeping since there's no game running.
        if (m_phase == Phase::Lobby) {
            bool changed = false;
            for (auto& a : m_lobby.slots) {
                if (a.occupant == SlotOccupant::Human && a.peer_id == peer_id) {
                    a.occupant = SlotOccupant::Open;
                    a.peer_id  = 0;
                    a.display_name.clear();
                    changed = true;
                }
            }
            m_peers.erase(it);
            if (changed) host_broadcast_lobby_state();
            log::info(TAG, "Peer {} left lobby", peer_id);
            return;
        }

        u32 player_id = it->player.id;
        log::info(TAG, "Player {} (peer {}) disconnected — waiting {:.0f}s for reconnect",
                  player_id, peer_id, m_disconnect_timeout);

        DisconnectedPlayer dp;
        dp.player          = it->player;
        dp.timer           = m_disconnect_timeout;
        dp.auth_token      = std::move(it->auth_token);
        dp.player_name     = std::move(it->player_name);
        m_disconnected.push_back(std::move(dp));
        m_peers.erase(it);

        if (m_pause_on_disconnect && m_game_started) {
            m_paused = true;
            log::info(TAG, "Game paused — waiting for reconnect");
        }

        if (m_server) m_server->peer_disconnected(player_id);
        host_broadcast_pause_state();
        return;
    }
}

void NetworkManager::host_broadcast_lobby_state() {
    // Host: broadcast to all peers. Offline: no wire traffic, just fire
    // the local callback so UI-side listeners still see the update.
    if (m_mode == Mode::Host && m_transport) {
        auto msg = build_lobby_state(m_lobby);
        m_transport->broadcast(msg, true);
    }
    if (on_lobby_state_changed) on_lobby_state_changed();
}

void NetworkManager::host_commit_start() {
    if (m_mode != Mode::Host || m_phase != Phase::Lobby) return;

    // Safety net — the UI gates Start on this too, but if something slips
    // through, starting with seatless peers makes them zombie clients
    // (no S_WELCOME, no spawn burst, empty world). Refuse.
    if (!all_connected_peers_seated()) {
        log::warn(TAG, "host_commit_start refused — {} connected peer(s) haven't claimed a slot",
                  seatless_peer_count());
        return;
    }

    // Bind each connected peer to its claimed slot.
    for (auto& peer : m_peers) {
        peer.loaded = false;
        for (u32 i = 0; i < m_lobby.slots.size(); ++i) {
            const auto& a = m_lobby.slots[i];
            if (a.occupant == SlotOccupant::Human && a.peer_id == peer.peer_id) {
                peer.player = simulation::Player{i};
                break;
            }
        }
    }
    m_self_loaded = false;
    m_phase = Phase::Loading;

    auto msg = build_lobby_commit();
    m_transport->broadcast(msg, true);
    log::info(TAG, "Host committed lobby — {} peer(s) loading", m_peers.size());
}

void NetworkManager::host_finish_start() {
    if (m_mode != Mode::Host || m_phase != Phase::Loading) return;
    // A scene switch also parks the phase at Loading but runs its own finish
    // (host_finish_scene_switch). Without this guard the worker's "Loading &&
    // all_peers_loaded" transition would re-send S_WELCOME mid-switch with the
    // stale placement_count over the empty world → client PLACEMENT DESYNC.
    if (m_scene_switching) return;

    // S_WELCOME + spawn burst per seated peer, then broadcast S_START.
    for (auto& peer : m_peers) {
        if (!peer.player.is_valid()) continue;
        auto welcome = build_welcome(
            peer.player.id, static_cast<u32>(m_lobby.slots.size()),
            static_cast<u32>(SIM_TICK_RATE), m_server->placement_count());
        m_transport->send(peer.peer_id, welcome, true);
        m_server->send_spawn_burst(*this, peer.peer_id, peer.player);
        m_server->replay_persistent_state(*this, peer.peer_id, peer.player);
    }

    auto msg = build_start();
    m_transport->broadcast(msg, true);

    m_game_started = true;
    m_phase = Phase::Playing;
    log::info(TAG, "Host finished start: game live");
}

void NetworkManager::mark_self_loaded() {
    m_self_loaded = true;
}

bool NetworkManager::try_host_finish_start() {
    if (m_mode != Mode::Host || m_phase != Phase::Loading || m_scene_switching) return false;
    if (!all_peers_loaded()) return false;
    host_finish_start();
    return true;
}

void NetworkManager::host_broadcast_scene_switch(std::string_view scene_name) {
    if (m_mode != Mode::Host || !m_transport) return;
    if (m_phase != Phase::Playing) {
        log::warn(TAG, "host_broadcast_scene_switch called outside Playing (phase {})",
                  static_cast<i32>(m_phase));
        return;
    }
    m_scene_switching = true;
    m_phase = Phase::Loading;
    m_self_loaded = false;
    for (auto& p : m_peers) p.loaded = false;
    m_in_flight_scene_name = std::string(scene_name);
    auto msg = build_scene_switch(scene_name);
    m_transport->broadcast(msg, true);
    log::info(TAG, "Host broadcasting scene switch → '{}' ({} peer(s) loading)",
              scene_name, m_peers.size());
}

void NetworkManager::host_finish_scene_switch() {
    if (m_mode != Mode::Host || !m_scene_switching) return;

    // Re-welcome + spawn burst per peer for the new scene's entities. The
    // welcome carries the new scene's placement_count so the client re-syncs
    // its boundary (a scene switch rebuilds the world like a join); it also
    // re-runs the client's determinism guard against the rebuilt world.
    // since switch_scene cleared the previous scene, only new-scene entities
    // are visible.
    for (auto& peer : m_peers) {
        if (!peer.player.is_valid()) continue;
        auto welcome = build_welcome(
            peer.player.id, static_cast<u32>(m_lobby.slots.size()),
            static_cast<u32>(SIM_TICK_RATE), m_server->placement_count());
        m_transport->send(peer.peer_id, welcome, true);
        m_server->clear_replication(peer.player);
        m_server->send_spawn_burst(*this, peer.peer_id, peer.player);
        m_server->replay_persistent_state(*this, peer.peer_id, peer.player);
    }

    m_phase = Phase::Playing;
    m_scene_switching = false;
    m_in_flight_scene_name.clear();
    log::info(TAG, "Host finished scene switch — sim resumes");
}

bool NetworkManager::all_connected_peers_seated() const {
    return seatless_peer_count() == 0;
}

u32 NetworkManager::seatless_peer_count() const {
    if (m_mode != Mode::Host) return 0;
    u32 count = 0;
    for (const auto& peer : m_peers) {
        bool seated = false;
        for (const auto& a : m_lobby.slots) {
            if (a.occupant == SlotOccupant::Human && a.peer_id == peer.peer_id) {
                seated = true; break;
            }
        }
        if (!seated) ++count;
    }
    return count;
}

bool NetworkManager::all_peers_loaded() const {
    if (m_mode == Mode::Host) {
        if (!m_self_loaded) return false;
        for (const auto& p : m_peers) {
            if (p.player.is_valid() && !p.loaded) return false;
        }
        return true;
    }
    return true;
}

void NetworkManager::send_load_done() {
    if (m_mode != Mode::Client || !m_transport) return;
    auto msg = build_load_done();
    m_transport->send(0, msg, true);
}

void NetworkManager::host_on_receive(u32 peer_id, std::span<const u8> data) {
    if (data.empty()) return;
    auto type = peek_type(data);

    switch (type) {
    case MsgType::C_JOIN: {
        ByteReader r(data);
        r.read_u8();  // skip type
        std::array<u8, 32> client_hash{};
        r.read_bytes(client_hash.data(), client_hash.size());
        u16 token_len = r.read_u16();
        std::vector<u8> client_token;
        if (token_len > 0) {
            client_token.resize(token_len);
            r.read_bytes(client_token.data(), token_len);
        }
        std::string peer_name = r.read_string();

        // All-zero hash on the server means "no map verification" — used
        // by tests / future generic-server flows before the host has
        // bound to a specific map. Skip the comparison in that case.
        bool host_unset = std::all_of(m_map_hash.begin(), m_map_hash.end(),
                                      [](u8 b) { return b == 0; });
        if (!host_unset && client_hash != m_map_hash) {
            auto reject = build_reject(RejectReason::WrongMap);
            m_transport->send(peer_id, reject, true);
            log::warn(TAG, "Peer {} rejected: wrong map hash", peer_id);
            return;
        }

        // Auth-on-join: only runs if a callback has been installed (the
        // worker does this after reading its stdin config). Without one
        // we accept every join, preserving LAN / dev ergonomics.
        if (m_auth_callback && !m_auth_callback(client_token, peer_name)) {
            auto reject = build_reject(RejectReason::Unauthorized);
            m_transport->send(peer_id, reject, true);
            log::warn(TAG, "Peer {} rejected: auth callback denied", peer_id);
            return;
        }

        // Lobby phase: register the peer with no slot and send them the
        // current lobby snapshot. Slot claims happen via C_CLAIM_SLOT.
        if (m_phase == Phase::Lobby) {
            // Idempotent register: a duplicate C_JOIN (retransmit, or a
            // misbehaving/hostile client) must not append a second record
            // for the same peer_id — that would leak PeerInfo entries and
            // let one connection masquerade as several. Refresh in place if
            // we already know this peer.
            PeerInfo* existing = find_peer(peer_id);
            if (existing) {
                existing->player_name = std::move(peer_name);
                existing->auth_token  = client_token;
                log::info(TAG, "Peer {} re-joined lobby (dedup)", peer_id);
            } else {
                PeerInfo info{.peer_id = peer_id, .player = simulation::Player{UINT32_MAX},
                              .player_name = std::move(peer_name), .loaded = false,
                              .auth_token = client_token};
                m_peers.push_back(std::move(info));
                log::info(TAG, "Peer {} joined lobby", peer_id);
            }

            auto assign = build_lobby_assign(peer_id);
            m_transport->send(peer_id, assign, true);

            auto state_msg = build_lobby_state(m_lobby);
            m_transport->send(peer_id, state_msg, true);
            return;
        }

        // Playing phase: reconnect path. Match the C_JOIN against the
        // disconnected list. With a non-empty token we pin the match
        // to the specific peer that holds that token, so multiple
        // simultaneous disconnects don't shuffle slots. With an empty
        // token (LAN / dev) we fall back to first-in-the-list.
        std::vector<DisconnectedPlayer>::iterator it = m_disconnected.end();
        if (!client_token.empty()) {
            it = std::find_if(m_disconnected.begin(), m_disconnected.end(),
                              [&](const DisconnectedPlayer& d) {
                                  return d.auth_token == client_token;
                              });
            if (it == m_disconnected.end()) {
                // Token doesn't match anyone we remember — a stranger
                // trying to slip into someone else's slot. Reject.
                auto reject = build_reject(RejectReason::Unauthorized);
                m_transport->send(peer_id, reject, true);
                log::warn(TAG, "Peer {} rejected: token didn't match any disconnected slot", peer_id);
                return;
            }
        } else if (!m_disconnected.empty()) {
            it = m_disconnected.begin();
        }
        if (it != m_disconnected.end()) {
            u32 slot = it->player.id;
            // Prefer the saved display_name (the one the player joined
            // with originally) over whatever the reconnecting client
            // re-sent — keeps the lobby UI stable across blips.
            std::string restored_name = it->player_name.empty() ? std::move(peer_name)
                                                                 : it->player_name;
            PeerInfo info{.peer_id = peer_id, .player = it->player,
                          .player_name = std::move(restored_name), .loaded = false,
                          .auth_token = std::move(it->auth_token)};
            m_disconnected.erase(it);

            auto welcome = build_welcome(
                slot, static_cast<u32>(m_lobby.slots.size()),
                static_cast<u32>(SIM_TICK_RATE), m_server->placement_count());
            m_transport->send(peer_id, welcome, true);

            m_peers.push_back(std::move(info));

            if (m_scene_switching) {
                // Mid-barrier reconnect: the world is currently empty
                // on the host (placements haven't loaded yet) and the
                // new scene's main() hasn't run, so a normal spawn
                // burst would send stale / empty state. Route the new
                // peer onto the scene-load path instead — they ack
                // via C_LOAD_DONE and the post-barrier finish will
                // burst the new scene's entities to them along with
                // every other peer.
                auto& fresh = m_peers.back();
                m_server->clear_replication(fresh.player);
                fresh.loaded = false;
                if (!m_in_flight_scene_name.empty()) {
                    auto msg = build_scene_switch(m_in_flight_scene_name);
                    m_transport->send(peer_id, msg, true);
                }
                log::info(TAG, "Player {} reconnected mid-scene-switch (peer {}) — joining barrier",
                          slot, peer_id);
            } else {
                m_server->send_spawn_burst(
                    *this, m_peers.back().peer_id, m_peers.back().player);
                m_server->replay_persistent_state(
                    *this, m_peers.back().peer_id, m_peers.back().player);

                if (m_game_started) {
                    auto msg = build_start();
                    m_transport->send(peer_id, msg, true);
                }
                log::info(TAG, "Player {} reconnected (peer {})", slot, peer_id);
            }

            if (m_paused && m_disconnected.empty()) {
                m_paused = false;
                log::info(TAG, "All players reconnected — game resumed");
            }

            host_broadcast_pause_state();
            return;
        }

        // Playing phase with no disconnected slot waiting — nothing to
        // assign. Reject.
        log::warn(TAG, "Peer {} attempted to join a running game with no open slot", peer_id);
        auto reject = build_reject(RejectReason::Started);
        m_transport->send(peer_id, reject, true);
        break;
    }

    case MsgType::C_CLAIM_SLOT: {
        if (m_phase != Phase::Lobby) break;
        u32 slot = parse_claim_or_release_slot(data);
        if (slot >= m_lobby.slots.size()) break;
        auto& a = m_lobby.slots[slot];
        if (a.locked) break;
        // Accept only if the slot isn't Human-claimed by someone else.
        if (a.occupant == SlotOccupant::Human && a.peer_id != peer_id) break;

        std::string claimer_name;
        if (peer_id == LOCAL_PEER) {
            claimer_name = m_player_name.empty() ? "Host" : m_player_name;
        } else {
            const PeerInfo* claimer = find_peer(peer_id);
            if (!claimer) {
                log::warn(TAG, "Peer {} tried to claim slot {} without registering — ignored",
                          peer_id, slot);
                break;
            }
            claimer_name = claimer->player_name.empty() ? "Player" : claimer->player_name;
        }

        // Release any other slot currently claimed by this peer. Restore
        // each one's manifest-declared base_name so the UI doesn't show
        // an empty label.
        for (auto& other : m_lobby.slots) {
            if (&other != &a && other.occupant == SlotOccupant::Human &&
                                 other.peer_id == peer_id) {
                other.occupant     = SlotOccupant::Open;
                other.peer_id      = 0;
                other.display_name = other.base_name;
            }
        }
        a.occupant     = SlotOccupant::Human;
        a.peer_id      = peer_id;
        a.display_name = claimer_name;
        host_broadcast_lobby_state();
        break;
    }

    case MsgType::C_RELEASE_SLOT: {
        if (m_phase != Phase::Lobby) break;
        u32 slot = parse_claim_or_release_slot(data);
        if (slot >= m_lobby.slots.size()) break;
        auto& a = m_lobby.slots[slot];
        if (a.occupant == SlotOccupant::Human && a.peer_id == peer_id && !a.locked) {
            a.occupant     = SlotOccupant::Open;
            a.peer_id      = 0;
            a.display_name = a.base_name;
            host_broadcast_lobby_state();
        }
        break;
    }

    case MsgType::C_LOAD_DONE: {
        if (m_phase != Phase::Loading) break;
        if (auto* peer = find_peer(peer_id)) {
            peer->loaded = true;
            log::info(TAG, "Peer {} finished loading", peer_id);
        }
        break;
    }

    case MsgType::C_ORDER: {
        const PeerInfo* peer = find_peer(peer_id);
        if (!peer || !peer->player.is_valid()) return;
        if (!m_server->receive_order(peer->player, data)) {
            log::warn(TAG, "Peer {} sent a malformed C_ORDER — dropped", peer_id);
        }
        break;
    }

    case MsgType::C_LEAVE: {
        log::info(TAG, "Peer {} sent C_LEAVE", peer_id);
        break;
    }

    case MsgType::C_NODE_EVENT: {
        const PeerInfo* peer = find_peer(peer_id);
        if (!peer || !peer->player.is_valid()) return;

        m_server->receive_node_event(peer->player, data);
        break;
    }

    default:
        log::warn(TAG, "Host received unknown message type 0x{:02x}", static_cast<u8>(type));
        break;
    }
}


void NetworkManager::host_update_disconnected(f32 dt) {
    bool changed = false;
    for (auto it = m_disconnected.begin(); it != m_disconnected.end(); ) {
        it->timer -= dt;
        if (it->timer <= 0) {
            u32 player_id = it->player.id;
            log::info(TAG, "Player {} reconnect timeout expired — dropped", player_id);
            if (m_server) m_server->player_dropped(player_id);
            it = m_disconnected.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    // Unpause if all disconnected players have been dropped
    if (m_paused && m_disconnected.empty()) {
        m_paused = false;
        changed = true;
        log::info(TAG, "All disconnected players dropped — game resumed");
    }

    // Abandoned-session auto-end. A headless worker (no seated local player)
    // whose game has started but now has no connected peers AND no one left
    // in the reconnect window is unplayable — nobody can rejoin. End the
    // session so the worker's is_game_ended() exit fires and the orchestrator
    // reaps it (frees the port). Reuses the EndGame path with no winner.
    // Guarded so it never touches a dev host / offline (valid m_local_player)
    // and only fires once.
    if (!m_game_ended && m_game_started && !m_local_player.is_valid() &&
        m_peers.empty() && m_disconnected.empty()) {
        log::info(TAG, "Session abandoned (no players, no reconnect pending) — ending");
        host_end_game();  // no winner
    }

    // Rebuild local view + broadcast. Events (disconnect / drop / unpause)
    // broadcast immediately; otherwise re-broadcast once a second so clients
    // see the countdown tick.
    m_pause_broadcast_timer += dt;
    bool periodic = (m_paused && m_pause_broadcast_timer >= 1.0f);
    if (changed || periodic) {
        m_pause_broadcast_timer = 0.0f;
        host_broadcast_pause_state();
    }
}

void NetworkManager::host_broadcast_pause_state() {
    if (m_mode != Mode::Host) return;
    m_disconnected_view.clear();
    m_disconnected_view.reserve(m_disconnected.size());
    for (const auto& d : m_disconnected) {
        DisconnectedView v;
        v.player_id = d.player.id;
        v.display_name = (d.player.id < m_lobby.slots.size())
            ? m_lobby.slots[d.player.id].display_name : std::string{};
        v.seconds_remaining = d.timer;
        m_disconnected_view.push_back(std::move(v));
    }
    m_pause_view_active = m_paused;
    if (on_pause_state_changed) on_pause_state_changed();

    if (m_transport) {
        auto msg = build_pause_state(m_paused, m_disconnected_view);
        m_transport->broadcast(msg, true);
    }
}

void NetworkManager::send_to_peer(
        u32 peer_id, std::span<const u8> packet, bool reliable) {
    if (m_mode != Mode::Host || !m_transport) return;
    m_transport->send(peer_id, packet, reliable);
}

void NetworkManager::host_broadcast(std::span<const u8> packet) {
    if (m_mode != Mode::Host || m_peers.empty()) return;
    for (auto& peer : m_peers) {
        m_transport->send(peer.peer_id, packet, true);
    }
}

void NetworkManager::host_send_to_player(u32 player_id, std::span<const u8> packet) {
    if (m_mode != Mode::Host) return;
    for (auto& peer : m_peers) {
        if (peer.player.id == player_id) {
            m_transport->send(peer.peer_id, packet, true);
            return;
        }
    }
}

void NetworkManager::host_end_game(u32 winning_team, std::string_view stats_json) {
    if (m_mode != Mode::Host && m_mode != Mode::Offline) return;
    m_game_ended = true;
    m_end_data = EndData{winning_team, std::string(stats_json)};
    if (m_mode == Mode::Host) {
        auto msg = build_end(winning_team, stats_json);
        m_transport->broadcast(msg, true);
    }
    if (winning_team == UINT32_MAX)
        log::info(TAG, "Game ended — no winner");
    else
        log::info(TAG, "Game ended — winning team {}", winning_team);
}

// ── Client ───────────────────────────────────────────────────────────────

bool NetworkManager::init_client(std::string_view address, u16 port,
                                 GameClient& client) {
    m_client = &client;
    auto transport = std::make_unique<ENetTransport>();
    if (!transport->connect(address, port)) return false;

    transport->on_connect = [this](u32 id) { client_on_connect(id); };
    transport->on_disconnect = [this](u32 id) { client_on_disconnect(id); };
    transport->on_receive = [this](u32 id, std::span<const u8> d) { client_on_receive(id, d); };

    m_transport = std::move(transport);
    m_mode = Mode::Client;
    m_phase = Phase::Lobby;
    m_connected = false;  // flipped to true on S_LOBBY_ASSIGN (lobby) or S_WELCOME (legacy)
    log::info(TAG, "NetworkManager initialized — mode=Client, connecting to {}:{}", address, port);
    return true;
}

void NetworkManager::client_on_connect(u32 /*peer_id*/) {
    log::info(TAG, "Connected to server, sending C_JOIN (name='{}', token={}B)",
              m_player_name, m_auth_token.size());
    auto msg = build_join(m_map_hash, m_auth_token, m_player_name);
    m_transport->send(0, msg, true);
}

void NetworkManager::client_on_disconnect(u32 /*peer_id*/) {
    log::warn(TAG, "Disconnected from server");
    m_connected = false;
}

void NetworkManager::client_on_receive(u32 /*peer_id*/, std::span<const u8> data) {
    if (data.empty()) return;
    auto type = peek_type(data);

    switch (type) {
    case MsgType::S_LOBBY_ASSIGN: {
        m_client_peer_id = parse_lobby_assign(data);
        m_connected = true;
        log::info(TAG, "Lobby: assigned peer id {}", m_client_peer_id);
        break;
    }
    case MsgType::S_LOBBY_STATE: {
        m_lobby = parse_lobby_state(data);
        if (on_lobby_state_changed) on_lobby_state_changed();
        break;
    }
    case MsgType::S_LOBBY_COMMIT: {
        m_phase = Phase::Loading;
        log::info(TAG, "Host committed lobby — entering Loading");
        if (on_lobby_commit) on_lobby_commit();
        break;
    }
    case MsgType::S_WELCOME: client_handle_welcome(data); break;
    case MsgType::S_REJECT: {
        ByteReader r(data);
        r.read_u8();
        u8 reason = r.read_u8();
        std::string_view explanation;
        switch (static_cast<RejectReason>(reason)) {
            case RejectReason::Full:         explanation = "lobby is full";                                     break;
            case RejectReason::WrongMap:     explanation = "map mismatch — your map differs from the server's"; break;
            case RejectReason::Started:      explanation = "session already started";                           break;
            case RejectReason::Unauthorized: explanation = "auth-on-join rejected the presented token";         break;
            default:                         explanation = "unknown";                                            break;
        }
        log::error(TAG, "Server rejected connection: {} (reason={})", explanation, reason);
        break;
    }
    case MsgType::S_SPAWN: {
        auto materialize = parse_materialize(data);
        if (materialize) m_client->apply_spawn(*materialize, true);
        break;
    }
    case MsgType::S_SHOW: {
        auto materialize = parse_materialize(data);
        if (materialize) m_client->apply_spawn(*materialize, false);
        break;
    }
    case MsgType::S_HIDE:
        m_client->apply_hide(parse_hide(data));
        break;
    case MsgType::S_DESTROY:
        m_client->apply_destroy(parse_destroy(data));
        break;
    case MsgType::S_UNIT_STATE:
        m_client->apply_unit_state(parse_unit_state(data));
        break;
    case MsgType::S_PROJECTILE_STATE:
        m_client->apply_projectile_state(parse_projectile_state(data));
        break;
    case MsgType::S_ANIM_EVENT: {
        auto event = parse_anim_event(data);
        if (!event) break;
        if (event->kind == AnimEventKind::AttackStart) {
            m_client->apply_attack_start(*event);
        } else if (on_anim_event) {
            on_anim_event(*event);
        }
        break;
    }
    case MsgType::S_SOUND: client_handle_sound(data); break;
    case MsgType::S_SOUND_PLAY_2D: {
        ByteReader r(data); r.read_u8();
        std::string path = r.read_string();
        if (on_sound_2d) on_sound_2d(path);
        break;
    }
    case MsgType::S_MUSIC_PLAY: {
        ByteReader r(data); r.read_u8();
        std::string path = r.read_string();
        f32 fade_in = r.read_f32();
        if (on_music_play) on_music_play(path, fade_in);
        break;
    }
    case MsgType::S_MUSIC_STOP: {
        ByteReader r(data); r.read_u8();
        f32 fade_out = r.read_f32();
        if (on_music_stop) on_music_stop(fade_out);
        break;
    }
    case MsgType::S_AMBIENT_START: {
        ByteReader r(data); r.read_u8();
        u32 handle = r.read_u32();
        std::string path = r.read_string();
        f32 x = r.read_f32();
        f32 y = r.read_f32();
        if (on_ambient_start) on_ambient_start(handle, path, x, y);
        break;
    }
    case MsgType::S_AMBIENT_STOP: {
        ByteReader r(data); r.read_u8();
        u32 handle = r.read_u32();
        f32 fade_out = r.read_f32();
        if (on_ambient_stop) on_ambient_stop(handle, fade_out);
        break;
    }
    case MsgType::S_SET_SUN_DIRECTION: {
        ByteReader r(data); r.read_u8();
        f32 x = r.read_f32();
        f32 y = r.read_f32();
        f32 z = r.read_f32();
        if (on_set_sun_direction) on_set_sun_direction(x, y, z);
        break;
    }
    case MsgType::S_EFFECT_CREATE:   client_handle_effect_create(data); break;
    case MsgType::S_EFFECT_DESTROY:  client_handle_effect_destroy(data); break;
    case MsgType::S_PROJECTILE_DYING: {
        auto hit = m_client->apply_projectile_dying(parse_projectile_dying(data));
        if (hit && on_projectile_hit_animation) on_projectile_hit_animation(*hit);
        break;
    }
    case MsgType::S_COLD:
        m_client->apply_cold(parse_cold(data));
        break;
    case MsgType::S_START:
        m_game_started = true;
        m_phase = Phase::Playing;
        log::info(TAG, "Game started!");
        if (on_lobby_start) on_lobby_start();
        break;
    case MsgType::S_END: {
        m_end_data = parse_end(data);
        m_game_ended = true;
        if (m_end_data.winning_team == UINT32_MAX)
            log::info(TAG, "Game ended — no winner");
        else
            log::info(TAG, "Game ended — winning team {}", m_end_data.winning_team);
        break;
    }
    case MsgType::S_PAUSE_STATE: {
        auto ps = parse_pause_state(data);
        m_pause_view_active  = ps.paused;
        m_disconnected_view  = std::move(ps.disconnected);
        if (on_pause_state_changed) on_pause_state_changed();
        break;
    }

    case MsgType::S_SCENE_SWITCH: {
        // Mirror the host: enter the scene-switch barrier so any
        // sim-tick / tick-broadcast paths that depend on phase pause.
        m_scene_switching = true;
        m_phase = Phase::Loading;

        std::string scene_name = parse_scene_switch(data);
        log::info(TAG, "Client received scene switch → '{}'", scene_name);

        // Run the App-supplied teardown (terrain swap, sim wipe, HUD
        // / picker reset, camera re-pose). The callback runs inline —
        // the reliable-ordered channel guarantees subsequent S_SPAWN /
        // S_HUD_CREATE_NODE deltas land after this point.
        if (m_scene_switch_recv_fn) m_scene_switch_recv_fn(scene_name);

        // Ack so the host's barrier can clear once every peer reports.
        send_load_done();
        break;
    }

    case MsgType::S_CAMERA_APPLY_SETUP: {
        ByteReader r(data); r.read_u8();
        f32 tx = r.read_f32(), ty = r.read_f32(), tz = r.read_f32();
        f32 distance = r.read_f32();
        f32 pitch_rad = r.read_f32(), yaw_rad = r.read_f32();
        f32 duration = r.read_f32();
        if (m_camera_apply_setup_recv_fn)
            m_camera_apply_setup_recv_fn(tx, ty, tz, distance, pitch_rad, yaw_rad, duration);
        break;
    }
    case MsgType::S_CAMERA_SET_TARGET_POSITION: {
        ByteReader r(data); r.read_u8();
        f32 x = r.read_f32(), y = r.read_f32(), z = r.read_f32(), dur = r.read_f32();
        if (m_camera_set_target_position_recv_fn) m_camera_set_target_position_recv_fn(x, y, z, dur);
        break;
    }
    case MsgType::S_CAMERA_SET_SOURCE_DISTANCE: {
        ByteReader r(data); r.read_u8();
        f32 distance = r.read_f32(), dur = r.read_f32();
        if (m_camera_set_source_distance_recv_fn) m_camera_set_source_distance_recv_fn(distance, dur);
        break;
    }
    case MsgType::S_CAMERA_SHAKE: {
        ByteReader r(data); r.read_u8();
        f32 intensity = r.read_f32(), dur = r.read_f32();
        if (m_camera_shake_recv_fn) m_camera_shake_recv_fn(intensity, dur);
        break;
    }
    case MsgType::S_CAMERA_SET_TARGET_CONTROLLER: {
        ByteReader r(data); r.read_u8();
        u32 entity_id = r.read_u32();
        if (m_camera_set_target_controller_recv_fn) m_camera_set_target_controller_recv_fn(entity_id);
        break;
    }
    case MsgType::S_SET_CONTROLLED_UNIT: {
        ByteReader r(data); r.read_u8();
        u32 entity_id = r.read_u32();
        if (m_set_controlled_unit_recv_fn) m_set_controlled_unit_recv_fn(entity_id);
        break;
    }
    // HUD sync — opcodes 0x70..0x7D. Forward the raw payload to the
    // App-installed handler (which invokes hud::apply_network_message).
    // Keeping the decode out of NetworkManager lets the server drop the
    // hud library entirely.
    case MsgType::S_HUD_CREATE_NODE:
    case MsgType::S_HUD_DESTROY_NODE:
    case MsgType::S_HUD_SET_LABEL_TEXT:
    case MsgType::S_HUD_SET_BAR_FILL:
    case MsgType::S_HUD_SET_NODE_VISIBLE:
    case MsgType::S_HUD_SET_IMAGE_SOURCE:
    case MsgType::S_HUD_SET_BUTTON_ENABLED:
    case MsgType::S_HUD_CREATE_TEXT_TAG:
    case MsgType::S_HUD_DESTROY_TEXT_TAG:
    case MsgType::S_HUD_SET_TEXT_TAG_TEXT:
    case MsgType::S_HUD_DISPLAY_MESSAGE:
    case MsgType::S_USER_CONTROL_ENABLED:
    case MsgType::S_ORIGIN_HUD_VISIBLE: {
        if (m_hud_message_fn) m_hud_message_fn(data);
        break;
    }

    default:
        log::warn(TAG, "Client received unknown message type 0x{:02x}", static_cast<u8>(type));
        break;
    }
}

void NetworkManager::host_hud_sync(const std::vector<u8>& packet, u32 players_mask) {
    if (m_mode != Mode::Host || !m_transport) return;
    if (players_mask == UINT32_MAX) {
        // Broadcast — every connected peer sees it.
        m_transport->broadcast(packet, true);
        return;
    }
    // Targeted — send to each peer whose player bit is set in the mask.
    // Peers outside the mask never know the node exists.
    for (const auto& p : m_peers) {
        if (players_mask & (1u << p.player.id)) {
            m_transport->send(p.peer_id, packet, true);
        }
    }
    // Players in the mask without a peer (disconnected, or the host
    // itself plays one of them) — the host's own Hud already applied
    // the mutation locally at the Lua binding layer, so nothing else
    // to do.
}

void NetworkManager::send_node_event(std::string_view node_id, NodeEventKind kind) {
    if (m_mode != Mode::Client || !m_transport) return;
    auto msg = build_node_event(node_id, kind);
    m_transport->send(0, msg, true);   // host is peer 0 from the client's view
}

void NetworkManager::send_claim_slot(u32 slot) {
    auto msg = build_claim_slot(slot);
    if (m_mode == Mode::Host || m_mode == Mode::Offline) {
        // Host/Offline mutate locally (and Host broadcasts). Route through
        // host_on_receive with the LOCAL_PEER sentinel so the slot-bookkeeping
        // logic lives in one place and never collides with real ENet peer ids
        // (which start at 0). Offline skips the broadcast inside.
        host_on_receive(LOCAL_PEER, msg);
    } else if (m_mode == Mode::Client && m_transport) {
        m_transport->send(0, msg, true);
    }
}

void NetworkManager::send_release_slot(u32 slot) {
    auto msg = build_release_slot(slot);
    if (m_mode == Mode::Host || m_mode == Mode::Offline) {
        host_on_receive(LOCAL_PEER, msg);
    } else if (m_mode == Mode::Client && m_transport) {
        m_transport->send(0, msg, true);
    }
}


void NetworkManager::client_handle_welcome(std::span<const u8> data) {
    auto w = parse_welcome(data);
    m_local_player = simulation::Player{w.player_id};
    m_connected = true;
    log::info(TAG, "Welcome! Assigned player {}, {} players, {} tick/s",
              w.player_id, w.player_count, w.tick_rate);

    // Determinism guard: the client built preplaced entities [0, N) locally
    // from its own placements.bin, so its allocator should sit at exactly N.
    // A mismatch means host/client disagree on the placement set (map skew,
    // non-deterministic load) — every subsequent id-keyed message would land
    // on the wrong entity, so this is UNRECOVERABLE. Rather than log-and-limp
    // with silently diverged worlds, treat it like a fatal disconnect: tear the
    // connection down and drop m_connected so the App surfaces the same
    // "Lost connection / no longer in sync" dialog it shows for a dropped host.
    u32 client_n = m_client->simulation().world().entities.next_id();
    if (client_n != w.placement_count) {
        log::error(TAG, "PLACEMENT DESYNC: host placement_count={} but client built {} "
                        "placement entities — flagging fatal (worlds would diverge)",
                   w.placement_count, client_n);
        // Drop the connection flag so the App surfaces the same "Lost connection /
        // no longer in sync" dialog it shows for a dropped host, then bail out of
        // the welcome (don't proceed as if joined). Do NOT tear down the transport
        // here — this runs inside the transport's own receive callback (poll →
        // enet_host_service → on_receive), and enet_host_destroy mid-service is a
        // use-after-free. The App's EndSession → shutdown() destroys it safely.
        m_connected = false;
        return;
    }

    // Resume the client from the scene-switch barrier. S_SCENE_SWITCH set
    // m_scene_switching=true + Phase::Loading on this client; only the host
    // cleared its own copy (host_finish_scene_switch). Without this the
    // client's per-frame vision update + view projection stay gated off
    // forever → permanent fog, own units never revealed. The re-welcome is
    // the host's "new scene ready" signal, reliable-ordered just before the
    // spawn burst, so clearing here resumes cleanly. Harmless at first join
    // (already Playing / not switching); S_START still handles that path.
    if (m_scene_switching) {
        m_scene_switching = false;
        m_phase = Phase::Playing;
    }
}

void NetworkManager::client_handle_sound(std::span<const u8> data) {
    auto s = parse_sound(data);
    if (on_sound) on_sound(s.path, s.pos);
}

void NetworkManager::client_handle_effect_create(std::span<const u8> data) {
    auto e = parse_effect_create(data);
    if (on_effect_create) on_effect_create(e.server_id, e.name, e.entity, e.pos,
                                           e.attach_point);
}

void NetworkManager::client_handle_effect_destroy(std::span<const u8> data) {
    u32 id = parse_effect_destroy(data);
    if (on_effect_destroy) on_effect_destroy(id);
}

void NetworkManager::send_order(const simulation::GameCommand& cmd) {
    if (m_mode != Mode::Client || !m_transport) return;
    auto msg = build_order(cmd);
    m_transport->send(0, msg, true);
}

void NetworkManager::update(f32 dt) {
    if (m_mode == Mode::Offline) return;
    if (m_transport) m_transport->poll();

    // Host: tick disconnect timeouts
    if (m_mode == Mode::Host) {
        host_update_disconnected(dt);
    }

    // Client derivation (cooldown decay, projectile teardown, fog) lives in
    // Simulation::client_tick, driven from the engine's client branch. Corpse
    // lifecycle is host-driven: the client never advances corpse timers or hides a
    // corpse (visibility rides the synced `hidden` bit; corpse teardown is S_DESTROY).
}

void NetworkManager::shutdown() {
    if (m_transport) {
        m_transport->disconnect();
        m_transport.reset();
    }
    m_client = nullptr;
    m_server = nullptr;
    m_peers.clear();
    m_connected = false;
    m_mode = Mode::Offline;
    m_phase = Phase::None;
    m_game_started = false;
    m_game_ended = false;
    m_client_peer_id = UINT32_MAX;
    m_local_player = simulation::Player{UINT32_MAX};
    m_lobby = LobbyState{};
    m_paused = false;
    m_pause_view_active = false;
    m_pause_broadcast_timer = 0.0f;
    m_disconnected.clear();
    m_disconnected_view.clear();
}

} // namespace uldum::network
