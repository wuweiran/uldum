#include "asset/upk.h"
#include "core/log.h"

#include <fstream>

namespace uldum::asset {

static constexpr const char* TAG = "UPK";

bool UPKReader::open(std::string_view path, std::string_view encryption_key) {
    close();

    std::ifstream file(std::string(path), std::ios::binary);
    if (!file) return false;

    // Read header
    file.read(reinterpret_cast<char*>(&m_header), sizeof(m_header));
    if (std::memcmp(m_header.magic, UPK_MAGIC, 4) != 0) {
        log::error(TAG, "'{}' is not a valid UPK file", path);
        return false;
    }
    if (m_header.version != UPK_VERSION) {
        log::error(TAG, "'{}' has unsupported version {}", path, m_header.version);
        return false;
    }

    // Read file table
    m_entries.resize(m_header.file_count);
    file.read(reinterpret_cast<char*>(m_entries.data()),
              m_header.file_count * sizeof(UPKEntry));

    // Setup encryption
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
