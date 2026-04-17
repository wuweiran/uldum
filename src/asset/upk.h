#pragma once

#include "core/types.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace uldum::asset {

// ── UPK format constants ────────────────────────────────────────────────────
//
// On-disk layout:
//   Header (16 bytes):                          — fixed
//     u8[4] magic = "UPK\0"
//     u32   version = 1
//     u32   file_count
//     u32   flags    (bit 0: compressed, bit 1: encrypted)
//
//   Entry table (variable-length, file_count entries). For each entry:
//     u16    path_len
//     char[] path           (normalized: forward slashes, lowercase, UTF-8)
//     u32    offset         (bytes from start of file to data blob)
//     u32    raw_size       (original size)
//     u32    stored_size    (size after compression/encryption)
//
//   Data blobs (contiguous at the offsets listed in entries)
//
// Encryption (XOR) applies to data blobs only. Paths are plaintext so that
// unpack can restore the original folder structure exactly.

static constexpr u8  UPK_MAGIC[4] = {'U', 'P', 'K', 0};
static constexpr u32 UPK_VERSION  = 1;
static constexpr u32 UPK_FLAG_COMPRESSED = 1 << 0;
static constexpr u32 UPK_FLAG_ENCRYPTED  = 1 << 1;
static constexpr u32 UPK_KEY_LEN = 32;

struct UPKHeader {
    u8  magic[4];
    u32 version;
    u32 file_count;
    u32 flags;
};

// In-memory entry. On disk, `path` is preceded by a u16 path_len and
// followed by (offset, raw_size, stored_size); `name_hash` is a cache of
// upk_hash(path) for fast lookup and is not stored in the file.
struct UPKEntry {
    std::string path;
    u64 name_hash;
    u32 offset;
    u32 raw_size;
    u32 stored_size;
};

// ── FNV-1a 64-bit hash ─────────────────────────────────────────────────────

inline u64 upk_hash(std::string_view s) {
    u64 h = 14695981039346656037ULL;
    for (char c : s) { h ^= static_cast<u8>(c); h *= 1099511628211ULL; }
    return h;
}

// Normalize path: forward slashes, lowercase, no leading ./
inline std::string upk_normalize_path(std::string_view path) {
    std::string s(path);
    std::replace(s.begin(), s.end(), '\\', '/');
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s.starts_with("./")) s = s.substr(2);
    return s;
}

// ── XOR encryption ──────────────────────────────────────────────────────────

inline void upk_derive_key(std::string_view secret, u8 key[UPK_KEY_LEN]) {
    auto h = [](std::string_view s) -> u64 { return upk_hash(s); };
    std::string base(secret);
    u64 h1 = h(base + "_part1"), h2 = h(base + "_part2");
    u64 h3 = h(base + "_part3"), h4 = h(base + "_part4");
    std::memcpy(key,      &h1, 8);
    std::memcpy(key + 8,  &h2, 8);
    std::memcpy(key + 16, &h3, 8);
    std::memcpy(key + 24, &h4, 8);
}

inline void upk_xor(u8* data, u32 size, const u8 key[UPK_KEY_LEN]) {
    for (u32 i = 0; i < size; ++i) data[i] ^= key[i % UPK_KEY_LEN];
}

// ── UPK Reader (runtime) ───────────────────────────────────────────────────

class UPKReader {
public:
    // Open a .upk archive. Returns false if file doesn't exist or is invalid.
    bool open(std::string_view path, std::string_view encryption_key = "");

    void close();

    bool is_open() const { return !m_path.empty(); }

    // Check if a file exists in the archive (by normalized path).
    bool contains(std::string_view file_path) const;

    // Read a file from the archive. Returns empty vector if not found.
    std::vector<u8> read(std::string_view file_path) const;

    // Read a file by pre-computed hash.
    std::vector<u8> read_by_hash(u64 hash) const;

    u32 file_count() const { return static_cast<u32>(m_entries.size()); }

private:
    const UPKEntry* find_entry(u64 hash) const;

    std::string           m_path;
    UPKHeader             m_header{};
    std::vector<UPKEntry> m_entries;  // sorted by name_hash
    bool                  m_encrypted = false;
    u8                    m_key[UPK_KEY_LEN]{};
};

} // namespace uldum::asset
