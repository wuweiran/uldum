#include "ui/file_interface.h"
#include "asset/asset.h"
#include "core/log.h"

#include <cstring>
#include <fstream>

namespace uldum::ui {

static constexpr const char* TAG = "UI";

Rml::FileHandle FileInterface::Open(const Rml::String& path) {
    // Try the mounted packages / directories first (this is the path for
    // APK-bundled shell assets on Android and for shell/ mounted as a
    // DirectoryMount on desktop once we wire that up).
    std::vector<u8> bytes;
    if (auto* mgr = asset::AssetManager::instance()) {
        bytes = mgr->read_file_bytes(path.c_str());
    }

    // Fallback: direct filesystem read from the exe's working directory.
    // Lets the initial Shell UI integration work with loose `shell/` files
    // in dist/<Game>/ before we wire a DirectoryMount. Silent on miss —
    // RmlUi probes optional files (e.g. :hover decorators).
    if (bytes.empty()) {
        std::ifstream f(path.c_str(), std::ios::binary | std::ios::ate);
        if (f) {
            auto size = f.tellg();
            f.seekg(0);
            bytes.resize(static_cast<size_t>(size));
            f.read(reinterpret_cast<char*>(bytes.data()), size);
        }
    }
    if (bytes.empty()) return 0;

    auto handle = m_next_handle++;
    m_files.emplace(handle, OpenFile{std::move(bytes), 0});
    return handle;
}

void FileInterface::Close(Rml::FileHandle file) {
    m_files.erase(file);
}

size_t FileInterface::Read(void* buffer, size_t size, Rml::FileHandle file) {
    auto it = m_files.find(file);
    if (it == m_files.end()) return 0;
    auto& f = it->second;
    size_t remaining = f.bytes.size() - f.offset;
    size_t n = size < remaining ? size : remaining;
    if (n > 0) std::memcpy(buffer, f.bytes.data() + f.offset, n);
    f.offset += n;
    return n;
}

bool FileInterface::Seek(Rml::FileHandle file, long offset, int origin) {
    auto it = m_files.find(file);
    if (it == m_files.end()) return false;
    auto& f = it->second;

    size_t base = 0;
    switch (origin) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = f.offset; break;
        case SEEK_END: base = f.bytes.size(); break;
        default:       return false;
    }
    long target = static_cast<long>(base) + offset;
    if (target < 0 || static_cast<size_t>(target) > f.bytes.size()) return false;
    f.offset = static_cast<size_t>(target);
    return true;
}

size_t FileInterface::Tell(Rml::FileHandle file) {
    auto it = m_files.find(file);
    return it == m_files.end() ? 0 : it->second.offset;
}

size_t FileInterface::Length(Rml::FileHandle file) {
    auto it = m_files.find(file);
    return it == m_files.end() ? 0 : it->second.bytes.size();
}

} // namespace uldum::ui
