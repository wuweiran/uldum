#pragma once

#include "map/terrain_data.h"

#include <string_view>

namespace uldum::map {

class MapManager {
public:
    bool init();
    void shutdown();

    bool load_map(std::string_view path);
    void unload_map();
    bool is_loaded() const { return m_loaded; }

    // Terrain access — valid after init (procedural placeholder) or load_map.
    const TerrainData& terrain() const { return m_terrain; }
    TerrainData&       terrain()       { return m_terrain; }

    // Future API:
    // std::span<const PlacedObject> objects() const;
    // const MapManifest& manifest() const;
    // void apply_overrides(UnitTypeRegistry& registry);

private:
    bool m_loaded = false;
    TerrainData m_terrain;
};

} // namespace uldum::map
