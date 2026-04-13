#include "network/network.h"
#include "network/transport.h"
#include "network/enet_transport.h"
#include "network/protocol.h"
#include "simulation/simulation.h"
#include "simulation/type_registry.h"
#include "input/command_system.h"
#include "map/map.h"
#include "map/terrain_data.h"
#include "core/log.h"

#include <glm/gtc/constants.hpp>

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
    m_connected = false;
    log::info(TAG, "NetworkManager initialized — mode=Offline");
    return true;
}

// ── Host ─────────────────────────────────────────────────────────────────

bool NetworkManager::init_host(u16 port, u32 max_players,
                               simulation::Simulation& simulation,
                               input::CommandSystem& commands) {
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
    log::info(TAG, "NetworkManager initialized — mode=Host, port={}", port);
    return true;
}

void NetworkManager::host_on_connect(u32 peer_id) {
    log::info(TAG, "Peer {} connected, awaiting C_JOIN", peer_id);
}

void NetworkManager::host_on_disconnect(u32 peer_id) {
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        if (it->peer_id == peer_id) {
            log::info(TAG, "Player {} (peer {}) disconnected", it->player.id, peer_id);
            m_peers.erase(it);
            return;
        }
    }
}

void NetworkManager::host_on_receive(u32 peer_id, std::span<const u8> data) {
    if (data.empty()) return;
    auto type = peek_type(data);

    switch (type) {
    case MsgType::C_JOIN: {
        ByteReader r(data);
        r.read_u8();  // skip type
        u32 client_hash = r.read_u32();

        if (m_map_hash != 0 && client_hash != m_map_hash) {
            auto reject = build_reject(RejectReason::WrongMap);
            m_transport->send(peer_id, reject, true);
            log::warn(TAG, "Peer {} rejected: wrong map hash", peer_id);
            return;
        }

        // Assign player slot
        u32 slot = m_next_player_slot++;
        PeerInfo info{peer_id, simulation::Player{slot}, {}};

        // Send welcome
        auto welcome = build_welcome(slot,
            static_cast<u32>(m_simulation->world().handle_infos.count()),
            32);  // tick rate
        m_transport->send(peer_id, welcome, true);

        // Send spawn burst for all visible entities
        m_peers.push_back(std::move(info));
        host_send_spawn_burst(m_peers.back());

        log::info(TAG, "Player {} assigned to peer {}", slot, peer_id);
        break;
    }

    case MsgType::C_ORDER: {
        // Find which player this peer is
        simulation::Player player{UINT32_MAX};
        for (auto& p : m_peers) {
            if (p.peer_id == peer_id) { player = p.player; break; }
        }
        if (!player.is_valid()) return;

        auto cmd = parse_order(data, player);
        m_commands->submit(cmd);
        break;
    }

    case MsgType::C_LEAVE: {
        log::info(TAG, "Peer {} sent C_LEAVE", peer_id);
        break;
    }

    default:
        log::warn(TAG, "Host received unknown message type 0x{:02x}", static_cast<u8>(type));
        break;
    }
}

void NetworkManager::host_send_spawn_burst(PeerInfo& peer) {
    auto& world = m_simulation->world();
    auto& infos = world.handle_infos;

    for (u32 i = 0; i < infos.count(); ++i) {
        u32 id = infos.ids()[i];
        const auto& info = infos.data()[i];

        // Check fog visibility
        if (!is_visible_to(id, peer.player)) continue;

        const auto* transform = world.transforms.get(id);
        if (!transform) continue;

        const auto* owner = world.owners.get(id);
        u8 owner_id = owner ? static_cast<u8>(owner->player.id) : 0;

        auto msg = build_spawn(id, info.type_id, owner_id,
                               transform->position.x, transform->position.y,
                               transform->facing);
        m_transport->send(peer.peer_id, msg, true);
        peer.known_entities.insert(id);
    }

    log::info(TAG, "Sent {} entities to player {}", peer.known_entities.size(), peer.player.id);
}

bool NetworkManager::is_visible_to(u32 entity_id, simulation::Player player) const {
    auto& world = m_simulation->world();
    auto& fog = m_simulation->fog();

    // Own entities are always visible
    const auto* owner = world.owners.get(entity_id);
    if (owner && owner->player.id == player.id) return true;

    // Allied entities are always visible
    if (owner && m_simulation->is_allied(player, owner->player)) return true;

    // If fog is disabled, everything is visible
    if (!fog.enabled()) return true;

    // Check fog state
    const auto* transform = world.transforms.get(entity_id);
    if (!transform) return false;

    auto& terrain = *m_simulation->terrain();
    auto tile = terrain.world_to_tile(transform->position.x, transform->position.y);
    return fog.is_visible(player, static_cast<u32>(tile.x), static_cast<u32>(tile.y));
}

void NetworkManager::host_broadcast_tick(u32 tick) {
    if (m_mode != Mode::Host || m_peers.empty()) return;

    auto& world = m_simulation->world();
    auto& infos = world.handle_infos;

    for (auto& peer : m_peers) {
        // Track which entities are visible this tick
        std::unordered_set<u32> visible_now;
        std::vector<EntityState> states;

        for (u32 i = 0; i < infos.count(); ++i) {
            u32 id = infos.ids()[i];

            if (!is_visible_to(id, peer.player)) continue;
            visible_now.insert(id);

            // Send S_SPAWN for newly visible entities
            if (!peer.known_entities.contains(id)) {
                const auto& info = infos.data()[i];
                const auto* transform = world.transforms.get(id);
                if (!transform) continue;
                const auto* owner = world.owners.get(id);
                u8 owner_id = owner ? static_cast<u8>(owner->player.id) : 0;

                auto msg = build_spawn(id, info.type_id, owner_id,
                                       transform->position.x, transform->position.y,
                                       transform->facing);
                m_transport->send(peer.peer_id, msg, true);
                peer.known_entities.insert(id);
            }

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
                flags |= 0x02;
                es.target_id = combat->target.id;
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

    // Forward sounds to clients
    // (sounds are wired via on_sound callback in engine — handled there)
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
    m_connected = false;  // true after S_WELCOME
    log::info(TAG, "NetworkManager initialized — mode=Client, connecting to {}:{}", address, port);
    return true;
}

void NetworkManager::client_on_connect(u32 /*peer_id*/) {
    log::info(TAG, "Connected to server, sending C_JOIN");
    auto msg = build_join(m_map_hash);
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
    case MsgType::S_WELCOME: client_handle_welcome(data); break;
    case MsgType::S_REJECT: {
        ByteReader r(data);
        r.read_u8();
        u8 reason = r.read_u8();
        log::error(TAG, "Server rejected connection (reason={})", reason);
        break;
    }
    case MsgType::S_SPAWN:   client_handle_spawn(data); break;
    case MsgType::S_DESTROY: client_handle_destroy(data); break;
    case MsgType::S_STATE:   client_handle_state(data); break;
    case MsgType::S_SOUND:   client_handle_sound(data); break;
    default:
        log::warn(TAG, "Client received unknown message type 0x{:02x}", static_cast<u8>(type));
        break;
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
    spawn_client_entity(s.entity_id, s.type_id, s.owner, s.x, s.y, s.facing);
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

        // Update movement state (for animation)
        auto* mov = m_client_world.movements.get(e.entity_id);
        if (mov) mov->moving = (e.flags & 0x01) != 0;
    }
}

void NetworkManager::spawn_client_entity(u32 entity_id, std::string_view type_id,
                                          u8 owner, f32 x, f32 y, f32 facing) {
    auto& world = m_client_world;

    // Skip if already exists
    if (world.handle_infos.has(entity_id)) return;

    simulation::Handle h = world.handles.reserve(entity_id);

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
                if (!destr_def->model_path.empty()) model_path = destr_def->model_path;
                max_health = destr_def->max_health;
            }
        }
    }

    world.handle_infos.add(entity_id, simulation::HandleInfo{std::string(type_id), cat, h.generation});

    simulation::Transform t;
    t.position = glm::vec3{x, y, 0};
    t.prev_position = t.position;
    t.facing = facing;
    t.prev_facing = facing;
    t.scale = scale;
    world.transforms.add(entity_id, std::move(t));
    world.owners.add(entity_id, simulation::Owner{simulation::Player{owner}});
    world.renderables.add(entity_id, simulation::Renderable{model_path, true});
    world.healths.add(entity_id, simulation::Health{max_health, max_health, 0});
    world.movements.add(entity_id, simulation::Movement{});
    world.movements.get(entity_id)->collision_radius = collision_radius;

    if (sight_range > 0) {
        world.visions.add(entity_id, simulation::Vision{sight_range});
    }

    world.selectables.add(entity_id, simulation::Selectable{selection_radius, 5});
}

void NetworkManager::destroy_client_entity(u32 entity_id) {
    auto& world = m_client_world;
    world.transforms.remove(entity_id);
    world.handle_infos.remove(entity_id);
    world.owners.remove(entity_id);
    world.renderables.remove(entity_id);
    world.healths.remove(entity_id);
    world.movements.remove(entity_id);
    world.visions.remove(entity_id);
    world.selectables.remove(entity_id);
    world.combats.remove(entity_id);
    world.dead_states.remove(entity_id);
}

// ── Client fog of war ───────────────────────────────────────────────────

void NetworkManager::init_client_fog(const map::TerrainData& terrain,
                                      const map::MapManager& map,
                                      const simulation::Simulation& sim) {
    auto& manifest = map.manifest();
    simulation::FogMode fog_mode = simulation::FogMode::None;
    if (manifest.fog_of_war == "explored") fog_mode = simulation::FogMode::Explored;
    else if (manifest.fog_of_war == "unexplored") fog_mode = simulation::FogMode::Unexplored;

    m_client_fog.init(terrain.tiles_x, terrain.tiles_y, terrain.tile_size,
                      static_cast<u32>(manifest.players.size()), fog_mode, &terrain);
    m_client_sim_ref = &sim;
}

const f32* NetworkManager::update_client_fog(f32 dt) {
    if (!m_client_fog.enabled() || !m_local_player.is_valid()) return nullptr;
    if (m_client_sim_ref) {
        m_client_fog.update(m_client_world, *m_client_sim_ref);
    }
    return m_client_fog.update_visual(m_local_player, dt);
}

// ── Shared ──────────────────────────────────────────────────────────────

void NetworkManager::send_order(const input::GameCommand& cmd) {
    if (m_mode != Mode::Client || !m_transport) return;
    auto msg = build_order(cmd);
    m_transport->send(0, msg, true);
}

void NetworkManager::update() {
    if (m_mode == Mode::Offline) return;
    if (m_transport) m_transport->poll();

    // Client: apply interpolation each frame
    if (m_mode == Mode::Client && m_has_two_snaps) {
        client_apply_interpolation();
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
}

} // namespace uldum::network
