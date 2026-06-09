#include "core/hash.h"

#include <cstring>

namespace uldum {

namespace {

constexpr u32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline u32 rotr(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }

} // namespace

Sha256::Sha256() {
    reset();
}

void Sha256::reset() {
    m_state[0] = 0x6a09e667;
    m_state[1] = 0xbb67ae85;
    m_state[2] = 0x3c6ef372;
    m_state[3] = 0xa54ff53a;
    m_state[4] = 0x510e527f;
    m_state[5] = 0x9b05688c;
    m_state[6] = 0x1f83d9ab;
    m_state[7] = 0x5be0cd19;
    m_bit_count = 0;
    m_buffer_len = 0;
}

void Sha256::compress(const u8 block[64]) {
    u32 w[64];
    for (u32 i = 0; i < 16; ++i) {
        w[i] = (static_cast<u32>(block[i*4    ]) << 24)
             | (static_cast<u32>(block[i*4 + 1]) << 16)
             | (static_cast<u32>(block[i*4 + 2]) << 8)
             |  static_cast<u32>(block[i*4 + 3]);
    }
    for (u32 i = 16; i < 64; ++i) {
        u32 s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        u32 s1 = rotr(w[i-2], 17) ^ rotr(w[i-2],  19) ^ (w[i-2]  >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    u32 a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
    u32 e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];

    for (u32 i = 0; i < 64; ++i) {
        u32 S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        u32 ch = (e & f) ^ ((~e) & g);
        u32 t1 = h + S1 + ch + K[i] + w[i];
        u32 S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        u32 mj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    m_state[0] += a; m_state[1] += b; m_state[2] += c; m_state[3] += d;
    m_state[4] += e; m_state[5] += f; m_state[6] += g; m_state[7] += h;
}

void Sha256::update(std::span<const u8> data) {
    m_bit_count += static_cast<u64>(data.size()) * 8;
    usize i = 0;
    if (m_buffer_len > 0) {
        usize need = 64 - m_buffer_len;
        usize take = (data.size() < need) ? data.size() : need;
        std::memcpy(m_buffer + m_buffer_len, data.data(), take);
        m_buffer_len += static_cast<u32>(take);
        i += take;
        if (m_buffer_len == 64) {
            compress(m_buffer);
            m_buffer_len = 0;
        }
    }
    while (i + 64 <= data.size()) {
        compress(data.data() + i);
        i += 64;
    }
    if (i < data.size()) {
        std::memcpy(m_buffer, data.data() + i, data.size() - i);
        m_buffer_len = static_cast<u32>(data.size() - i);
    }
}

void Sha256::update(std::string_view data) {
    update({reinterpret_cast<const u8*>(data.data()), data.size()});
}

std::array<u8, 32> Sha256::finalize() {
    // Append 0x80, zero-pad so that (length+1+8) % 64 == 0, then 64-bit big-endian bit count.
    u64 bits = m_bit_count;
    u8 pad[64] = {0};
    pad[0] = 0x80;
    u32 pad_len = (m_buffer_len < 56) ? (56 - m_buffer_len) : (120 - m_buffer_len);
    update({pad, pad_len});
    u8 lenbuf[8];
    for (int i = 0; i < 8; ++i) lenbuf[7 - i] = static_cast<u8>(bits >> (i * 8));
    update({lenbuf, 8});

    std::array<u8, 32> out{};
    for (u32 i = 0; i < 8; ++i) {
        out[i*4    ] = static_cast<u8>(m_state[i] >> 24);
        out[i*4 + 1] = static_cast<u8>(m_state[i] >> 16);
        out[i*4 + 2] = static_cast<u8>(m_state[i] >> 8);
        out[i*4 + 3] = static_cast<u8>(m_state[i]);
    }
    // Re-seed so the object can be reused for another digest. finalize
    // mutated m_state/bit_count/buffer via the padding compress; leaving
    // them as-is would make a second finalize continue from here and
    // produce garbage.
    reset();
    return out;
}

std::string to_hex(const std::array<u8, 32>& digest) {
    static const char* hex = "0123456789abcdef";
    std::string s(64, '0');
    for (u32 i = 0; i < 32; ++i) {
        s[i*2    ] = hex[digest[i] >> 4];
        s[i*2 + 1] = hex[digest[i] & 0xF];
    }
    return s;
}

} // namespace uldum
