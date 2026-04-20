#include "asset/upk.h"
#include "core/log.h"

#include <fstream>

namespace uldum::asset {

static constexpr const char* TAG = "UPK";

// Shared parse logic — reads header + entry table from a contiguous byte
// buffer. Used by both open (file → buffer) and open_from_memory (buffer).
static bool parse_archive_header(const u8* data, size_t size,
                                 UPKHeader& out_header,
                                 std::vector<UPKEntry>& out_entries,
                                 std::string_view label) {
    if (size < sizeof(UPKHeader)) {
        log::error(TAG, "'{}' truncated — size {} < header {}", label, size, sizeof(UPKHeader));
        return false;
    }
    std::memcpy(&out_header, data, sizeof(UPKHeader));
    if (std::memcmp(out_header.magic, UPK_MAGIC, 4) != 0) {
        log::error(TAG, "'{}' is not a valid UPK file", label);
        return false;
    }
    if (out_header.version != UPK_VERSION) {
        log::error(TAG, "'{}' has unsupported version {} (expected {})",
                   label, out_header.version, UPK_VERSION);
        return false;
    }

    size_t pos = sizeof(UPKHeader);
    out_entries.clear();
    out_entries.reserve(out_header.file_count);
    for (u32 i = 0; i < out_header.file_count; ++i) {
        if (pos + sizeof(u16) > size) {
            log::error(TAG, "'{}' entry {} truncated (path_len)", label, i);
            return false;
        }
        u16 path_len = 0;
        std::memcpy(&path_len, data + pos, sizeof(u16));
        pos += sizeof(u16);

        if (pos + path_len + 3 * sizeof(u32) > size) {
            log::error(TAG, "'{}' entry {} truncated (path/offsets)", label, i);
            return false;
        }
        UPKEntry e;
        e.path.assign(reinterpret_cast<const char*>(data + pos), path_len);
        pos += path_len;
        std::memcpy(&e.offset,      data + pos, sizeof(u32)); pos += sizeof(u32);
        std::memcpy(&e.raw_size,    data + pos, sizeof(u32)); pos += sizeof(u32);
        std::memcpy(&e.stored_size, data + pos, sizeof(u32)); pos += sizeof(u32);
        e.name_hash = upk_hash(e.path);
        out_entries.push_back(std::move(e));
    }
    std::sort(out_entries.begin(), out_entries.end(),
              [](const UPKEntry& a, const UPKEntry& b) { return a.name_hash < b.name_hash; });
    return true;
}

bool UPKReader::open(std::string_view path, std::string_view encryption_key) {
    close();

    std::ifstream file(std::string(path), std::ios::binary | std::ios::ate);
    if (!file) return false;
    auto size = file.tellg();
    file.seekg(0);
    std::vector<u8> buf(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(buf.data()), size);
    if (!file) return false;

    if (!parse_archive_header(buf.data(), buf.size(), m_header, m_entries, path)) {
        close();
        return false;
    }

    m_encrypted = (m_header.flags & UPK_FLAG_ENCRYPTED) != 0;
    if (m_encrypted) {
        if (encryption_key.empty()) {
            log::error(TAG, "'{}' is encrypted but no key provided", path);
            close();
            return false;
        }
        upk_derive_key(encryption_key, m_key);
    }

    m_path = std::string(path);
    m_bytes = std::move(buf);
    log::info(TAG, "Opened '{}' — {} files{}", path, m_header.file_count,
              m_encrypted ? " [encrypted]" : "");
    return true;
}

bool UPKReader::open_from_memory(std::vector<u8> bytes, std::string_view encryption_key,
                                 std::string_view debug_label) {
    close();

    if (!parse_archive_header(bytes.data(), bytes.size(), m_header, m_entries, debug_label)) {
        close();
        return false;
    }

    m_encrypted = (m_header.flags & UPK_FLAG_ENCRYPTED) != 0;
    if (m_encrypted) {
        if (encryption_key.empty()) {
            log::error(TAG, "'{}' is encrypted but no key provided", debug_label);
            close();
            return false;
        }
        upk_derive_key(encryption_key, m_key);
    }

    m_bytes = std::move(bytes);
    log::info(TAG, "Opened '{}' from memory — {} files{}", debug_label, m_header.file_count,
              m_encrypted ? " [encrypted]" : "");
    return true;
}

void UPKReader::close() {
    m_path.clear();
    m_bytes.clear();
    m_bytes.shrink_to_fit();
    m_entries.clear();
    m_header = {};
    m_encrypted = false;
    std::memset(m_key, 0, UPK_KEY_LEN);
}

const UPKEntry* UPKReader::find_entry(u64 hash) const {
    // Binary search on sorted entries
    auto it = std::lower_bound(m_entries.begin(), m_entries.end(), hash,
        [](const UPKEntry& e, u64 h) { return e.name_hash < h; });
    if (it != m_entries.end() && it->name_hash == hash) return &(*it);
    return nullptr;
}

bool UPKReader::contains(std::string_view file_path) const {
    auto norm = upk_normalize_path(file_path);
    return find_entry(upk_hash(norm)) != nullptr;
}

std::vector<u8> UPKReader::read(std::string_view file_path) const {
    auto norm = upk_normalize_path(file_path);
    return read_by_hash(upk_hash(norm));
}

std::vector<u8> UPKReader::read_by_hash(u64 hash) const {
    const auto* entry = find_entry(hash);
    if (!entry) return {};

    // Both file-backed (open) and memory-backed (open_from_memory) paths store
    // the full archive in m_bytes — reads slice from there. File-backed path
    // loaded it once in open(); no per-read disk I/O.
    if (entry->offset + entry->stored_size > m_bytes.size()) return {};
    std::vector<u8> data(m_bytes.begin() + entry->offset,
                         m_bytes.begin() + entry->offset + entry->stored_size);

    if (m_encrypted) {
        upk_xor(data.data(), static_cast<u32>(data.size()), m_key);
    }

    // TODO: decompress if FLAG_COMPRESSED (LZ4, future)

    return data;
}

} // namespace uldum::asset
