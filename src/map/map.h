#pragma once

#include "map/terrain_data.h"

#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>

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

// Layer type — determines engine rendering behavior
enum class LayerType : u8 { Ground, WaterShallow, WaterDeep, Grass };

struct TilesetLayer {
    u32         id = 0;
    std::string name;
    std::string diffuse_path;   // relative to map root
    LayerType   type = LayerType::Ground;

    // Water properties (only used when type is WaterShallow or WaterDeep)
    glm::vec3   water_color{0.1f, 0.3f, 0.5f};
    f32         water_opacity = 0.6f;
    f32         water_wave_speed = 0.4f;
};

struct Tileset {
    std::string name;
    std::vector<TilesetLayer> layers;

    const TilesetLayer* get_layer(u32 id) const {
        for (auto& l : layers) { if (l.id == id) return &l; }
        return nullptr;
    }
    bool is_water(u32 layer_id) const {
        auto* l = get_layer(layer_id);
        return l && (l->type == LayerType::WaterShallow || l->type == LayerType::WaterDeep);
    }
    void get_water_layer_ids(std::vector<u8>& shallow, std::vector<u8>& deep) const {
        for (auto& l : layers) {
            if (l.type == LayerType::WaterShallow) shallow.push_back(static_cast<u8>(l.id));
            else if (l.type == LayerType::WaterDeep) deep.push_back(static_cast<u8>(l.id));
        }
    }
};

struct MapManifest {
    std::string id;
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

    // Fog of war: "none", "explored", or "unexplored" (default: "none")
    std::string fog_of_war = "none";

    // Reconnect settings
    f32  disconnect_timeout = 60.0f;   // seconds to wait before dropping a disconnected player
    bool pause_on_disconnect = false;  // pause the game when a player disconnects

    // Input configuration
    std::string input_preset = "rts";
    nlohmann::json input_bindings_json;  // raw "bindings" object from manifest
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
    const Tileset&     tileset()  const { return m_tileset; }
    const TerrainData& terrain()  const { return m_scene.terrain; }
    TerrainData&       terrain()        { return m_scene.terrain; }
    const SceneData&   scene()    const { return m_scene; }

    const std::string& map_root() const { return m_map_root; }

    // List scene directories found under the map root.
    std::vector<std::string> list_scenes() const;

    // Switch to a different scene (reloads terrain + placements).
    bool switch_scene(std::string_view scene_name, asset::AssetManager& assets, simulation::Simulation& sim);

private:
    bool load_manifest(asset::AssetManager& assets);
    bool load_tileset(asset::AssetManager& assets);
    bool load_types(asset::AssetManager& assets, simulation::Simulation& sim);
    bool load_scene(std::string_view scene_name, asset::AssetManager& assets, simulation::Simulation& sim);
    bool load_placements(std::string_view scene_name, asset::AssetManager& assets, simulation::Simulation& sim);

    bool        m_loaded = false;
    std::string m_map_root;
    MapManifest m_manifest;
    Tileset     m_tileset;
    SceneData   m_scene;
};

} // namespace uldum::map
