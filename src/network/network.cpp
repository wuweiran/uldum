#include "network/network.h"
#include "hud/hud.h"
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

// ── Entity-state snapshot: build (host) + apply-scalars (viewer) ────────────
// Single source of truth for the S_UNIT_STATE field/flag conventions, shared by
// the host's per-tick build and the viewer's apply so the two can never drift.
// `build_unit_state` samples a unit's transform/health/combat/cast/dead/corpse +
// states into a UnitState (caller guarantees a Transform; units only).
static UnitState build_unit_state(const simulation::World& world, u32 id) {
    UnitState es{};
    es.entity_id = id;

    const auto* transform = world.transforms.get(id);
    es.x = transform->position.x;
    es.y = transform->position.y;
    es.z = transform->position.z;
    es.facing = transform->facing;

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
    // Casting bit (0x04): spells drive the renderer's Spell state via
    // AbilitySet::cast_state, which is NOT part of the ability-add sync — it's a
    // per-tick runtime field like attack_state. Without this the client never
    // sees a unit cast and plays no spell animation. Set during the visible cast
    // phases (Foreswing/Channeling/Backswing), same phases derive_anim_state
    // treats as Spell on the host.
    if (const auto* aset = world.ability_sets.get(id);
        aset && (aset->cast_state == simulation::CastState::Foreswing  ||
                 aset->cast_state == simulation::CastState::Channeling ||
                 aset->cast_state == simulation::CastState::Backswing)) {
        flags |= 0x04;
    }
    // Authentic health: send Health.current (not a derived fraction — the engine
    // stores absolute current/max). has_health (0x20) gates presence so
    // projectiles/doodads carry none. The client already has max from the type
    // def (+ cold updates on runtime mutation) to render the bar.
    const auto* hp = world.healths.get(id);
    if (hp) {
        flags |= 0x20;
        es.health_current = hp->current;
    }
    es.flags = flags;

    // Map-defined states (mana/energy/etc.) — KEYLESS: send authentic
    // StateValue.current in the StateBlock's sorted key order. The client owns
    // the identical frozen schema (seeded from the unit type at spawn; Lua can't
    // add keys), so it maps positionally. max/regen are cold, not here.
    if (const auto* sb = world.state_blocks.get(id)) {
        es.state_currents.reserve(sb->states.size());
        for (const auto& [name, sv] : sb->states) {
            es.state_currents.push_back(sv.current);
        }
    }
    return es;
}

// Apply the SCALAR half of a UnitState (health, moving/attacking/cast/dead/
// corpse flags, target, states) into `world`. Deliberately does NOT touch the
// transform — the client interpolates it separately (and the host copies it).
static void apply_unit_state_scalars(simulation::World& world, const UnitState& e) {
    // Authentic health: write Health.current directly (the wire carries the
    // real value, gated by has_health). max stays whatever the client seeded
    // from the type def / received via a cold update — so the bar (current/max)
    // is correct without ever shipping max on the hot path.
    if (e.flags & 0x20) {
        if (auto* hp = world.healths.get(e.entity_id)) hp->current = e.health_current;
    }

    if (auto* mov = world.movements.get(e.entity_id)) mov->moving = (e.flags & 0x01) != 0;

    // Combat state: reproduce the server attack cycle from the single
    // "attacking" flag. Server cycles WindUp (dmg_time) → Backswing → Cooldown
    // → WindUp; the viewer kicks a cycle on the rising edge and lets its own
    // per-frame advance carry the cadence.
    if (auto* combat = world.combats.get(e.entity_id)) {
        if (e.flags & 0x02) {
            combat->target = simulation::Unit{e.target_id};
            if (combat->attack_state == simulation::AttackState::Idle) {
                combat->attack_state = simulation::AttackState::WindUp;
                combat->attack_timer = combat->dmg_time;
            }
        } else {
            combat->attack_state = simulation::AttackState::Idle;
            combat->target = simulation::Unit{};
            combat->attack_timer = 0;
        }
    }

    // Cast state (0x04): mirror the host's cast phase so derive_anim_state picks
    // AnimState::Spell. The client doesn't run the cast state machine, so it
    // holds whatever we set here until the next snapshot. Foreswing is enough —
    // derive_anim_state treats all three cast phases identically (Spell), and
    // set_anim_state plays the spell clip once at natural speed when the cached
    // timing fields are zero (client never receives them). Clear to None when
    // the bit drops so the unit falls back to idle/walk.
    if (auto* aset = world.ability_sets.get(e.entity_id)) {
        const bool casting = (e.flags & 0x04) != 0;
        if (casting) {
            if (aset->cast_state == simulation::CastState::None) {
                aset->cast_state = simulation::CastState::Foreswing;
            }
        } else if (aset->cast_state != simulation::CastState::None) {
            aset->cast_state = simulation::CastState::None;
        }
    }

    // Death is NOT applied here — it's derived from the health field above
    // (health_is_dead), the same predicate the host uses. No "dead" component on
    // the client: the renderer reads is_dead() → health, so writing health < 0
    // (which the host does at death, shipped via 0x20) already makes it a corpse.

    // Map-defined states (mana/energy/etc.) — KEYLESS map-back. The wire carries
    // only current values in the host's sorted state-key order; the client owns
    // the identical schema (StateBlock seeded from the unit type at spawn, same
    // std::map sort), so we assign positionally. max/regen live in the client's
    // StateValue already (type def + cold updates) and are left untouched. If
    // counts disagree (shouldn't — schema is frozen by type), skip rather than
    // mis-assign.
    if (!e.state_currents.empty()) {
        if (auto* sb = world.state_blocks.get(e.entity_id);
            sb && sb->states.size() == e.state_currents.size()) {
            usize i = 0;
            for (auto& [name, sv] : sb->states) sv.current = e.state_currents[i++];
        }
    }
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
        dp.known           = std::move(it->known);
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
    // A scene switch also parks the phase at Loading but runs its own finish
    // (host_finish_scene_switch). Without this guard the worker's "Loading &&
    // all_peers_loaded" transition would re-send S_WELCOME mid-switch with the
    // stale placement_count over the empty world → client PLACEMENT DESYNC.
    if (m_scene_switching) return;

    // S_WELCOME + spawn burst per seated peer, then broadcast S_START.
    for (auto& peer : m_peers) {
        if (!peer.player.is_valid()) continue;
        auto welcome = build_welcome(peer.player.id,
            static_cast<u32>(m_simulation->world().handle_infos.count()), 32,
            m_placement_count);
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
    // Entity ids reset on the scene wipe, so the old scene's controlled-unit
    // locks are stale. Phase 2's re-run main() re-issues SetControlledUnit for
    // the new scene, repopulating this.
    m_controlled_unit_by_player.clear();

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

bool NetworkManager::host_send_set_controlled_unit(u32 player_id, u32 entity_id) {
    if (m_mode != Mode::Host) return false;
    // Persist per player FIRST — the spawn-burst replay needs it even if the
    // peer isn't connected yet (late join), and it's the authoritative copy the
    // client re-applies after its scene-switch reload. UINT32_MAX clears.
    if (entity_id == UINT32_MAX) m_controlled_unit_by_player.erase(player_id);
    else                        m_controlled_unit_by_player[player_id] = entity_id;
    if (!m_transport) return false;
    u32 peer = peer_id_for_player(m_peers, player_id);
    if (peer == UINT32_MAX) return false;  // host's own slot or not-yet-joined
    auto msg = build_set_controlled_unit(entity_id);
    m_transport->send(peer, msg, true);
    return true;
}

void NetworkManager::host_finish_scene_switch() {
    if (m_mode != Mode::Host || !m_scene_switching) return;

    // Re-welcome + spawn burst per peer for the new scene's entities. The
    // welcome carries the new scene's placement_count so the client re-syncs
    // its boundary (a scene switch rebuilds the world like a join); it also
    // re-runs the client's determinism guard against the rebuilt world.
    // host_send_spawn_burst resends every entity currently in the world;
    // since switch_scene cleared the previous scene, only new-scene entities
    // are visible.
    for (auto& peer : m_peers) {
        if (!peer.player.is_valid()) continue;
        auto welcome = build_welcome(peer.player.id,
            static_cast<u32>(m_simulation->world().handle_infos.count()), 32,
            m_placement_count);
        m_transport->send(peer.peer_id, welcome, true);
        peer.known.clear();
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
                          .known = std::move(it->known),
                          .auth_token = std::move(it->auth_token)};
            m_disconnected.erase(it);

            auto welcome = build_welcome(slot,
                static_cast<u32>(m_simulation->world().handle_infos.count()), 32,
                m_placement_count);
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
                fresh.known.clear();
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

// Resolve the (type_id, variation) an S_SPAWN/S_SHOW should carry so the client
// can pick the right model itself. Registry entities send their real type_id; the
// client resolves the model from type + variation (destructable/doodad pick from
// models[variation]). Projectiles aren't in any registry — the host puts the
// MODEL PATH in the type_id field (the client sentinel-dispatches on registry-miss
// → projectile). variation defaults 0 (units/items have a single model).
static void resolve_spawn_wire(const simulation::World& world, u32 entity_id,
                               const simulation::HandleInfo& info,
                               std::string_view& out_type_id, u8& out_variation) {
    out_type_id  = info.type_id;
    out_variation = 0;
    if (info.category == simulation::Category::Projectile) {
        // Model IS the identity; ride it in the type_id field.
        if (const auto* r = world.renderables.get(entity_id)) out_type_id = r->model_path;
    } else if (const auto* d = world.destructables.get(entity_id)) {
        out_variation = d->variation;
    } else if (const auto* dd = world.doodads.get(entity_id)) {
        out_variation = dd->variation;
    }
}

// The MATERIALIZE cold batch — only records that DIVERGE from what the client
// rebuilds from the type def (via spawn_*_with_id). An intact tree diverges in
// nothing → empty batch → host_send_cold_batch sends only the S_SHOW.
static std::vector<network::ColdRecord> collect_cold_records(
        const simulation::World& world, u32 entity_id) {
    std::vector<network::ColdRecord> recs;

    const simulation::UnitTypeDef* udef = nullptr;
    if (world.types) {
        if (const auto* hi = world.handle_infos.get(entity_id))
            udef = world.types->get_unit_type(hi->type_id);
    }

    // Health — only when NOT full (damage or death). The client seeds {max,max}
    // from the type; an intact entity needs nothing. A dead one (current < 0) is
    // caught here too, so the materialize batch carries the corpse state.
    if (const auto* h = world.healths.get(entity_id); h && h->current < h->max)
        recs.push_back(network::cold_health(h->current, h->max));

    // States (mana/energy/…) — only when current diverges from max (client seeds
    // at max). max itself is never mutated at runtime (no binding), so it's always
    // the type default the client already has.
    if (const auto* sb = world.state_blocks.get(entity_id)) {
        for (const auto& [key, val] : sb->states)
            if (val.current != val.max)
                recs.push_back(network::cold_state(key, val.current, val.max));
    }

    // Attributes are deliberately NOT in the materialize batch. The client's
    // Attribute apply writes the EFFECTIVE `numeric`, but an AbilityAdd in the same
    // batch triggers recalculate_modifiers which rebuilds `numeric` from base +
    // modifiers — the two would stomp each other. The old join-burst skipped
    // attributes for the same reason; live changes still ride the on-change S_COLD
    // while the unit is visible. (Revisit if a base-vs-effective split is added.)

    // Abilities — only RUNTIME-ADDED instances (not in the type's default list;
    // the client already seeded those). Plus any live cooldown on ANY ability
    // (default or not), since cooldown is pure runtime state.
    if (const auto* aset = world.ability_sets.get(entity_id)) {
        for (const auto& inst : aset->abilities) {
            bool is_default = udef &&
                std::find(udef->abilities.begin(), udef->abilities.end(),
                          inst.ability_id) != udef->abilities.end();
            if (!is_default) {
                for (const auto& src : inst.sources) {
                    recs.push_back(network::cold_ability_add_rec(
                        inst.ability_id, inst.level,
                        static_cast<u8>(simulation::ability_source_kind(src))));
                }
                for (const auto& [key, val] : inst.active_modifiers)
                    recs.push_back(network::cold_ability_modifier_rec(inst.ability_id, key, val));
            }
            if (inst.cooldown_remaining > 0.0f)
                recs.push_back(network::cold_cooldown_rec(inst.ability_id, inst.cooldown_remaining));
        }
    }

    // Manual status bits (the ability-driven layer rides AbilityAdd above).
    if (const auto* sf = world.status_flags.get(entity_id); sf && sf->manual_bits != 0)
        recs.push_back(network::cold_status_rec(sf->manual_bits, true));

    // Item scalars — only when diverged from the item type's defaults.
    if (const auto* ii = world.item_infos.get(entity_id)) {
        const simulation::ItemTypeDef* idef =
            world.types ? world.types->get_item_type(ii->type_id) : nullptr;
        if (!idef || ii->charges != idef->initial_charges)
            recs.push_back(network::cold_item_charges_rec(ii->charges));
        if (!idef || ii->level != idef->initial_level)
            recs.push_back(network::cold_item_level_rec(ii->level));
    }

    // Current script animation queue (render-sticky) — always divergent (the
    // client has no queue by default), so send whenever one exists.
    if (const auto* aq = world.anim_queues.get(entity_id); aq && !aq->clips.empty()) {
        std::vector<std::string> clips(aq->clips.begin(), aq->clips.end());
        recs.push_back(network::cold_anim_rec(clips, aq->looping));
    }

    // Construction (render-sticky) — a building materialized/revealed mid-build
    // needs its progress so the client plays the time-scaled birth clip from the
    // right phase. A finished building has no Construction component → nothing
    // sent, and the on-change record at completion clears it client-side.
    if (const auto* c = world.constructions.get(entity_id); c && c->under_construction) {
        recs.push_back(network::cold_construction_rec(
            c->under_construction, c->build_time_total, c->build_progress));
    }

    return recs;
}

// Send the MATERIALIZE cold-state batch right after S_SPAWN/S_SHOW, for any
// stateful entity. This is what makes cold state (incl. health→death) survive a
// fog round-trip: everything that diverged while the entity sat in fog is
// re-established on re-show, not left at type defaults. No-op if no records.
void NetworkManager::host_send_cold_batch(PeerInfo& peer, u32 entity_id) {
    auto& world = m_simulation->world();
    auto recs = collect_cold_records(world, entity_id);
    if (recs.empty()) return;
    auto msg = build_cold_batch(entity_id, recs);
    m_transport->send(peer.peer_id, msg, true);
}

void NetworkManager::host_send_inventory_state(PeerInfo& peer, u32 carrier_id) {
    auto& world = m_simulation->world();
    const auto* inventory = world.inventories.get(carrier_id);
    if (!inventory) return;
    for (u32 slot = 0; slot < inventory->slots.size(); ++slot) {
        simulation::Item item = inventory->slots[slot];
        if (!world.contains(item)) continue;
        m_transport->send(
            peer.peer_id,
            build_cold_inventory(carrier_id, slot, item.id), true);
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
    std::string_view type_id; u8 variation;
    resolve_spawn_wire(world, entity_id, info, type_id, variation);

    auto msg = build_spawn(entity_id, type_id, owner_id,
                           transform->position.x, transform->position.y,
                           transform->facing, newly_created, variation);
    m_transport->send(peer.peer_id, msg, true);
    peer.known.insert(entity_id);

    // Slim reveal + cold batch = MATERIALIZE. The batch carries health (→ death),
    // states, abilities, cooldowns, attrs, current anim — atomic-enough (same
    // reliable-ordered drain) that the entity is born with its real state.
    host_send_cold_batch(peer, entity_id);
    host_send_inventory_state(peer, entity_id);
}

void NetworkManager::host_send_show(PeerInfo& peer, u32 entity_id,
                                    const simulation::HandleInfo& info) {
    // An already-alive entity re-entered the peer's live sight. Same materialize
    // payload as spawn, but the client comes up at current state with NO birth
    // clip. Used instead of S_SPAWN when the entity was NOT born this tick.
    auto& world = m_simulation->world();
    const auto* transform = world.transforms.get(entity_id);
    if (!transform) return;

    const auto* owner = world.owners.get(entity_id);
    u8 owner_id = owner ? static_cast<u8>(owner->id) : 0;
    std::string_view type_id; u8 variation;
    resolve_spawn_wire(world, entity_id, info, type_id, variation);

    auto msg = build_show(entity_id, type_id, owner_id,
                          transform->position.x, transform->position.y,
                          transform->facing, variation);
    m_transport->send(peer.peer_id, msg, true);
    peer.known.insert(entity_id);

    // Cold batch after the slim reveal — the fog-reentry path where cold state
    // (health→death of a crate, abilities/cooldowns/mana that changed out of
    // sight) would otherwise be lost.
    host_send_cold_batch(peer, entity_id);
    host_send_inventory_state(peer, entity_id);
}

void NetworkManager::host_send_spawn_burst(PeerInfo& peer) {
    auto& world = m_simulation->world();
    auto& infos = world.handle_infos;

    for (u32 i = 0; i < infos.count(); ++i) {
        u32 id = infos.ids()[i];
        const auto& info = infos.data()[i];
        // Placement entity (id < boundary): the client built it locally from
        // its own placements.bin. Don't S_SPAWN it — just seed known so the
        // per-tick loop still ships its S_UNIT_STATE / real-death S_DESTROY. But its
        // cold state may have diverged from type defaults during main() (abilities
        // added, mana spent, a crate destroyed → health 0), and it skips
        // host_send_spawn's batch — so send the batch here for ones visible at join.
        if (id < m_placement_count) {
            if (info.hidden) {
                m_transport->send(peer.peer_id, build_destroy(id), true);
                continue;
            }
            peer.known.insert(id);
            if (is_visible_to(id, peer.player)) {
                host_send_cold_batch(peer, id);
                host_send_inventory_state(peer, id);
            }
            continue;
        }
        if (!is_visible_to(id, peer.player)) continue;
        host_send_spawn(peer, id, info, false);
    }

    log::info(TAG, "Sent {} entities to player {}", peer.known.size(), peer.player.id);

    // HUD join-replay: re-send the persistent HUD state (Lua-created nodes +
    // permanent text tags) this peer missed because it was created before the
    // peer connected. Route each packet to THIS peer only if its bit is in the
    // node/tag's players_mask (same filter as live host_hud_sync). The entity
    // burst above is the parallel for world state; this is its HUD counterpart.
    if (m_hud_replay) {
        u32 pbit = 1u << peer.player.id;
        m_hud_replay->emit_state_to(
            [&](const std::vector<u8>& pkt, u32 mask) {
                if (mask & pbit) m_transport->send(peer.peer_id, pkt, true);
            });
    }

    // Controlled-unit join-replay: SetControlledUnit ran inside main() before
    // this peer finished loading, so its live S_SET_CONTROLLED_UNIT was sent to
    // a peer that didn't exist yet (or is reloading on a scene switch). Re-send
    // the stored lock now, after the entity burst so the unit exists client-side.
    if (auto it = m_controlled_unit_by_player.find(peer.player.id);
        it != m_controlled_unit_by_player.end()) {
        auto msg = build_set_controlled_unit(it->second);
        m_transport->send(peer.peer_id, msg, true);
    }
}

bool NetworkManager::is_visible_to(u32 entity_id, simulation::Player player) const {
    // Server-side snapshot filter — refuses to serialize unit data for
    // a peer that cannot see it. Shares logic with the renderer's
    // is_fog_hidden via Vision::is_unit_visible_to so we can never
    // ship invisibility data the client could mine for cheats.
    return m_simulation->vision().is_unit_visible_to(
        m_simulation->world(), *m_simulation, entity_id, player);
}

NetworkManager::ViewGate NetworkManager::view_gate(
        const simulation::World& world, const simulation::Simulation& sim,
        u32 id, simulation::Player player, u32 placement_count,
        std::unordered_set<u32>& discovered) const {
    // Uniform fog rule (the single source of truth for host + client):
    //   MOBILE  → kept only while live-visible; culled when it leaves vision.
    //   STATIC  → frozen at last-seen once DISCOVERED (seen live at least once),
    //             kept on any Explored tile thereafter.
    // `preplaced` is NOT part of the keep decision — it's only a wire-spawn
    // optimization handled by the caller's output loop. Preplaced statics are
    // authored map decoration (visible from the start), so they're seeded
    // discovered on first encounter; a *dynamic* static must be scouted first
    // (H3) or a never-seen static on a pre-explored tile would show.
    ViewGate g{};
    const auto* info = world.handle_infos.get(id);
    if (!info || info->hidden) return g;
    g.remembered = simulation::is_static_remembered_entity(world, id);
    g.live_vis   = sim.vision().is_unit_visible_to(world, sim, id, player);

    const bool preplaced_static = g.remembered && id < placement_count;
    if (g.live_vis || preplaced_static) discovered.insert(id);

    const bool remembered_ok = g.remembered && discovered.contains(id) &&
        sim.vision().is_unit_visible_to(world, sim, id, player, /*is_static_remembered=*/true);
    g.keep = g.live_vis || remembered_ok;
    return g;
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
        std::vector<UnitState> unit_states;
        std::vector<ProjectileState> proj_states;
        unit_states.reserve(infos.count());

        for (u32 i = 0; i < infos.count(); ++i) {
            u32 id = infos.ids()[i];
            const auto& info = infos.data()[i];

            // A CARRIED item isn't world-present — it lives in a carrier's
            // inventory slot; its ground Transform is stale. Skip the live gate:
            // gating on that stale tile would S_HIDE/S_DESTROY it when the pickup
            // tile fogs, killing the client's slot. Its sync is the S_COLD
            // Inventory pickup + charge updates; drop re-materializes it.
            if (const auto* car = world.carriables.get(id);
                car && simulation::is_non_null_handle(car->carried_by)) {
                continue;
            }

            // LIVE-ONLY gate: ship only what the peer can see now — no fog memory,
            // no per-peer `discovered`. The client owns memory (decides snapshot
            // vs drop on S_HIDE). Same predicate as the anti-cheat snapshot filter.
            if (!is_visible_to(id, peer.player)) continue;

            // A dying projectile is gameplay-dead; its death clip is a pure
            // client-side visual. We already sent S_PROJECTILE_DYING and the
            // client now owns teardown (it drains death_timer + removes the
            // entity). Treat it as GONE for the wire: skip visible_now so the
            // leave-loop drops it from `known` SILENTLY — no S_DESTROY (projectiles
            // are too common to spend one) and no S_HIDE (that would cut the clip).
            if (const auto* pc = world.projectiles.get(id); pc && pc->dying) continue;

            visible_now.insert(id);

            // First knowledge: born this tick → S_SPAWN (plays birth); already
            // existed → S_SHOW (no birth). Placement ids are seeded into `known`
            // at burst (client built them locally) so they skip this. A dynamic
            // entity that left sight was dropped from `known`, so re-entry
            // re-materializes at its CURRENT position (the ghost-unit fix).
            if (!peer.known.contains(id)) {
                if (!last_tick.contains(id)) host_send_spawn(peer, id, info, /*newly_created=*/true);
                else                         host_send_show(peer, id, info);
            }

            // Per-tick HOT state — MOVERS ONLY, split by class. Units get the rich
            // UnitState; projectiles get the lean ProjectileState. Statics
            // (destructable/doodad/item) don't move or regen — their lifecycle
            // already rode S_SPAWN/S_SHOW above and S_HIDE/S_DESTROY below, so they
            // send NO per-tick state (a crate's client HP is seeded from its type
            // and only gates liveness; its death arrives via S_DESTROY).
            const auto* transform = world.transforms.get(id);
            if (!transform) continue;

            if (info.category == simulation::Category::Unit) {
                unit_states.push_back(build_unit_state(world, id));
            } else if (info.category == simulation::Category::Projectile) {
                proj_states.push_back(ProjectileState{
                    id, transform->position.x, transform->position.y,
                    transform->position.z, transform->facing});
            }
        }

        // Peer knew it but can't see it now. Cases:
        //   CARRIED           → drop from `known` SILENTLY (the S_COLD Inventory
        //                       already hid+carried it; S_HIDE/S_DESTROY here would
        //                       kill the client's slot).
        //   dying projectile   → drop SILENTLY. S_PROJECTILE_DYING already went out
        //                       and the client owns teardown; no S_DESTROY (too
        //                       common) and no S_HIDE (it would cut the death clip).
        //   gone from world    → S_DESTROY (freed / expiry / Lua).
        //   still in world     → S_HIDE  (client snapshots a static, drops a mobile).
        std::vector<u32> to_remove;
        for (u32 known_id : peer.known) {
            if (visible_now.contains(known_id)) continue;
            const auto* car = world.carriables.get(known_id);
            const bool carried = car && simulation::is_non_null_handle(car->carried_by);
            const auto* pc = world.projectiles.get(known_id);
            const bool dying_projectile = pc && pc->dying;
            if (!carried && !dying_projectile) {
                const auto* known_info = infos.get(known_id);
                const bool gone = !known_info || known_info->hidden;
                auto msg = gone ? build_destroy(known_id)
                                : build_hide(known_id);
                m_transport->send(peer.peer_id, msg, true);
            }
            to_remove.push_back(known_id);   // leaves `known` either way
        }
        for (u32 id : to_remove) peer.known.erase(id);

        // Send the two HOT snapshots (unreliable, self-healing).
        if (!unit_states.empty()) {
            auto msg = build_unit_state(tick, unit_states);
            m_transport->send(peer.peer_id, msg, false);
        }
        if (!proj_states.empty()) {
            auto msg = build_projectile_state(tick, proj_states);
            m_transport->send(peer.peer_id, msg, false);
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

void NetworkManager::host_broadcast_update(u32 entity_id, std::span<const u8> update_packet) {
    if (m_mode != Mode::Host || m_peers.empty() || !m_simulation) return;
    const auto& world = m_simulation->world();
    const auto* info = world.handle_infos.get(entity_id);
    if (!info) return;
    // Visibility key: a CARRIED item isn't in any peer's `known` (it left the
    // live gate on pickup), so its own updates (charges) would never send. Route
    // through the CARRIER — a peer that can see the holder gets the item update.
    u32 vis_key = entity_id;
    if (const auto* car = world.carriables.get(entity_id);
        car && simulation::is_non_null_handle(car->carried_by)) {
        vis_key = car->carried_by.id;
    }
    for (auto& peer : m_peers) {
        if (peer.known.contains(vis_key)) {
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

void NetworkManager::host_end_game(u32 winning_team, std::string_view stats_json) {
    if (m_mode != Mode::Host) return;
    m_game_ended = true;
    m_end_data = EndData{winning_team, std::string(stats_json)};
    auto msg = build_end(winning_team, stats_json);
    m_transport->broadcast(msg, true);
    if (winning_team == UINT32_MAX)
        log::info(TAG, "Game ended — no winner");
    else
        log::info(TAG, "Game ended — winning team {}", winning_team);
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
    case MsgType::S_SHOW:    client_handle_show(data); break;
    case MsgType::S_HIDE:    client_handle_hide(data); break;
    case MsgType::S_DESTROY: client_handle_destroy(data); break;
    case MsgType::S_UNIT_STATE:       client_handle_unit_state(data); break;
    case MsgType::S_PROJECTILE_STATE: client_handle_projectile_state(data); break;
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
    case MsgType::S_COLD: client_handle_cold(data); break;
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
    case MsgType::S_HUD_DESTROY_TEXT_TAG:
    case MsgType::S_HUD_DISPLAY_MESSAGE:
    case MsgType::S_HUD_ACTION_BAR_SET_SLOT: {
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
    u32 client_n = mirror_world().entities.next_id();
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

    // Adopt the host's authoritative placement boundary. Sent at join AND at
    // every scene switch (a switch rebuilds the world like a join, so the
    // client's boundary would otherwise stay stale at the old scene's N and
    // misclassify the new scene's dynamic entities as preplaced-local).
    m_placement_count = w.placement_count;

    // Resume the client from the scene-switch barrier. S_SCENE_SWITCH set
    // m_scene_switching=true + Phase::Loading on this client; only the host
    // cleared its own copy (host_finish_scene_switch). Without this the
    // client's per-frame vision().update + project_local_view stay gated off
    // forever → permanent fog, own units never revealed. The re-welcome is
    // the host's "new scene ready" signal, reliable-ordered just before the
    // spawn burst, so clearing here resumes cleanly. Harmless at first join
    // (already Playing / not switching); S_START still handles that path.
    if (m_scene_switching) {
        m_scene_switching = false;
        m_phase = Phase::Playing;
    }
}

void NetworkManager::client_handle_spawn(std::span<const u8> data) {
    auto s = parse_spawn(data);
    spawn_client_entity(mirror_world(), s.entity_id, s.type_id, s.owner, s.x, s.y, s.facing,
                        s.newly_created, s.variation);
    // Cold state (health→death, abilities, current anim, …) arrives in the S_COLD
    // batch the host sends right after, in the same reliable-ordered drain — so
    // the entity is born with its real state (a dead crate comes up a corpse).
    // Born → drop any stale snapshot for this id (defensive; ids are fresh).
    m_local_view_impl.drop_snapshot(s.entity_id);
}

void NetworkManager::client_handle_show(std::span<const u8> data) {
    // Already-alive entity re-entered live sight. Materialize into the mirror if
    // absent (newly_created=false → no birth clip), then DROP its snapshot — the
    // host says it's live now, so it renders from the mirror, not memory. The
    // S_COLD batch that follows re-establishes health (→ death), abilities,
    // cooldowns, the current script anim — so a dead crate rebuilds as a corpse,
    // not intact.
    auto s = parse_show(data);
    spawn_client_entity(mirror_world(), s.entity_id, s.type_id, s.owner, s.x, s.y, s.facing,
                        /*newly_created=*/false, s.variation);
    m_local_view_impl.drop_snapshot(s.entity_id);
}

void NetworkManager::client_handle_hide(std::span<const u8> data) {
    // Entity left the peer's live sight (still exists in the world). The CLIENT
    // owns the memory decision here (the host shipped a type-agnostic S_HIDE):
    //   static-remembered → SNAPSHOT it (mirror → snapshot store), then remove
    //       from the mirror so nothing can resurrect it live (snapshot ⊥ mirror).
    //       It renders frozen/dimmed while out of sight; dropped on reveal.
    //   mobile            → just remove from the mirror (forgotten; ghost-proof).
    u32 id = parse_hide(data);
    if (simulation::is_static_remembered_entity(mirror_world(), id)) {
        m_local_view_impl.snapshot_from(mirror_world(), id);
    }
    destroy_client_entity(id);   // remove from mirror either way
}

void NetworkManager::client_handle_destroy(std::span<const u8> data) {
    // Removed from the world (decay / expiry / Lua). Always gone — no memory.
    u32 id = parse_destroy(data);
    m_local_view_impl.drop_snapshot(id);
    destroy_client_entity(id);
}

// Advance the interpolation double-buffer to `tick`. Unit and projectile HOT
// packets are separate messages that share a tick (sent back-to-back in the same
// host loop iteration). The FIRST packet of a new tick swaps newer→older and
// opens a fresh newer snapshot; a later packet with the SAME tick fills into that
// same snapshot without swapping. Tick-keyed so unit-only, projectile-only, and
// both-in-one-tick all work, and out-of-order unreliable arrivals self-heal.
u32 NetworkManager::client_begin_snapshot_tick(u32 tick) {
    if (m_snapshots[m_snap_idx].tick == tick && m_snapshots[m_snap_idx].receive_time > 0) {
        return m_snap_idx;   // same tick — fill the already-open newer snapshot
    }
    u32 older = m_snap_idx;
    u32 newer = 1 - m_snap_idx;
    m_snapshots[newer].tick = tick;
    m_snapshots[newer].receive_time = wall_time();
    m_snapshots[newer].units.clear();
    m_snapshots[newer].projectiles.clear();
    m_snap_idx = newer;
    if (!m_has_two_snaps && m_snapshots[older].receive_time > 0) m_has_two_snaps = true;
    return newer;
}

void NetworkManager::client_handle_unit_state(std::span<const u8> data) {
    auto s = parse_unit_state(data);
    u32 idx = client_begin_snapshot_tick(s.tick);
    m_snapshots[idx].units = std::move(s.units);
    client_apply_interpolation();
}

void NetworkManager::client_handle_projectile_state(std::span<const u8> data) {
    auto s = parse_projectile_state(data);
    u32 idx = client_begin_snapshot_tick(s.tick);
    m_snapshots[idx].projectiles = std::move(s.projectiles);
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
    // Wire signal is bare (just an id) — the client never runs system_projectile,
    // so it applies the dying state itself: mark + size death_timer + queue the
    // death clip (all in enter_projectile_dying). The renderer reads the queued
    // clip via project_local_view (which fills own_anim_queues only when dying is
    // set), and client_tick → tick_projectile_death drains the timer + removes the
    // entity. The host sends no S_DESTROY for projectiles.
    simulation::enter_projectile_dying(mirror_world(), parse_projectile_dying(data));
}

// Interpolate one record's transform into the view world. Works for any record
// with entity_id/x/y/z/facing (UnitState, ProjectileState) — the position +
// facing math is identical; only units apply the scalar half afterward. `Lookup`
// resolves the same id in the older snapshot (or nullptr → snap).
template <typename Rec>
static void interp_transform(simulation::World& world, const Rec& e, const Rec* o, f32 alpha) {
    auto* t = world.transforms.get(e.entity_id);
    if (!t) return;
    if (o) {
        // `position` is the smooth wall-clock lerp shown this frame.
        // `prev_position` is set so `position - prev_position` equals the STABLE
        // full-tick delta (new − old) the renderer uses for projectile pitch /
        // motion-blur — NOT the frame-to-frame delta (which jittered with packet
        // arrival). Client renders at alpha=1, so `position` drives the drawn spot.
        glm::vec3 disp{ o->x + (e.x - o->x) * alpha,
                        o->y + (e.y - o->y) * alpha,
                        o->z + (e.z - o->z) * alpha };
        glm::vec3 tick_delta{ e.x - o->x, e.y - o->y, e.z - o->z };
        t->position = disp;
        t->prev_position = disp - tick_delta;

        f32 diff = e.facing - o->facing;
        while (diff > glm::pi<f32>()) diff -= glm::two_pi<f32>();
        while (diff < -glm::pi<f32>()) diff += glm::two_pi<f32>();
        f32 disp_facing = o->facing + diff * alpha;
        t->facing = disp_facing;
        t->prev_facing = disp_facing - diff;
    } else {
        // New entity — snap.
        t->prev_position = t->position = {e.x, e.y, e.z};
        t->prev_facing = t->facing = e.facing;
    }
}

void NetworkManager::client_apply_interpolation() {
    if (!m_has_two_snaps) {
        // Only one snapshot — snap both classes directly to it.
        auto& snap = m_snapshots[m_snap_idx];
        for (auto& e : snap.units) {
            if (auto* t = mirror_world().transforms.get(e.entity_id)) {
                t->prev_position = t->position = {e.x, e.y, e.z};
                t->prev_facing = t->facing = e.facing;
            }
            apply_unit_state_scalars(mirror_world(), e);
        }
        for (auto& p : snap.projectiles) {
            if (auto* t = mirror_world().transforms.get(p.entity_id)) {
                t->prev_position = t->position = {p.x, p.y, p.z};
                t->prev_facing = t->facing = p.facing;
            }
        }
        return;
    }

    u32 newer = m_snap_idx;
    u32 older = 1 - m_snap_idx;
    auto& snap_old = m_snapshots[older];
    auto& snap_new = m_snapshots[newer];

    f64 tick_dur = SIM_TICK_DT;
    f64 now = wall_time();
    f64 render_time = now - tick_dur;  // render one tick behind

    f32 alpha = 0.0f;
    f64 span = snap_new.receive_time - snap_old.receive_time;
    if (span > 0.0) {
        alpha = static_cast<f32>((render_time - snap_old.receive_time) / span);
        alpha = std::clamp(alpha, 0.0f, 1.0f);
    }

    // Units: interpolate transform + apply the scalar half.
    std::unordered_map<u32, const UnitState*> unit_old;
    for (auto& e : snap_old.units) unit_old[e.entity_id] = &e;
    for (auto& e : snap_new.units) {
        auto it = unit_old.find(e.entity_id);
        interp_transform(mirror_world(), e, it != unit_old.end() ? it->second : nullptr, alpha);
        // Scalar half (health / flags / target / states) — shared with the host's
        // in-process apply. Transform interp above stays client-specific.
        apply_unit_state_scalars(mirror_world(), e);
    }

    // Projectiles: transform only.
    std::unordered_map<u32, const ProjectileState*> proj_old;
    for (auto& p : snap_old.projectiles) proj_old[p.entity_id] = &p;
    for (auto& p : snap_new.projectiles) {
        auto it = proj_old.find(p.entity_id);
        interp_transform(mirror_world(), p, it != proj_old.end() ? it->second : nullptr, alpha);
    }
}

// Apply one cold record to the view world. The SINGLE client-side apply for cold
// state, shared by the on-change (1 record) and MATERIALIZE (N-record batch) uses
// of S_COLD. Mirrors the host's own mutation logic so the client's derived values
// (modifiers, flag refcounts, visual_alpha) match the server exactly.
static void apply_cold_record(simulation::World& world, u32 entity_id,
                              const network::ColdRecord& rec) {
    using network::ColdKind;
    switch (rec.kind) {
    case ColdKind::Health: {
        auto* h = world.healths.get(entity_id);
        if (h) { h->current = rec.value; h->max = rec.value2; }
        break;
    }
    case ColdKind::Attribute: {
        auto* attrs = world.attribute_blocks.get(entity_id);
        if (!attrs) {
            world.attribute_blocks.add(entity_id, simulation::AttributeBlock{});
            attrs = world.attribute_blocks.get(entity_id);
        }
        attrs->numeric[rec.key] = rec.value;
        break;
    }
    case ColdKind::StringAttribute: {
        auto* attrs = world.attribute_blocks.get(entity_id);
        if (!attrs) {
            world.attribute_blocks.add(entity_id, simulation::AttributeBlock{});
            attrs = world.attribute_blocks.get(entity_id);
        }
        attrs->string_attrs[rec.key] = rec.str_value;
        break;
    }
    case ColdKind::State: {
        auto* states = world.state_blocks.get(entity_id);
        if (!states) {
            world.state_blocks.add(entity_id, simulation::StateBlock{});
            states = world.state_blocks.get(entity_id);
        }
        auto& state = states->states[rec.key];
        state.current = rec.value;
        state.max = rec.value2;
        break;
    }
    case ColdKind::AbilityAdd: {
        // Mirror server-side add_ability: populate active_modifiers /
        // active_flags from the def, bump per-flag refcounts, and run
        // recalculate_modifiers so the client's renderable visual_alpha
        // (and any other derived values) match the server. Without
        // this, passive_flag buffs like windwalk_invisible would have
        // no effect on the client — the unit wouldn't appear invisible
        // or carry its alpha modifier even though it does on the host.
        auto* aset = world.ability_sets.get(entity_id);
        if (!aset) {
            world.ability_sets.add(entity_id, simulation::AbilitySet{});
            aset = world.ability_sets.get(entity_id);
        }
        const simulation::AbilityDef* def =
            world.abilities ? world.abilities->get(rec.key) : nullptr;

        simulation::AbilitySourceKind source_kind =
            static_cast<simulation::AbilitySourceKind>(rec.byte_value);
        simulation::AbilitySource source;
        if (source_kind == simulation::AbilitySourceKind::Item) {
            source.value = simulation::ItemAbilitySource{};
        } else if (source_kind == simulation::AbilitySourceKind::Applied) {
            source.value = simulation::AppliedAbilitySource{};
        } else {
            source.value = simulation::InnateAbilitySource{};
        }
        source.remaining_duration = def
            ? def->level_data(rec.uint_value).duration
            : -1.0f;

        simulation::AbilityInstance* instance = nullptr;
        if (def && !def->stackable) {
            for (auto& candidate : aset->abilities) {
                if (candidate.ability_id == rec.key) {
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
            created.ability_id = rec.key;
            created.level = rec.uint_value;
            created.sources.push_back(source);
            if (def) {
                auto& lvl = def->level_data(rec.uint_value);
                created.active_modifiers = lvl.modifiers;
                created.active_flags     = lvl.flags;
            }
            auto flags_snapshot = created.active_flags;
            aset->abilities.push_back(std::move(created));
            simulation::flag_refcount_delta(world, entity_id, flags_snapshot, +1);
        }
        simulation::recalculate_modifiers(world, entity_id);
        break;
    }
    case ColdKind::AbilityRemove: {
        auto* aset = world.ability_sets.get(entity_id);
        if (aset) {
            simulation::AbilitySourceKind source_kind =
                static_cast<simulation::AbilitySourceKind>(rec.byte_value);
            auto& abilities = aset->abilities;
            for (auto instance = abilities.begin();
                 instance != abilities.end(); ) {
                if (instance->ability_id != rec.key) {
                    ++instance;
                    continue;
                }

                if (rec.bool_value) {
                    simulation::flag_refcount_delta(
                        world, entity_id, instance->active_flags, -1);
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
                        world, entity_id, instance->active_flags, -1);
                    abilities.erase(instance);
                }
                break;
            }
            simulation::recalculate_modifiers(world, entity_id);
        }
        break;
    }
    case ColdKind::Owner: {
        auto* owner = world.owners.get(entity_id);
        if (owner) owner->id = rec.uint_value;
        break;
    }
    case ColdKind::AbilityModifier: {
        // Mirror SetAbilityModifier: mutate the named modifier on every
        // matching instance and recalculate. Drives Lua-side tweens
        // like Wind Walk's fade-in.
        auto* aset = world.ability_sets.get(entity_id);
        if (!aset) break;
        bool changed = false;
        for (auto& a : aset->abilities) {
            if (a.ability_id == rec.key) {
                a.active_modifiers[rec.str_value] = rec.value;
                changed = true;
            }
        }
        if (changed) simulation::recalculate_modifiers(world, entity_id);
        break;
    }
    case ColdKind::Status: {
        // Mirror SetUnitStatus on the client — manual_bits layer only;
        // ability-driven refcounts arrive through AbilityAdd/Remove
        // already and compose with this via recompute_effective_flags
        // inside set_unit_status.
        simulation::Unit unit = world.unit(entity_id);
        simulation::set_unit_status(world, unit, rec.uint_value, rec.bool_value);
        break;
    }
    case ColdKind::Transform: {
        if (auto* t = world.transforms.get(entity_id)) {
            t->position = {rec.x, rec.y, rec.z};
            t->prev_position = t->position;
            t->facing = rec.facing;
            t->prev_facing = rec.facing;
        }
        break;
    }
    case ColdKind::Cooldown: {
        // Apply the cooldown to every instance of `ability_id` on the
        // unit — matches the server's SetAbilityCooldown / Reset
        // semantics (which iterate all matching instances).
        auto* aset = world.ability_sets.get(entity_id);
        if (aset) {
            for (auto& a : aset->abilities) {
                if (a.ability_id == rec.key) {
                    a.cooldown_remaining = std::max(0.0f, rec.value);
                }
            }
        }
        break;
    }
    case ColdKind::ItemCharges:
        simulation::set_charges(
            world, simulation::Item{entity_id}, static_cast<i32>(rec.uint_value));
        break;
    case ColdKind::ItemLevel:
        simulation::set_level(
            world, simulation::Item{entity_id}, static_cast<i32>(rec.uint_value));
        break;
    case ColdKind::Inventory: {
        // Item pickup / drop mirrored onto the client. entity_id = carrier.
        //   item_id != INVALID → PICKUP into `slot`: put the item in the
        //     carrier's Inventory slot, mark its Carriable, hide ground render.
        //   item_id == INVALID → (shouldn't happen; kept for symmetry).
        //   slot == UINT32_MAX  → DROP: find item_id in the carrier's slots,
        //     clear it + Carriable, restore ground render.
        u32 carrier_id = entity_id;
        u32 slot       = rec.uint_value;
        u32 item_id    = rec.uint_value2;

        if (slot == UINT32_MAX) {
            // Drop: locate the item in the carrier's slots and clear it.
            if (auto* inv = world.inventories.get(carrier_id)) {
                for (auto& s : inv->slots) {
                    if (s.id == item_id) { s = simulation::Item{}; break; }
                }
            }
            if (auto* car = world.carriables.get(item_id)) car->carried_by = simulation::Unit{};
            // Place the item at its drop position (rides the drop event now that
            // items are off the per-tick snapshot) and restore the ground render.
            // Without the position the client would re-show the item at its stale
            // spawn transform (the "item teleports back to where it was born" bug).
            if (auto* t = world.transforms.get(item_id)) {
                t->position = {rec.x, rec.y, rec.z};
                t->prev_position = t->position;
            }
            if (auto* r = world.renderables.get(item_id)) r->visible = true;
            break;
        }

        // Pickup: ensure the carrier has a sized Inventory, then set the slot.
        auto* inv = world.inventories.get(carrier_id);
        if (!inv) {
            simulation::Inventory fresh;
            // Size from the unit type def's inventory_size (fallback: grow to fit).
            if (world.types) {
                if (const auto* hi = world.handle_infos.get(carrier_id)) {
                    if (const auto* ud = world.types->get_unit_type(hi->type_id)) {
                        fresh.max_slots = ud->inventory_size;
                        fresh.slots.resize(ud->inventory_size);
                    }
                }
            }
            world.inventories.add(carrier_id, std::move(fresh));
            inv = world.inventories.get(carrier_id);
        }
        if (inv) {
            if (slot >= inv->slots.size()) inv->slots.resize(slot + 1);
            inv->slots[slot] = simulation::Item{item_id};
        }
        // Mark the item carried (lazy-add Carriable if the ground item lacked it).
        if (world.handle_infos.has(item_id)) {
            if (auto* car = world.carriables.get(item_id)) {
                car->carried_by = simulation::Unit{carrier_id};
            } else {
                world.carriables.add(item_id, simulation::Carriable{simulation::Unit{carrier_id}});
            }
            // Hide the ground render immediately (don't wait for S_UNIT_STATE — the
            // host stops shipping the item's state once it's carried).
            if (auto* r = world.renderables.get(item_id)) r->visible = false;
        }
        break;
    }
    case ColdKind::Anim: {
        // Scripted animation queue (SetUnitAnimation/Queue/Reset). Empty clip list
        // = reset → drop the queue, engine-derived animation resumes. Otherwise
        // set/replace it (the host already resolved Queue's append into the full
        // list). On materialize the queue is present the frame the render instance
        // is born, so the first-sight rule holds a finished one-shot's last frame.
        if (rec.clips.empty()) {
            world.anim_queues.remove(entity_id);
            break;
        }
        simulation::AnimQueue aq;
        for (const auto& c : rec.clips) aq.clips.push_back(c);
        aq.looping = rec.bool_value;
        if (auto* q = world.anim_queues.get(entity_id)) *q = std::move(aq);
        else world.anim_queues.add(entity_id, std::move(aq));
        break;
    }
    case ColdKind::Construction: {
        // Under-construction state for the client's time-scaled birth clip. While
        // building, add/update the mirror Construction so derive_anim_state plays
        // the stretched birth (seeded to build_progress on materialize). On finish
        // (under_construction=false) drop it → the building falls through to Idle.
        if (rec.bool_value) {
            simulation::Construction c;
            c.under_construction = true;
            c.build_time_total   = rec.value;
            c.build_progress     = rec.value2;
            if (auto* existing = world.constructions.get(entity_id)) *existing = c;
            else world.constructions.add(entity_id, std::move(c));
        } else {
            world.constructions.remove(entity_id);
        }
        break;
    }
    }
}

void NetworkManager::client_handle_cold(std::span<const u8> data) {
    // S_COLD is a COUNT-PREFIXED batch: 1 record on-change, N records on
    // MATERIALIZE (right after S_SPAWN/S_SHOW). Either way, apply each record
    // through the one shared path — batch = apply all, on-change = apply one. This
    // is what restores cold state (health→death, mana, abilities, cooldowns,
    // current anim) on fog re-show instead of leaving type defaults.
    auto c = parse_cold(data);
    for (const auto& rec : c.records)
        apply_cold_record(mirror_world(), c.entity_id, rec);
}

void NetworkManager::spawn_client_entity(simulation::World& world, u32 entity_id,
                                          std::string_view type_id,
                                          u8 owner, f32 x, f32 y, f32 facing,
                                          bool newly_created,
                                          u8 variation) {
    // Thin dispatcher: materialize a wire-spawned entity through the SAME
    // create_* construction code the host uses (via the spawn_*_with_id id-seam),
    // so the client can never drift from the host. Z is sampled from world.terrain
    // inside the builders (local Z). Category is resolved from the
    // world's own type registry (already wired on the client's mirror world).
    if (world.handle_infos.has(entity_id)) return;

    // skip_birth: the wire's newly_created flag says whether this is a fresh birth
    // (play the clip) or a re-materialize / already-alive show (no clip).
    simulation::SpawnOpts opts;
    opts.skip_birth = !newly_created;

    const auto* types = world.types;
    if (!types) return;

    // Resolve category from the registries, same order the host uses. A
    // registry-MISS means projectile: it's the one non-registry entity, and its
    // model rides the type_id field (see resolve_spawn_wire on the host).
    if (types->get_item_type(type_id)) {
        simulation::spawn_item_with_id(world, entity_id, type_id, x, y, opts);
    } else if (types->get_doodad_type(type_id)) {
        simulation::spawn_doodad_with_id(world, entity_id, type_id, x, y, facing, variation);
    } else if (types->get_destructable_type(type_id)) {
        simulation::spawn_destructable_with_id(world, entity_id, type_id, x, y, facing, variation);
    } else if (types->get_unit_type(type_id)) {
        simulation::spawn_unit_with_id(world, entity_id, type_id, simulation::Player{owner}, x, y, facing, opts);
    } else {
        // Not in any registry → projectile; type_id carries the model path.
        simulation::spawn_projectile_with_id(world, entity_id, type_id, x, y, facing);
    }
}

void NetworkManager::destroy_client_entity(u32 entity_id) {
    simulation::remove_all_components(mirror_world(), entity_id);
}

void NetworkManager::reset_local_view() {
    // Only the client clears its mirror here (GameClient's sim world). Host/offline
    // point the mirror at the auth world (wiped separately by the scene-switch);
    // the worker has no mirror. The LocalView projection is cleared in every mode.
    if (m_mode == Mode::Client && m_mirror) {
        m_mirror->clear_entities();
    }
    m_local_discovered.clear();
    m_local_view_impl.clear();
}

void NetworkManager::project_local_view(const simulation::Simulation& sim,
                                        simulation::Player local, u32 placement_count) {
    // `src` = what LocalView's live reads resolve to: the authoritative World on
    // host/offline, or the network mirror on the client. Either way it's sim.world()
    // of the caller's active Simulation (GameServer's on host/offline, GameClient's
    // on a client) — the client's sim genuinely owns the mirror. LocalView::source
    // points at it.
    auto& src = const_cast<simulation::World&>(sim.world());
    const auto& vis = sim.vision();
    auto& view = m_local_view_impl;
    view.source = &src;
    auto& infos = src.handle_infos;

    // "Is this world position currently live-visible to the local player?" —
    // the reveal check for the snapshot reconcile below.
    const auto* terrain = sim.terrain();
    auto tile_live_visible = [&](const glm::vec3& pos) -> bool {
        if (!terrain) return true;
        auto t = terrain->world_to_tile(pos.x, pos.y);
        return vis.is_visible(local, static_cast<u32>(t.x), static_cast<u32>(t.y));
    };

    auto take_snapshot  = [&](u32 id) { view.snapshot_from(src, id); };
    auto drop_snapshot  = [&](u32 id) { view.drop_snapshot(id); };

    view.visible.clear();
    view.visible.reserve(infos.count());

    for (u32 i = 0; i < infos.count(); ++i) {
        u32 id = infos.ids()[i];
        const auto& info = infos.data()[i];
        if (info.hidden) {
            if (view.snapshotted(id)) drop_snapshot(id);
            continue;
        }

        // A CARRIED item isn't world-present — it lives in a carrier's inventory
        // slot and its ground Transform is frozen at the pickup tile. Running the
        // fog gate on that stale tile is meaningless AND buggy: when the carrier
        // crosses a ramp its cliff level changes, so is_unit_visible_to on the
        // stale pickup tile starts failing → the item drops from `view.visible` →
        // LocalView::item_info returns null → the HUD inventory icon vanishes.
        // Keep it live (its cold state — charges, icon — reads from source) and
        // skip the gate, exactly as host_broadcast_tick does for the wire.
        if (const auto* car = src.carriables.get(id);
            car && simulation::is_non_null_handle(car->carried_by)) {
            if (view.snapshotted(id)) drop_snapshot(id);
            view.visible.insert(id);
            continue;
        }

        // The local player's fog gate (the wire fork ships live-only instead).
        auto g = view_gate(src, sim, id, local, placement_count, m_local_discovered);
        if (!g.keep) continue;

        // Seed the OWNED selectable once per vision-entry so the picker has a
        // click volume that survives snapshotting (source may delete the entity).
        // The renderer refines it in-place on first draw via size_selectable.
        if (!view.own_selectables.has(id)) {
            if (const auto* as = src.selectables.get(id)) {
                view.own_selectables.add(id, simulation::Selectable{*as});
            }
        }

        // Static out of live sight → freeze its last-seen snapshot (a crate holds
        // its alive state even as it dies unseen). NOT in `visible`, so reads
        // resolve to the snapshot store.
        if (g.remembered && !g.live_vis) {
            take_snapshot(id);
            continue;
        }

        // Live: read straight from source. Re-scouted → drop the snapshot so
        // reads resolve to live truth again.
        if (view.snapshotted(id)) drop_snapshot(id);
        view.visible.insert(id);

        // Projectile death clip: source sets ProjectileComp::dying + queues
        // "death"; mirror it ONCE onto the OWNED queue (source's is never
        // consumed), latched on the owned queue's presence.
        if (const auto* ap = src.projectiles.get(id); ap && ap->dying) {
            if (!view.own_anim_queues.has(id)) {
                if (const auto* aq = src.anim_queues.get(id)) {
                    view.own_anim_queues.add(id, simulation::AnimQueue{*aq});
                } else {
                    simulation::AnimQueue q;
                    q.clips.push_back("death");
                    view.own_anim_queues.add(id, std::move(q));
                }
            }
        }
    }

    // Remove-on-reveal, EDGE-triggered. Drop a snapshot when the player re-scouts
    // its tile (host then S_SHOWs it live if still alive, or stays silent if it
    // died unseen → it correctly vanishes). "Reveal" must be a HIDDEN→VISIBLE
    // edge, not "tile visible now": the client's fog is computed from
    // interpolated (one-tick-behind) positions, so right after S_HIDE the tile
    // still reads live for ~1 tick — a level test would drop the snapshot the
    // instant it's made. So require the tile seen HIDDEN once first.
    std::vector<u32> thaw;
    for (u32 id : view.snapshot.handle_infos.ids()) {
        if (view.visible.count(id)) continue;
        const auto* ft = view.snapshot.transforms.get(id);
        bool tile_live = ft && tile_live_visible(ft->position);
        if (!tile_live) view.snapshot_hidden_seen.insert(id);        // arm the edge
        else if (view.snapshot_hidden_seen.count(id)) thaw.push_back(id);  // hidden→visible
        // else: live but not-yet-hidden = the post-S_HIDE lag window; hold.
    }
    for (u32 id : thaw) drop_snapshot(id);

    // Cull owned scratch for ids no longer visible or snapshotted.
    auto still_present = [&](u32 id) { return view.visible.count(id) || view.snapshotted(id); };
    std::vector<u32> drop_sel;
    for (u32 id : view.own_selectables.ids())
        if (!still_present(id)) drop_sel.push_back(id);
    for (u32 id : drop_sel) view.own_selectables.remove(id);
    std::vector<u32> drop_aq;
    for (u32 id : view.own_anim_queues.ids())
        if (!still_present(id)) drop_aq.push_back(id);
    for (u32 id : drop_aq) view.own_anim_queues.remove(id);

    // Rebuild per-tick iteration id-lists (visible ∪ snapshotted, per pool).
    view.iter_renderables.clear();
    view.iter_transforms.clear();
    view.iter_item_infos.clear();
    for (u32 id : view.visible) {
        if (src.renderables.has(id)) view.iter_renderables.push_back(id);
        if (src.transforms.has(id))  view.iter_transforms.push_back(id);
        if (src.item_infos.has(id))  view.iter_item_infos.push_back(id);
    }
    for (u32 id : view.snapshot.renderables.ids()) view.iter_renderables.push_back(id);
    for (u32 id : view.snapshot.transforms.ids())  view.iter_transforms.push_back(id);
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

    // Client derivation (attack/cooldown timers, projectile teardown, fog) lives
    // in Simulation::client_tick, driven from the engine's client branch. Corpse
    // lifecycle is host-driven: the client never advances corpse timers or hides a
    // corpse (visibility rides the synced `hidden` bit; corpse teardown is S_DESTROY).
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
    m_placement_count = 0;
    m_disconnected.clear();
    m_disconnected_view.clear();
    reset_local_view();

    // Session hygiene: wipe interp buffers + host per-tick set. The mirror is
    // owned by GameClient and cleared by its own shutdown() — just drop the
    // pointer so a reused NetworkManager never derefs a stale one.
    m_mirror = nullptr;
    m_snapshots[0] = Snapshot{};
    m_snapshots[1] = Snapshot{};
    m_snap_idx = 0;
    m_has_two_snaps = false;
    m_prev_tick_entities.clear();
}

} // namespace uldum::network
