#pragma once

#include "network/transport.h"

#include <unordered_map>

struct _ENetHost;
struct _ENetPeer;

namespace uldum::network {

class ENetTransport : public Transport {
public:
    ~ENetTransport() override;

    bool host(u16 port, u32 max_clients) override;
    bool connect(std::string_view address, u16 port) override;
    void disconnect() override;
    void send(u32 peer_id, std::span<const u8> data, bool reliable) override;
    void broadcast(std::span<const u8> data, bool reliable) override;
    void poll() override;

private:
    _ENetHost* m_host = nullptr;
    bool m_is_server = false;

    // Peer tracking (server assigns IDs, client uses 0 for the server)
    std::unordered_map<u32, _ENetPeer*> m_id_to_peer;
    std::unordered_map<_ENetPeer*, u32> m_peer_to_id;
    u32 m_next_peer_id = 0;
};

} // namespace uldum::network
