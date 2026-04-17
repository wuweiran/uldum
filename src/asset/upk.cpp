#include "asset/upk.h"
#include "core/log.h"

#include <fstream>

namespace uldum::asset {

static constexpr const char* TAG = "UPK";

bool UPKReader::open(std::string_view path, std::string_view encryption_key) {
    close();

    std::ifstream file(std::string(path), std::ios::binary);
    if (!file) return false;

    file.read(reinterpret_cast<char*>(&m_header), sizeof(m_header));
    if (!file || std::memcmp(m_header.magic, UPK_MAGIC, 4) != 0) {
        log::error(TAG, "'{}' is not a valid UPK file", path);
        return false;
    }
    if (m_header.version != UPK_VERSION) {
        log::error(TAG, "'{}' has unsupported version {} (expected {})",
                   path, m_header.version, UPK_VERSION);
        return false;
    }

    // Variable-length entry table — read one entry at a time.
    m_entries.clear();
    m_entries.reserve(m_header.file_count);
    for (u32 i = 0; i < m_header.file_count; ++i) {
        UPKEntry e;
        u16 path_len = 0;
        file.read(reinterpret_cast<char*>(&path_len), sizeof(path_len));
        e.path.resize(path_len);
        if (path_len) file.read(e.path.data(), path_len);
        file.read(reinterpret_cast<char*>(&e.offset),      sizeof(e.offset));
        file.read(reinterpret_cast<char*>(&e.raw_size),    sizeof(e.raw_size));
        file.read(reinterpret_cast<char*>(&e.stored_size), sizeof(e.stored_size));
        if (!file) {
            log::error(TAG, "'{}' entry table truncated at entry {}", path, i);
            close();
            return false;
        }
        e.name_hash = upk_hash(e.path);
        m_entries.push_back(std::move(e));
    }
    std::sort(m_entries.begin(), m_entries.end(),
              [](const UPKEntry& a, const UPKEntry& b) { return a.name_hash < b.name_hash; });

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
    log::info(TAG, "Opened '{}' — {} files{}", path, m_header.file_count,
              m_encrypted ? " [encrypted]" : "");
    return true;
}

void UPKReader::close() {
    m_path.clear();
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

    std::ifstream file(m_path, std::ios::binary);
    if (!file) return {};

    file.seekg(entry->offset);
    std::vector<u8> data(entry->stored_size);
    file.read(reinterpret_cast<char*>(data.data()), entry->stored_size);

    if (m_encrypted) {
        upk_xor(data.data(), static_cast<u32>(data.size()), m_key);
    }

    // TODO: decompress if FLAG_COMPRESSED (LZ4, future)

    return data;
}

} // namespace uldum::asset
