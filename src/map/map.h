#pragma once

#include <string_view>

namespace uldum::map {

class MapManager {
public:
    bool init();
    void shutdown();

    bool load_map(std::string_view path);
    void unload_map();
    bool is_loaded() const { return m_loaded; }

    // Future API:
    // const TerrainData& terrain() const;
    // std::span<const PlacedObject> objects() const;
    // const MapManifest& manifest() const;
    // void apply_overrides(UnitTypeRegistry& registry);

private:
    bool m_loaded = false;
};

} // namespace uldum::map
