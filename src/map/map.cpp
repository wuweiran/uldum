#include "map/map.h"
#include "core/log.h"

namespace uldum::map {

static constexpr const char* TAG = "Map";

bool MapManager::init() {
    // Create procedural placeholder terrain until real map loading (Phase 6)
    m_terrain = create_procedural_terrain(64, 64, 2.0f);

    log::info(TAG, "MapManager initialized — procedural terrain {}x{}", m_terrain.tiles_x, m_terrain.tiles_y);
    return true;
}

void MapManager::shutdown() {
    unload_map();
    log::info(TAG, "MapManager shut down");
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
    m_terrain = {};
}

} // namespace uldum::map
