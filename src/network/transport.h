#pragma once

#include "core/types.h"

#include <functional>
#include <span>
#include <string_view>

namespace uldum::network {

// Abstract network transport. ENet for Phase 13b; can be replaced with
// QUIC or TCP fallback later without changing game code.
class Transport {
public:
    virtual ~Transport() = default;

    // Server: start listening on port, accept up to max_clients.
    virtual bool host(u16 port, u32 max_clients) = 0;

    // Client: connect to a remote host.
    virtual bool connect(std::string_view address, u16 port) = 0;

    // Graceful disconnect (client or server).
    virtual void disconnect() = 0;

    // Send data to a specific peer. reliable = channel 0, unreliable = channel 1.
    virtual void send(u32 peer_id, std::span<const u8> data, bool reliable) = 0;

    // Send data to all connected peers.
    virtual void broadcast(std::span<const u8> data, bool reliable) = 0;

    // Process incoming events. Fires on_connect/on_disconnect/on_receive callbacks.
    virtual void poll() = 0;

    // Callbacks — set by NetworkManager before use.
    std::function<void(u32 peer_id)> on_connect;
    std::function<void(u32 peer_id)> on_disconnect;
    std::function<void(u32 peer_id, std::span<const u8> data)> on_receive;
};

} // namespace uldum::network
