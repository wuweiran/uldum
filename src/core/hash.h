#pragma once

#include "core/types.h"

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace uldum {

// SHA-256 — FIPS 180-4. Used for map fingerprinting (script hash) and
// any other content-identity needs. Self-contained, no third-party dep.
//
// Streaming usage:
//   Sha256 h;
//   h.update(bytes1); h.update(bytes2); ...
//   auto digest = h.finalize();
//
// One-shot:
//   auto digest = sha256(bytes);
class Sha256 {
public:
    Sha256();
    void update(std::span<const u8> data);
    void update(std::string_view data);
    std::array<u8, 32> finalize();

private:
    void compress(const u8 block[64]);

    u32 m_state[8];
    u64 m_bit_count = 0;
    u8  m_buffer[64];
    u32 m_buffer_len = 0;
};

inline std::array<u8, 32> sha256(std::span<const u8> data) {
    Sha256 h;
    h.update(data);
    return h.finalize();
}

inline std::array<u8, 32> sha256(std::string_view data) {
    Sha256 h;
    h.update(data);
    return h.finalize();
}

// Lowercase hex string of a 32-byte digest (64 chars).
std::string to_hex(const std::array<u8, 32>& digest);

} // namespace uldum
