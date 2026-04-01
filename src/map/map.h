#pragma once

#include "map/terrain_data.h"

#include <string>
#include <string_view>
#include <vector>

namespace uldum::asset { class AssetManager; }
namespace uldum::simulation { class Simulation; struct World; }

namespace uldum::map {

struct PlayerSlot {
    u32         slot = 0;
    std::string type;   // "human", "computer", "open"
    u32         team = 0;
    std::string name;
    std::string color;
};

struct TeamDef {
    u32         id = 0;
    std::string name;
    bool        allied = false;
    bool        shared_vision = false;
};

struct MapManifest {
    std::string name;
    std::string author;
    std::string description;
    std::string version;
    std::string engine_version;
    std::string game_mode;
    std::string suggested_players;
    std::string tileset_path;
    std::string start_scene;

    std::vector<PlayerSlot> players;
    std::vector<TeamDef>    teams;

    // Map-defined enumerations
    std::vector<std::string> classifications;
    std::vector<std::string> attack_types;
    std::vector<std::string> armor_types;
    std::vector<std::string> attributes;
};

struct PlacedUnit {
    std::string type;
    f32 x = 0, y = 0, facing = 0;
    u32 owner = 0;
};

struct PlacedDestructable {
    std::string type;
    f32 x = 0, y = 0, facing = 0;
    u8  variation = 0;
};

struct PlacedItem {
    std::string type;
    f32 x = 0, y = 0;
};

struct Region {
    std::string name;
    f32 x = 0, y = 0, width = 0, height = 0;
};

struct CameraDef {
    std::string name;
    f32 x = 0, y = 0, z = 0, pitch = 0, yaw = 0;
};

struct SceneData {
    TerrainData                     terrain;
    std::vector<PlacedUnit>         units;
    std::vector<PlacedDestructable> destructables;
    std::vector<PlacedItem>         items;
    std::vector<Region>             regions;
    std::vector<CameraDef>          cameras;
};

class MapManager {
public:
    bool init();
    void shutdown();

    // Load a map package from a directory path. Loads manifest, types, and start scene.
    // Requires AssetManager for file loading and Simulation for type registration + entity creation.
    bool load_map(std::string_view path, asset::AssetManager& assets, simulation::Simulation& sim);
    void unload_map();
    bool is_loaded() const { return m_loaded; }

    const MapManifest& manifest() const { return m_manifest; }
    const TerrainData& terrain()  const { return m_scene.terrain; }
    TerrainData&       terrain()        { return m_scene.terrain; }
    const SceneData&   scene()    const { return m_scene; }

    const std::string& map_root() const { return m_map_root; }

private:
    bool load_manifest(asset::AssetManager& assets);
    bool load_types(asset::AssetManager& assets, simulation::Simulation& sim);
    bool load_scene(std::string_view scene_name, asset::AssetManager& assets, simulation::Simulation& sim);
    bool load_placements(std::string_view scene_name, asset::AssetManager& assets, simulation::Simulation& sim);

    bool        m_loaded = false;
    std::string m_map_root;
    MapManifest m_manifest;
    SceneData   m_scene;
};

} // namespace uldum::map
