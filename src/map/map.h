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

// Player-slot declaration. The map declares the slot's team topology, color,
// and — optionally — whether the slot is a locked computer player. Array
// index into MapManifest::players is the player id; there is no separate
// slot number. Default (no `type`) means "lobby-decided human or open".
struct PlayerSlot {
    u32         team = 0;
    std::string color;
    std::string type;   // "" (lobby-decided) or "computer" (map-locked AI)
    std::string name;   // placeholder name; required for computer slots
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
    std::string normal_path;    // optional normal map (GL convention, relative to map root)
    LayerType   type = LayerType::Ground;

    // Water properties (only used when type is WaterShallow or WaterDeep)
    glm::vec3   water_color{0.1f, 0.3f, 0.5f};
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

// Environment settings — drives skybox, lighting, fog
struct EnvironmentConfig {
    // Sun / directional light
    glm::vec3 sun_direction{-0.4f, -0.5f, 0.8f};  // WC3-style: shadows to upper-right
    glm::vec3 sun_color{1.0f, 1.0f, 0.9f};
    f32       sun_intensity = 1.0f;

    // Ambient fill light
    glm::vec3 ambient_color{0.15f, 0.15f, 0.2f};
    f32       ambient_intensity = 0.25f;

    // Fog (matches skybox horizon)
    glm::vec3 fog_color{0.5f, 0.5f, 0.6f};

    // Skybox cubemap face paths (relative to map root). Empty = no skybox.
    std::string skybox_right, skybox_left;
    std::string skybox_top, skybox_bottom;
    std::string skybox_front, skybox_back;

    bool has_skybox() const { return !skybox_right.empty(); }
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

    // Environment (skybox, lighting, fog)
    EnvironmentConfig environment;
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

struct PlacedDoodad {
    std::string type;
    f32 x = 0, y = 0, facing = 0;
    u8  variation = 0;
};

// Authored region — script-addressable zone with one or more rect /
// circle shapes. `id` is the lookup key code uses; the runtime
// allocates a numeric handle on top of it (see World::Region).
struct RegionRect   { f32 x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
struct RegionCircle { f32 cx = 0, cy = 0, r  = 0; };
struct Region {
    std::string              id;
    std::vector<RegionRect>   rects;
    std::vector<RegionCircle> circles;
};

// WC3-style camera setup. Target-based: stores what the camera looks AT
// (target xyz), how far the eye sits along the eye-ray (`distance`),
// and the eye angles (`pitch_deg`, `yaw_deg`). The eye position is
// derived at render time as `target - distance * forward_dir`. Authors
// always read these fields in degrees; the renderer converts to radians
// at the render boundary.
struct CameraSetup {
    std::string id;
    f32 target_x = 0, target_y = 0, target_z = 0;
    f32 distance  = 1650.0f;       // WC3 default eye-to-target distance
    f32 pitch_deg = -56.0f;        // WC3 angle-of-attack 304°
    f32 yaw_deg   = 0.0f;          // 0 = looking +Y
};

struct SceneData {
    TerrainData                     terrain;
    std::vector<PlacedUnit>         units;
    std::vector<PlacedDestructable> destructables;
    std::vector<PlacedItem>         items;
    std::vector<PlacedDoodad>       doodads;
    std::vector<Region>             regions;
    std::vector<CameraSetup>        cameras;
};

class MapManager {
public:
    bool init();
    void shutdown();

    // Load a map. Loads manifest, types, and start scene via AssetManager.
    //
    // `path` is the map root (e.g. "maps/test_map.uldmap"). It is also the
    // virtual asset prefix. The backing storage is chosen by `allow_directory`:
    //   - false (default): `path` must be a packaged `.uldmap` file on disk.
    //     This is what uldum_dev / uldum_game / uldum_server use — they only
    //     consume shipped packages.
    //   - true:            `path` may also be a directory on disk (loose files).
    //     Only uldum_editor uses this, so it can edit the source tree live.
    //
    // Equivalent to `load_manifest_only(...) && load_content(...)`.
    bool load_map(std::string_view path, asset::AssetManager& assets, simulation::Simulation& sim,
                  bool allow_directory = false);

    // Lobby-phase load: mounts the map and parses only `manifest.json`. Fast
    // enough to run on Menu → Lobby transition without a loading screen. Does
    // not populate the simulation or load terrain. Call `load_content` later
    // (on Start / Loading state) to actually bring the map online.
    bool load_manifest_only(std::string_view path, asset::AssetManager& assets,
                            bool allow_directory = false);

    // Loading-phase load: tileset, types, terrain, preplaced objects. Assumes
    // `load_manifest_only` (or `load_map`) has already mounted the map.
    bool load_content(asset::AssetManager& assets, simulation::Simulation& sim);

    void unload_map();
    bool is_loaded() const { return m_loaded; }

    const MapManifest& manifest() const { return m_manifest; }
    const Tileset&     tileset()  const { return m_tileset; }
    const TerrainData& terrain()  const { return m_scene.terrain; }
    TerrainData&       terrain()        { return m_scene.terrain; }
    const SceneData&   scene()    const { return m_scene; }

    // Mutable region list — the editor is the only legitimate caller.
    // m_scene.regions is the canonical authored data; save_objects
    // serializes from this vector and runtime registration in
    // load_placements reads from it.
    std::vector<Region>& mutable_regions() { return m_scene.regions; }

    const std::string& map_root() const { return m_map_root; }

    // List scene directories found under the map root.
    std::vector<std::string> list_scenes() const;

    // Switch to a different scene (reloads terrain + placements).
    bool switch_scene(std::string_view scene_name, asset::AssetManager& assets, simulation::Simulation& sim);

    // Write the live simulation state back to <map_root>/scenes/<scene>/objects.json.
    // Iterates world entities (units, destructables, items) and emits
    // their placement records; regions + cameras pass through from
    // the authored scene data. Requires the map to be a source-folder
    // mount (not a packed .uldmap) — packed maps go through the
    // unpack → modify → repack flow in the editor.
    bool save_objects(const simulation::World& world, std::string_view scene_name) const;

    // Editor helper: temporarily redirect save_objects to a staging
    // directory (used by the packed-.uldmap save flow that unpacks
    // into a temp folder, writes there, then repacks). Caller is
    // responsible for restoring the original root afterward.
    void set_map_root_for_save(std::string root) { m_map_root = std::move(root); }

    // Switch to a different scene's terrain only — does NOT spawn the
    // scene's placement entities. Used by client-side scene switch
    // (host re-spawns entities via S_SPAWN) and by the host's MP
    // path which defers placement instantiation until after the
    // client-load barrier so spawn deltas don't fire before clients
    // have torn down the previous scene.
    bool switch_scene_terrain_only(std::string_view scene_name,
                                   asset::AssetManager& assets,
                                   simulation::Simulation& sim);

    // Public entry to load placements for the current scene. Pairs
    // with switch_scene_terrain_only on the host's MP path.
    bool load_scene_placements(std::string_view scene_name,
                               asset::AssetManager& assets,
                               simulation::Simulation& sim) {
        return load_placements(scene_name, assets, sim);
    }

private:
    bool load_manifest(asset::AssetManager& assets);
    bool load_tileset(asset::AssetManager& assets);
    bool load_types(asset::AssetManager& assets, simulation::Simulation& sim);
    bool load_scene(std::string_view scene_name, asset::AssetManager& assets, simulation::Simulation& sim);
    bool load_scene_terrain(std::string_view scene_name, asset::AssetManager& assets);
    // Pure data — regions + cameras from objects.json. No entity
    // creation, so safe to call in MP scene-switch teardown before
    // the barrier closes.
    bool load_scene_metadata(std::string_view scene_name, asset::AssetManager& assets);
    bool load_placements(std::string_view scene_name, asset::AssetManager& assets, simulation::Simulation& sim);

    bool                  m_loaded = false;
    std::string           m_map_root;
    MapManifest           m_manifest;
    Tileset               m_tileset;
    SceneData             m_scene;
    // Non-owning. Set by load_manifest_only() at the time the map is
    // mounted; used by unload_map() to release the mount so the
    // AssetManager's mount list doesn't accumulate stale entries
    // across sessions.
    asset::AssetManager*  m_assets = nullptr;
};

} // namespace uldum::map
