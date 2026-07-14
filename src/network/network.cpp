#include "network/network.h"
#include "network/transport.h"
#include "network/enet_transport.h"
#include "network/protocol.h"
#include "simulation/simulation.h"
#include "simulation/type_registry.h"
#include "simulation/ability_def.h"
#include "simulation/world.h"
#include "simulation/command_system.h"
#include "map/map.h"
#include "map/terrain_data.h"
#include "script/script.h"
#include "core/log.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <chrono>

namespace uldum::network {

static constexpr const char* TAG = "Network";

NetworkManager::NetworkManager() = default;
NetworkManager::~NetworkManager() = default;

static f64 wall_time() {
    using namespace std::chrono;
    return duration<f64>(steady_clock::now().time_since_epoch()).count();
}

// ── Offline ──────────────────────────────────────────────────────────────

bool NetworkManager::init_offline() {
    m_mode = Mode::Offline;
    m_phase = Phase::Lobby;   // so the slot-claim handler accepts edits
    m_connected = false;
    log::info(TAG, "NetworkManager initialized — mode=Offline");
    return true;
}

// ── Host ─────────────────────────────────────────────────────────────────

bool NetworkManager::init_host(u16 port, u32 max_players,
                               simulation::Simulation& simulation,
                               simulation::CommandSystem& commands) {
    m_simulation = &simulation;
    m_commands = &commands;

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
        dp.known_entities  = std::move(it->known_entities);
        dp.timer           = m_disconnect_timeout;
        dp.auth_token      = std::move(it->auth_token);
        dp.player_name     = std::move(it->player_name);
        m_disconnected.push_back(std::move(dp));
        m_peers.erase(it);

        if (m_pause_on_disconnect && m_game_started) {
            m_paused = true;
            log::info(TAG, "Game paused — waiting for reconnect");
        }

        if (on_player_disconnected) on_player_disconnected(player_id);
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

    // S_WELCOME + spawn burst per seated peer, then broadcast S_START.
    for (auto& peer : m_peers) {
        if (!peer.player.is_valid()) continue;
        auto welcome = build_welcome(peer.player.id,
            static_cast<u32>(m_simulation->world().handle_infos.count()), 32);
        m_transport->send(peer.peer_id, welcome, true);
        host_send_spawn_burst(peer);
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

// ── Scripted-camera routing ──────────────────────────────────────────────

namespace {
// Walk the peer list to map a player id to its peer transport id.
// Returns UINT32_MAX if no peer owns that slot (e.g. host's own slot,
// disconnected player, dedicated-server case where no human plays).
template <typename Peers>
u32 peer_id_for_player(const Peers& peers, u32 player_id) {
    for (const auto& p : peers) {
        if (p.player.id == player_id) return p.peer_id;
    }
    return UINT32_MAX;
}
} // namespace

bool NetworkManager::host_send_camera_apply_setup(u32 player_id,
        f32 tx, f32 ty, f32 tz, f32 distance,
        f32 pitch_rad, f32 yaw_rad, f32 duration) {
    if (m_mode != Mode::Host || !m_transport) return false;
    u32 peer = peer_id_for_player(m_peers, player_id);
    if (peer == UINT32_MAX) return false;
    auto msg = build_camera_apply_setup(tx, ty, tz, distance,
                                          pitch_rad, yaw_rad, duration);
    m_transport->send(peer, msg, true);
    return true;
}

bool NetworkManager::host_send_camera_set_target_position(u32 player_id,
        f32 x, f32 y, f32 z, f32 duration) {
    if (m_mode != Mode::Host || !m_transport) return false;
    u32 peer = peer_id_for_player(m_peers, player_id);
    if (peer == UINT32_MAX) return false;
    auto msg = build_camera_set_target_position(x, y, z, duration);
    m_transport->send(peer, msg, true);
    return true;
}

bool NetworkManager::host_send_camera_set_source_distance(u32 player_id,
        f32 distance, f32 duration) {
    if (m_mode != Mode::Host || !m_transport) return false;
    u32 peer = peer_id_for_player(m_peers, player_id);
    if (peer == UINT32_MAX) return false;
    auto msg = build_camera_set_source_distance(distance, duration);
    m_transport->send(peer, msg, true);
    return true;
}

bool NetworkManager::host_send_camera_shake(u32 player_id,
        f32 intensity, f32 duration) {
    if (m_mode != Mode::Host || !m_transport) return false;
    u32 peer = peer_id_for_player(m_peers, player_id);
    if (peer == UINT32_MAX) return false;
    auto msg = build_camera_shake(intensity, duration);
    m_transport->send(peer, msg, true);
    return true;
}

bool NetworkManager::host_send_camera_set_target_controller(u32 player_id,
        u32 entity_id) {
    if (m_mode != Mode::Host || !m_transport) return false;
    u32 peer = peer_id_for_player(m_peers, player_id);
    if (peer == UINT32_MAX) return false;
    auto msg = build_camera_set_target_controller(entity_id);
    m_transport->send(peer, msg, true);
    return true;
}

void NetworkManager::host_finish_scene_switch() {
    if (m_mode != Mode::Host || !m_scene_switching) return;

    // Spawn burst per peer for the new scene's entities. host_send_spawn_burst
    // resends every entity currently in the world; since switch_scene cleared
    // the previous scene, only new-scene entities are visible.
    for (auto& peer : m_peers) {
        if (!peer.player.is_valid()) continue;
        peer.known_entities.clear();
        host_send_spawn_burst(peer);
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
                PeerInfo info{peer_id, simulation::Player{UINT32_MAX}, std::move(peer_name),
                              false, {}, client_token};
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
            PeerInfo info{peer_id, it->player, std::move(restored_name),
                          false, std::move(it->known_entities), std::move(it->auth_token)};
            m_disconnected.erase(it);

            auto welcome = build_welcome(slot,
                static_cast<u32>(m_simulation->world().handle_infos.count()), 32);
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
                fresh.known_entities.clear();
                fresh.loaded = false;
                if (!m_in_flight_scene_name.empty()) {
                    auto msg = build_scene_switch(m_in_flight_scene_name);
                    m_transport->send(peer_id, msg, true);
                }
                log::info(TAG, "Player {} reconnected mid-scene-switch (peer {}) — joining barrier",
                          slot, peer_id);
            } else {
                host_send_spawn_burst(m_peers.back());

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

        auto cmd = parse_order(data, peer->player);
        if (!cmd) {
            log::warn(TAG, "Peer {} sent a malformed C_ORDER — dropped", peer_id);
            break;
        }
        m_commands->submit(*cmd);
        break;
    }

    case MsgType::C_LEAVE: {
        log::info(TAG, "Peer {} sent C_LEAVE", peer_id);
        break;
    }

    case MsgType::C_NODE_EVENT: {
        const PeerInfo* peer = find_peer(peer_id);
        if (!peer || !peer->player.is_valid()) return;

        ByteReader r(data);
        r.read_u8();
        std::string node_id = r.read_string();
        NodeEventKind kind  = static_cast<NodeEventKind>(r.read_u8());

        if (m_script && kind == NodeEventKind::ButtonPressed) {
            m_script->fire_node_event("button_pressed", peer->player.id, node_id);
        }
        break;
    }

    default:
        log::warn(TAG, "Host received unknown message type 0x{:02x}", static_cast<u8>(type));
        break;
    }
}

void NetworkManager::host_send_spawn(PeerInfo& peer, u32 entity_id,
                                     const simulation::HandleInfo& info,
                                     bool newly_created) {
    auto& world = m_simulation->world();
    const auto* transform = world.transforms.get(entity_id);
    if (!transform) return;

    const auto* owner = world.owners.get(entity_id);
    u8 owner_id = owner ? static_cast<u8>(owner->id) : 0;
    std::string_view model_path;
    if (info.category == simulation::Category::Projectile) {
        if (auto* renderable = world.renderables.get(entity_id)) model_path = renderable->model_path;
    }

    auto msg = build_spawn(entity_id, info.type_id, owner_id,
                           transform->position.x, transform->position.y,
                           transform->facing, newly_created, model_path);
    m_transport->send(peer.peer_id, msg, true);
    peer.known_entities.insert(entity_id);
}

void NetworkManager::host_send_spawn_burst(PeerInfo& peer) {
    auto& world = m_simulation->world();
    auto& infos = world.handle_infos;

    for (u32 i = 0; i < infos.count(); ++i) {
        u32 id = infos.ids()[i];
        if (!is_visible_to(id, peer.player)) continue;
        host_send_spawn(peer, id, infos.data()[i], false);
    }

    log::info(TAG, "Sent {} entities to player {}", peer.known_entities.size(), peer.player.id);
}

bool NetworkManager::is_visible_to(u32 entity_id, simulation::Player player) const {
    // Server-side snapshot filter — refuses to serialize unit data for
    // a peer that cannot see it. Shares logic with the renderer's
    // is_fog_hidden via Vision::is_unit_visible_to so we can never
    // ship invisibility data the client could mine for cheats.
    return m_simulation->vision().is_unit_visible_to(
        m_simulation->world(), *m_simulation, entity_id, player);
}

bool NetworkManager::is_visible_or_remembered_to(u32 entity_id, simulation::Player player) const {
    // Variant used to decide whether a static-category entity (tree,
    // doodad, building) should be shipped to / kept on the client.
    // Explored tiles count as visible for these; the client keeps the
    // last-seen state until the player re-scouts.
    return m_simulation->vision().is_unit_visible_to(
        m_simulation->world(), *m_simulation, entity_id, player, /*remembered_ok=*/true);
}

void NetworkManager::host_broadcast_tick(u32 tick) {
    if (m_mode != Mode::Host || m_peers.empty()) return;

    auto& world = m_simulation->world();
    auto& infos = world.handle_infos;

    auto last_tick = std::move(m_prev_tick_entities);
    m_prev_tick_entities.clear();
    m_prev_tick_entities.reserve(infos.count());
    for (u32 id : infos.ids()) m_prev_tick_entities.insert(id);

    for (auto& peer : m_peers) {
        std::unordered_set<u32> visible_now;
        visible_now.reserve(infos.count());
        std::vector<EntityState> states;
        states.reserve(infos.count());

        for (u32 i = 0; i < infos.count(); ++i) {
            u32 id = infos.ids()[i];
            const auto& info = infos.data()[i];

            const bool remembered  = simulation::is_static_remembered_entity(world, id);
            const bool live_vis    = is_visible_to(id, peer.player);
            const bool keep_in_view = live_vis ||
                (remembered && is_visible_or_remembered_to(id, peer.player));
            if (!keep_in_view) continue;
            visible_now.insert(id);

            if (!peer.known_entities.contains(id)) {
                bool newly_created = !last_tick.contains(id);
                host_send_spawn(peer, id, info, newly_created);
            }

            // Static-remembered entities frozen in fog don't ship S_STATE
            // updates while the player can't see them live. Skipping the
            // state record below preserves the last-seen snapshot on the
            // client (and saves bandwidth).
            if (remembered && !live_vis) continue;

            // Build state record
            const auto* transform = world.transforms.get(id);
            if (!transform) continue;

            EntityState es{};
            es.entity_id = id;
            es.x = transform->position.x;
            es.y = transform->position.y;
            es.z = transform->position.z;
            es.facing = transform->facing;

            const auto* hp = world.healths.get(id);
            es.health_frac = hp ? (hp->max > 0 ? hp->current / hp->max : 1.0f) : 1.0f;

            u8 flags = 0;
            const auto* mov = world.movements.get(id);
            if (mov && mov->moving) flags |= 0x01;
            const auto* combat = world.combats.get(id);
            if (combat && combat->target.id != UINT32_MAX) {
                es.target_id = combat->target.id;
                // Only set attacking flag during actual attack (not while moving to target)
                if (combat->attack_state == simulation::AttackState::WindUp ||
                    combat->attack_state == simulation::AttackState::Backswing ||
                    combat->attack_state == simulation::AttackState::Cooldown) {
                    flags |= 0x02;
                }
            }
            if (world.dead_states.has(id)) flags |= 0x08;
            es.flags = flags;

            states.push_back(es);
        }

        // Send S_DESTROY for entities that left visibility or were destroyed
        std::vector<u32> to_remove;
        for (u32 known_id : peer.known_entities) {
            if (!visible_now.contains(known_id)) {
                auto msg = build_destroy(known_id);
                m_transport->send(peer.peer_id, msg, true);
                to_remove.push_back(known_id);
            }
        }
        for (u32 id : to_remove) peer.known_entities.erase(id);

        // Send S_STATE
        if (!states.empty()) {
            auto msg = build_state(tick, states);
            m_transport->send(peer.peer_id, msg, false);  // unreliable
        }
    }

}

void NetworkManager::host_update_disconnected(f32 dt) {
    bool changed = false;
    for (auto it = m_disconnected.begin(); it != m_disconnected.end(); ) {
        it->timer -= dt;
        if (it->timer <= 0) {
            u32 player_id = it->player.id;
            log::info(TAG, "Player {} reconnect timeout expired — dropped", player_id);
            if (on_player_dropped) on_player_dropped(player_id);
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

void NetworkManager::host_broadcast_update(u32 entity_id, std::span<const u8> update_packet) {
    if (m_mode != Mode::Host || m_peers.empty() || !m_simulation) return;
    const auto* info = m_simulation->world().handle_infos.get(entity_id);
    if (!info) return;
    for (auto& peer : m_peers) {
        if (peer.known_entities.contains(entity_id)) {
            m_transport->send(peer.peer_id, update_packet, true);
        }
    }
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

void NetworkManager::host_end_game(u32 winner_id, std::string_view stats_json) {
    if (m_mode != Mode::Host) return;
    m_game_ended = true;
    m_end_data = EndData{winner_id, std::string(stats_json)};
    auto msg = build_end(winner_id, stats_json);
    m_transport->broadcast(msg, true);
    log::info(TAG, "Game ended — winner: player {}", winner_id);
}

// ── Client ───────────────────────────────────────────────────────────────

bool NetworkManager::init_client(std::string_view address, u16 port) {
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
    case MsgType::S_SPAWN:   client_handle_spawn(data); break;
    case MsgType::S_DESTROY: client_handle_destroy(data); break;
    case MsgType::S_STATE:   client_handle_state(data); break;
    case MsgType::S_SOUND:           client_handle_sound(data); break;
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
    case MsgType::S_PROJECTILE_DYING: client_handle_projectile_dying(data); break;
    case MsgType::S_UPDATE: client_handle_update(data); break;
    case MsgType::S_START:
        m_game_started = true;
        m_phase = Phase::Playing;
        log::info(TAG, "Game started!");
        if (on_lobby_start) on_lobby_start();
        break;
    case MsgType::S_END: {
        m_end_data = parse_end(data);
        m_game_ended = true;
        log::info(TAG, "Game ended — winner: player {}", m_end_data.winner_id);
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

    // HUD sync — opcodes 0x70..0x78. Forward the raw payload to the
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
    case MsgType::S_HUD_DISPLAY_MESSAGE: {
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
}

void NetworkManager::client_handle_spawn(std::span<const u8> data) {
    auto s = parse_spawn(data);
    spawn_client_entity(s.entity_id, s.type_id, s.owner, s.x, s.y, s.facing,
                        s.newly_created, s.model_path);
}

void NetworkManager::client_handle_destroy(std::span<const u8> data) {
    u32 id = parse_destroy(data);
    destroy_client_entity(id);
}

void NetworkManager::client_handle_state(std::span<const u8> data) {
    auto s = parse_state(data);

    // Swap snapshots: current newer becomes older
    u32 older = m_snap_idx;
    u32 newer = 1 - m_snap_idx;
    m_snapshots[newer].tick = s.tick;
    m_snapshots[newer].receive_time = wall_time();
    m_snapshots[newer].entities = std::move(s.entities);
    m_snap_idx = newer;

    if (!m_has_two_snaps && m_snapshots[older].tick > 0) {
        m_has_two_snaps = true;
    }

    // Apply latest state immediately (interpolation smooths between frames)
    client_apply_interpolation();
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

void NetworkManager::client_handle_projectile_dying(std::span<const u8> data) {
    u32 id = parse_projectile_dying(data);
    auto& world = m_client_world;
    // Queue the model's "death" clip on the local entity so the
    // renderer plays it over the dying window. The server will follow
    // up with S_DESTROY when the entity is actually torn down.
    simulation::AnimQueue aq;
    aq.clips.push_back("death");
    aq.looping = false;
    if (auto* q = world.anim_queues.get(id)) {
        *q = std::move(aq);
    } else if (world.handle_infos.has(id)) {
        world.anim_queues.add(id, std::move(aq));
    }
}

void NetworkManager::client_apply_interpolation() {
    if (!m_has_two_snaps) {
        // Only one snapshot — snap to it directly
        auto& snap = m_snapshots[m_snap_idx];
        for (auto& e : snap.entities) {
            auto* t = m_client_world.transforms.get(e.entity_id);
            if (!t) continue;
            t->prev_position = t->position;
            t->prev_facing = t->facing;
            t->position = {e.x, e.y, e.z};
            t->facing = e.facing;
        }
        return;
    }

    u32 newer = m_snap_idx;
    u32 older = 1 - m_snap_idx;
    auto& snap_old = m_snapshots[older];
    auto& snap_new = m_snapshots[newer];

    f64 tick_dur = 1.0 / 32.0;
    f64 now = wall_time();
    f64 render_time = now - tick_dur;  // render one tick behind

    f32 alpha = 0.0f;
    f64 span = snap_new.receive_time - snap_old.receive_time;
    if (span > 0.0) {
        alpha = static_cast<f32>((render_time - snap_old.receive_time) / span);
        alpha = std::clamp(alpha, 0.0f, 1.0f);
    }

    // Build lookup for older snapshot
    std::unordered_map<u32, const EntityState*> old_lookup;
    for (auto& e : snap_old.entities) old_lookup[e.entity_id] = &e;

    for (auto& e : snap_new.entities) {
        auto* t = m_client_world.transforms.get(e.entity_id);
        if (!t) continue;

        auto it = old_lookup.find(e.entity_id);
        if (it != old_lookup.end()) {
            auto* o = it->second;
            // Interpolate position
            t->prev_position = t->position;
            t->position.x = o->x + (e.x - o->x) * alpha;
            t->position.y = o->y + (e.y - o->y) * alpha;
            t->position.z = o->z + (e.z - o->z) * alpha;

            // Angular interpolation for facing
            f32 diff = e.facing - o->facing;
            while (diff > glm::pi<f32>()) diff -= glm::two_pi<f32>();
            while (diff < -glm::pi<f32>()) diff += glm::two_pi<f32>();
            t->prev_facing = t->facing;
            t->facing = o->facing + diff * alpha;
        } else {
            // New entity — snap
            t->prev_position = t->position = {e.x, e.y, e.z};
            t->prev_facing = t->facing = e.facing;
        }

        // Update health
        auto* hp = m_client_world.healths.get(e.entity_id);
        if (hp) hp->current = e.health_frac * hp->max;

        // Update components from server flags
        auto* mov = m_client_world.movements.get(e.entity_id);
        if (mov) mov->moving = (e.flags & 0x01) != 0;

        // Combat state: simulate server attack cycle on client.
        // Server cycles: WindUp (dmg_time) → Backswing (backsw_time) → Cooldown → WindUp.
        // Client receives a single "attacking" flag. We cycle attack_timer to match.
        auto* combat = m_client_world.combats.get(e.entity_id);
        if (combat) {
            if (e.flags & 0x02) {
                combat->target = simulation::Unit{e.target_id};
                if (combat->attack_state == simulation::AttackState::Idle) {
                    // Start new attack cycle
                    combat->attack_state = simulation::AttackState::WindUp;
                    combat->attack_timer = combat->dmg_time;
                }
                // attack_timer is advanced per-frame below
            } else {
                combat->attack_state = simulation::AttackState::Idle;
                combat->target = simulation::Unit{};
                combat->attack_timer = 0;
            }
        }

        // Dead state
        if (e.flags & 0x08) {
            if (!m_client_world.dead_states.has(e.entity_id)) {
                m_client_world.dead_states.add(e.entity_id, simulation::DeadState{});
            }
        } else {
            // Server says not dead — remove stale dead state if present
            m_client_world.dead_states.remove(e.entity_id);
        }
    }
}

void NetworkManager::client_handle_update(std::span<const u8> data) {
    auto u = parse_update(data);
    auto& world = m_client_world;

    switch (u.type) {
    case UpdateType::Attribute: {
        auto* attrs = world.attribute_blocks.get(u.entity_id);
        if (!attrs) {
            world.attribute_blocks.add(u.entity_id, simulation::AttributeBlock{});
            attrs = world.attribute_blocks.get(u.entity_id);
        }
        attrs->numeric[u.key] = u.value;
        break;
    }
    case UpdateType::StringAttribute: {
        auto* attrs = world.attribute_blocks.get(u.entity_id);
        if (!attrs) {
            world.attribute_blocks.add(u.entity_id, simulation::AttributeBlock{});
            attrs = world.attribute_blocks.get(u.entity_id);
        }
        attrs->string_attrs[u.key] = u.str_value;
        break;
    }
    case UpdateType::State: {
        auto* states = world.state_blocks.get(u.entity_id);
        if (!states) {
            world.state_blocks.add(u.entity_id, simulation::StateBlock{});
            states = world.state_blocks.get(u.entity_id);
        }
        auto& state = states->states[u.key];
        state.current = u.value;
        state.max = u.value2;
        break;
    }
    case UpdateType::AbilityAdd: {
        // Mirror server-side add_ability: populate active_modifiers /
        // active_flags from the def, bump per-flag refcounts, and run
        // recalculate_modifiers so the client's renderable visual_alpha
        // (and any other derived values) match the server. Without
        // this, passive_flag buffs like windwalk_invisible would have
        // no effect on the client — the unit wouldn't appear invisible
        // or carry its alpha modifier even though it does on the host.
        auto* aset = world.ability_sets.get(u.entity_id);
        if (!aset) {
            world.ability_sets.add(u.entity_id, simulation::AbilitySet{});
            aset = world.ability_sets.get(u.entity_id);
        }
        const simulation::AbilityDef* def =
            m_client_abilities ? m_client_abilities->get(u.key) : nullptr;

        simulation::AbilitySourceKind source_kind =
            static_cast<simulation::AbilitySourceKind>(u.byte_value);
        simulation::AbilitySource source;
        if (source_kind == simulation::AbilitySourceKind::Item) {
            source.value = simulation::ItemAbilitySource{};
        } else if (source_kind == simulation::AbilitySourceKind::Applied) {
            source.value = simulation::AppliedAbilitySource{};
        } else {
            source.value = simulation::InnateAbilitySource{};
        }
        source.remaining_duration = def
            ? def->level_data(u.uint_value).duration
            : -1.0f;

        simulation::AbilityInstance* instance = nullptr;
        if (def && !def->stackable) {
            for (auto& candidate : aset->abilities) {
                if (candidate.ability_id == u.key) {
                    instance = &candidate;
                    break;
                }
            }
        }

        if (instance) {
            if (source_kind == simulation::AbilitySourceKind::Item) {
                instance->sources.push_back(source);
            } else {
                auto existing = std::find_if(
                    instance->sources.begin(), instance->sources.end(),
                    [&](const simulation::AbilitySource& candidate) {
                        return simulation::ability_source_kind(candidate) == source_kind;
                    });
                if (existing != instance->sources.end()) {
                    existing->remaining_duration = source.remaining_duration;
                } else {
                    instance->sources.push_back(source);
                }
            }
        } else {
            simulation::AbilityInstance created;
            created.ability_id = u.key;
            created.level = u.uint_value;
            created.sources.push_back(source);
            if (def) {
                auto& lvl = def->level_data(u.uint_value);
                created.active_modifiers = lvl.modifiers;
                created.active_flags     = lvl.flags;
            }
            auto flags_snapshot = created.active_flags;
            aset->abilities.push_back(std::move(created));
            simulation::flag_refcount_delta(world, u.entity_id, flags_snapshot, +1);
        }
        simulation::recalculate_modifiers(world, u.entity_id);
        break;
    }
    case UpdateType::AbilityRemove: {
        auto* aset = world.ability_sets.get(u.entity_id);
        if (aset) {
            simulation::AbilitySourceKind source_kind =
                static_cast<simulation::AbilitySourceKind>(u.byte_value);
            auto& abilities = aset->abilities;
            for (auto instance = abilities.begin();
                 instance != abilities.end(); ) {
                if (instance->ability_id != u.key) {
                    ++instance;
                    continue;
                }

                if (u.bool_value) {
                    simulation::flag_refcount_delta(
                        world, u.entity_id, instance->active_flags, -1);
                    instance = abilities.erase(instance);
                    continue;
                }

                auto source = std::find_if(
                    instance->sources.begin(), instance->sources.end(),
                    [&](const simulation::AbilitySource& candidate) {
                        return simulation::ability_source_kind(candidate) == source_kind;
                    });
                if (source == instance->sources.end()) {
                    ++instance;
                    continue;
                }
                instance->sources.erase(source);
                if (instance->sources.empty()) {
                    simulation::flag_refcount_delta(
                        world, u.entity_id, instance->active_flags, -1);
                    abilities.erase(instance);
                }
                break;
            }
            simulation::recalculate_modifiers(world, u.entity_id);
        }
        break;
    }
    case UpdateType::Owner: {
        auto* owner = world.owners.get(u.entity_id);
        if (owner) owner->id = u.uint_value;
        break;
    }
    case UpdateType::AbilityModifier: {
        // Mirror SetAbilityModifier: mutate the named modifier on every
        // matching instance and recalculate. Drives Lua-side tweens
        // like Wind Walk's fade-in.
        auto* aset = world.ability_sets.get(u.entity_id);
        if (!aset) break;
        bool changed = false;
        for (auto& a : aset->abilities) {
            if (a.ability_id == u.key) {
                a.active_modifiers[u.str_value] = u.value;
                changed = true;
            }
        }
        if (changed) simulation::recalculate_modifiers(world, u.entity_id);
        break;
    }
    case UpdateType::Status: {
        // Mirror SetUnitStatus on the client — manual_bits layer only;
        // ability-driven refcounts arrive through AbilityAdd/Remove
        // already and compose with this via recompute_effective_flags
        // inside set_unit_status.
        simulation::Unit unit = world.unit(u.entity_id);
        simulation::set_unit_status(world, unit, u.uint_value, u.bool_value);
        break;
    }
    case UpdateType::Cooldown: {
        // Apply the cooldown to every instance of `ability_id` on the
        // unit — matches the server's SetAbilityCooldown / Reset
        // semantics (which iterate all matching instances).
        auto* aset = world.ability_sets.get(u.entity_id);
        if (aset) {
            for (auto& a : aset->abilities) {
                if (a.ability_id == u.key) {
                    a.cooldown_remaining = std::max(0.0f, u.value);
                }
            }
        }
        break;
    }
    case UpdateType::ItemCharges:
        simulation::set_charges(
            world, simulation::Item{u.entity_id}, static_cast<i32>(u.uint_value));
        break;
    case UpdateType::ItemLevel:
        simulation::set_level(
            world, simulation::Item{u.entity_id}, static_cast<i32>(u.uint_value));
        break;
    }
}

void NetworkManager::spawn_client_entity(u32 entity_id,
                                          std::string_view type_id,
                                          u8 owner, f32 x, f32 y, f32 facing,
                                          bool newly_created,
                                          std::string_view model_path_override) {
    auto& world = m_client_world;

    // Skip if already exists
    if (world.handle_infos.has(entity_id)) return;

    world.handles.reserve(entity_id);

    // Projectiles take a minimal-components path — no Health, no Combat,
    // no Movement, no Owner, no Selectable. They render but don't
    // simulate; the host's authoritative position arrives via S_STATE
    // and the renderer drives the model + anim through the renderable
    // + anim_queue path. The model path comes inline from S_SPAWN
    // since projectiles aren't in the type registry.
    if (type_id == "projectile") {
        world.handle_infos.add(entity_id,
            simulation::HandleInfo{std::string(type_id), simulation::Category::Projectile});
        simulation::Transform t;
        t.position = glm::vec3{x, y, 0};
        t.prev_position = t.position;
        t.facing = facing;
        t.prev_facing = facing;
        // Mirror create_projectile's scale rule: placeholder mesh is a
        // tiny stub; glTF projectiles render at their authored size.
        t.scale = model_path_override.empty() ? 0.3f : 1.0f;
        world.transforms.add(entity_id, std::move(t));
        std::string model{model_path_override.empty() ? "projectile" : model_path_override};
        world.renderables.add(entity_id, simulation::Renderable{std::move(model), true});
        world.projectiles.add(entity_id, simulation::ProjectileComp{});
        return;
    }

    // Determine category from type
    simulation::Category cat = simulation::Category::Unit;
    std::string model_path = "placeholder";
    f32 sight_range = 0;
    f32 selection_radius = 1.0f;
    f32 scale = 1.0f;
    f32 max_health = 100.0f;
    f32 collision_radius = 28.0f;

    // Look up type def for model path and other render-relevant data
    if (m_client_types) {
        auto* unit_def = m_client_types->get_unit_type(type_id);
        if (unit_def) {
            if (!unit_def->model_path.empty()) model_path = unit_def->model_path;
            sight_range = unit_def->sight_range;
            selection_radius = unit_def->selection_radius;
            scale = unit_def->model_scale;
            max_health = unit_def->max_health;
            collision_radius = unit_def->collision_radius;
        } else {
            auto* destr_def = m_client_types->get_destructable_type(type_id);
            if (destr_def) {
                cat = simulation::Category::Destructable;
                if (!destr_def->models.empty()) model_path = destr_def->models[0];
                max_health = destr_def->max_health;
            }
        }
    }

    world.handle_infos.add(entity_id,
        simulation::HandleInfo{std::string(type_id), cat});

    simulation::Transform t;
    t.position = glm::vec3{x, y, 0};
    t.prev_position = t.position;
    t.facing = facing;
    t.prev_facing = facing;
    t.scale = scale;
    world.transforms.add(entity_id, std::move(t));
    world.owners.add(entity_id, simulation::Player{owner});
    world.renderables.add(entity_id, simulation::Renderable{model_path, true, !newly_created});
    world.healths.add(entity_id, simulation::Health{max_health, max_health, 0});
    world.movements.add(entity_id, simulation::Movement{});
    world.movements.get(entity_id)->collision_radius = collision_radius;
    {
        simulation::Combat combat{};
        if (m_client_types) {
            auto* ud = m_client_types->get_unit_type(type_id);
            if (ud) {
                combat.dmg_pt = ud->dmg_pt;
                combat.dmg_time = ud->dmg_time;
                combat.backsw_time = ud->backsw_time;
                combat.attack_cooldown = ud->attack_cooldown;
            }
        }
        world.combats.add(entity_id, std::move(combat));
    }

    if (sight_range > 0) {
        world.sights.add(entity_id, simulation::Sight{sight_range});
    }

    world.selectables.add(entity_id, simulation::Selectable{selection_radius, 5});
}

void NetworkManager::destroy_client_entity(u32 entity_id) {
    simulation::remove_all_components(m_client_world, entity_id);
}

// ── Client fog of war ───────────────────────────────────────────────────

void NetworkManager::init_client_fog(const map::TerrainData& terrain,
                                      const map::MapManager& map,
                                      const simulation::Simulation& sim) {
    auto& manifest = map.manifest();
    simulation::FogMode fog_mode = simulation::FogMode::None;
    if (manifest.fog_of_war == "explored") fog_mode = simulation::FogMode::Explored;
    else if (manifest.fog_of_war == "unexplored") fog_mode = simulation::FogMode::Unexplored;

    m_client_vision.init(terrain.tiles_x, terrain.tiles_y, terrain.tile_size,
                      static_cast<u32>(manifest.players.size()), fog_mode, &terrain);
    m_client_sim_ref = &sim;
}

const f32* NetworkManager::update_client_fog(f32 dt) {
    if (!m_client_vision.enabled() || !m_local_player.is_valid()) return nullptr;
    if (m_client_sim_ref) {
        m_client_vision.update(m_client_world, *m_client_sim_ref);
    }
    return m_client_vision.update_visual(m_local_player, dt);
}

// ── Shared ──────────────────────────────────────────────────────────────

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

    // Client: apply interpolation each frame
    if (m_mode == Mode::Client && m_has_two_snaps) {
        client_apply_interpolation();
    }

    // Client: advance attack cycle timers
    if (m_mode == Mode::Client && dt > 0) {
        for (u32 i = 0; i < m_client_world.combats.count(); ++i) {
            auto& combat = m_client_world.combats.data()[i];
            if (combat.attack_state == simulation::AttackState::Idle) continue;

            combat.attack_timer -= dt;
            if (combat.attack_timer <= 0) {
                if (combat.attack_state == simulation::AttackState::WindUp) {
                    // WindUp finished → Backswing
                    combat.attack_state = simulation::AttackState::Backswing;
                    combat.attack_timer = combat.backsw_time;
                } else if (combat.attack_state == simulation::AttackState::Backswing) {
                    // Backswing finished → Cooldown
                    f32 cooldown = combat.attack_cooldown - combat.dmg_time - combat.backsw_time;
                    if (cooldown > 0) {
                        combat.attack_state = simulation::AttackState::Cooldown;
                        combat.attack_timer = cooldown;
                    } else {
                        // No cooldown gap → start next swing
                        combat.attack_state = simulation::AttackState::WindUp;
                        combat.attack_timer = combat.dmg_time;
                    }
                } else if (combat.attack_state == simulation::AttackState::Cooldown) {
                    // Cooldown finished → next swing
                    combat.attack_state = simulation::AttackState::WindUp;
                    combat.attack_timer = combat.dmg_time;
                }
            }
        }
    }

    // Client: tick corpse timers — hide then remove dead entities
    if (m_mode == Mode::Client && dt > 0) {
        std::vector<u32> to_remove;
        for (u32 i = 0; i < m_client_world.dead_states.count(); ++i) {
            u32 id = m_client_world.dead_states.ids()[i];
            auto& dead = m_client_world.dead_states.data()[i];
            dead.corpse_timer += dt;

            if (dead.corpse_visible && dead.corpse_timer >= dead.corpse_duration) {
                dead.corpse_visible = false;
                auto* r = m_client_world.renderables.get(id);
                if (r) r->visible = false;
            }

            if (dead.corpse_timer >= dead.cleanup_delay) {
                to_remove.push_back(id);
            }
        }
        for (u32 id : to_remove) {
            destroy_client_entity(id);
        }
    }
}

void NetworkManager::shutdown() {
    if (m_transport) {
        m_transport->disconnect();
        m_transport.reset();
    }
    m_simulation = nullptr;
    m_commands = nullptr;
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
