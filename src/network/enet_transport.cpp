#include "network/enet_transport.h"
#include "core/log.h"

#include <enet/enet.h>

namespace uldum::network {

static constexpr const char* TAG = "ENet";
static constexpr u32 NUM_CHANNELS = 2;  // 0 = reliable, 1 = unreliable

static bool s_enet_initialized = false;

static bool ensure_enet() {
    if (s_enet_initialized) return true;
    if (enet_initialize() != 0) {
        log::error(TAG, "enet_initialize() failed");
        return false;
    }
    s_enet_initialized = true;
    return true;
}

ENetTransport::~ENetTransport() {
    disconnect();
}

bool ENetTransport::host(u16 port, u32 max_clients) {
    if (!ensure_enet()) return false;

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;

    m_host = enet_host_create(&address, max_clients, NUM_CHANNELS, 0, 0);
    if (!m_host) {
        log::error(TAG, "Failed to create server host on port {}", port);
        return false;
    }

    m_is_server = true;
    log::info(TAG, "Server listening on port {}", port);
    return true;
}

bool ENetTransport::connect(std::string_view address, u16 port) {
    if (!ensure_enet()) return false;

    // Client host: 1 connection, 2 channels
    m_host = enet_host_create(nullptr, 1, NUM_CHANNELS, 0, 0);
    if (!m_host) {
        log::error(TAG, "Failed to create client host");
        return false;
    }

    ENetAddress addr{};
    std::string addr_str(address);
    enet_address_set_host(&addr, addr_str.c_str());
    addr.port = port;

    ENetPeer* peer = enet_host_connect(m_host, &addr, NUM_CHANNELS, 0);
    if (!peer) {
        log::error(TAG, "Failed to initiate connection to {}:{}", address, port);
        enet_host_destroy(m_host);
        m_host = nullptr;
        return false;
    }

    // Peer ID 0 = the server (from client's perspective)
    m_id_to_peer[0] = peer;
    m_peer_to_id[peer] = 0;
    m_is_server = false;

    log::info(TAG, "Connecting to {}:{}...", address, port);
    return true;
}

void ENetTransport::disconnect() {
    if (!m_host) return;

    // Graceful disconnect for all peers
    for (auto& [id, peer] : m_id_to_peer) {
        enet_peer_disconnect(peer, 0);
    }

    // Flush pending disconnects
    ENetEvent event;
    while (enet_host_service(m_host, &event, 100) > 0) {
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(event.packet);
        }
    }

    enet_host_destroy(m_host);
    m_host = nullptr;
    m_id_to_peer.clear();
    m_peer_to_id.clear();
    m_next_peer_id = 0;
}

void ENetTransport::send(u32 peer_id, std::span<const u8> data, bool reliable) {
    auto it = m_id_to_peer.find(peer_id);
    if (it == m_id_to_peer.end()) return;

    u32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    u8 channel = reliable ? 0 : 1;

    ENetPacket* packet = enet_packet_create(data.data(), data.size(), flags);
    enet_peer_send(it->second, channel, packet);
}

void ENetTransport::broadcast(std::span<const u8> data, bool reliable) {
    if (!m_host) return;

    u32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    u8 channel = reliable ? 0 : 1;

    ENetPacket* packet = enet_packet_create(data.data(), data.size(), flags);
    enet_host_broadcast(m_host, channel, packet);
}

void ENetTransport::poll() {
    if (!m_host) return;

    ENetEvent event;
    while (enet_host_service(m_host, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            u32 peer_id;
            if (m_is_server) {
                peer_id = m_next_peer_id++;
                m_id_to_peer[peer_id] = event.peer;
                m_peer_to_id[event.peer] = peer_id;
            } else {
                // Client: server is always peer 0 (already set in connect())
                peer_id = 0;
            }
            log::info(TAG, "Peer {} connected", peer_id);
            if (on_connect) on_connect(peer_id);
            break;
        }

        case ENET_EVENT_TYPE_RECEIVE: {
            auto pit = m_peer_to_id.find(event.peer);
            if (pit != m_peer_to_id.end() && on_receive) {
                std::span<const u8> data(event.packet->data, event.packet->dataLength);
                on_receive(pit->second, data);
            }
            enet_packet_destroy(event.packet);
            break;
        }

        case ENET_EVENT_TYPE_DISCONNECT: {
            auto pit = m_peer_to_id.find(event.peer);
            if (pit != m_peer_to_id.end()) {
                u32 peer_id = pit->second;
                log::info(TAG, "Peer {} disconnected", peer_id);
                if (on_disconnect) on_disconnect(peer_id);
                m_id_to_peer.erase(peer_id);
                m_peer_to_id.erase(pit);
            }
            break;
        }

        default:
            break;
        }
    }
}

} // namespace uldum::network
