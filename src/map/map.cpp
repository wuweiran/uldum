#include "map/map.h"
#include "core/log.h"

namespace uldum::map {

static constexpr const char* TAG = "Map";

bool MapManager::init() {
    log::info(TAG, "MapManager initialized (stub) — FlatBuffers terrain/objects loading pending");
    return true;
}

void MapManager::shutdown() {
    unload_map();
    log::info(TAG, "MapManager shut down (stub)");
}

bool MapManager::load_map(std::string_view path) {
    log::info(TAG, "load_map (stub) — would load '{}'", path);
    m_loaded = true;
    return true;
}

void MapManager::unload_map() {
    if (m_loaded) {
        log::info(TAG, "unload_map (stub)");
        m_loaded = false;
    }
}

} // namespace uldum::map
