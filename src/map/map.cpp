#include "map/map.h"
#include "asset/asset.h"
#include "simulation/simulation.h"
#include "simulation/world.h"
#include "core/log.h"

#include <filesystem>

namespace uldum::map {

static constexpr const char* TAG = "Map";

bool MapManager::init() {
    log::info(TAG, "MapManager initialized");
    return true;
}

void MapManager::shutdown() {
    unload_map();
    log::info(TAG, "MapManager shut down");
}

bool MapManager::load_map(std::string_view path, asset::AssetManager& assets, simulation::Simulation& sim) {
    unload_map();
    sim.world().clear_entities();
    m_map_root = std::string(path);

    // Temporarily point the asset manager at the map root for map-relative paths
    // We'll load map files using absolute paths constructed from m_map_root

    if (!load_manifest(assets)) return false;
    if (!load_types(assets, sim)) return false;
    if (!load_scene(m_manifest.start_scene, assets, sim)) return false;

    m_loaded = true;
    log::info(TAG, "Map '{}' loaded — scene '{}'", m_manifest.name, m_manifest.start_scene);
    return true;
}

void MapManager::unload_map() {
    if (m_loaded) {
        log::info(TAG, "Unloading map '{}'", m_manifest.name);
        m_loaded = false;
    }
    m_manifest = {};
    m_scene = {};
    m_map_root.clear();
}

std::vector<std::string> MapManager::list_scenes() const {
    std::vector<std::string> scenes;
    if (m_map_root.empty()) return scenes;

    std::string scenes_dir = m_map_root + "/scenes";
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(scenes_dir, ec)) {
        if (entry.is_directory()) {
            scenes.push_back(entry.path().filename().string());
        }
    }
    std::sort(scenes.begin(), scenes.end());
    return scenes;
}

bool MapManager::switch_scene(std::string_view scene_name, asset::AssetManager& assets, simulation::Simulation& sim) {
    sim.world().clear_entities();
    m_scene = {};
    if (!load_scene(scene_name, assets, sim)) return false;
    log::info(TAG, "Switched to scene '{}'", scene_name);
    return true;
}

// ── Manifest ──────────────────────────────────────────────────────────────

bool MapManager::load_manifest(asset::AssetManager& assets) {
    std::string manifest_path = m_map_root + "/manifest.json";
    auto handle = assets.load_config_absolute(manifest_path);
    auto* doc = assets.get(handle);
    if (!doc) {
        log::error(TAG, "Failed to load manifest from '{}'", manifest_path);
        return false;
    }

    auto& j = doc->data;
    m_manifest.name             = j.value("name", "Unnamed");
    m_manifest.author           = j.value("author", "");
    m_manifest.description      = j.value("description", "");
    m_manifest.version          = j.value("version", "1.0.0");
    m_manifest.engine_version   = j.value("engine_version", "0.1.0");
    m_manifest.game_mode        = j.value("game_mode", "custom");
    m_manifest.suggested_players = j.value("suggested_players", "1");
    m_manifest.tileset_path     = j.value("tileset", "");
    m_manifest.start_scene      = j.value("start_scene", "scene_01");

    if (j.contains("players")) {
        for (auto& p : j["players"]) {
            PlayerSlot slot;
            slot.slot  = p.value("slot", 0u);
            slot.type  = p.value("type", "human");
            slot.team  = p.value("team", 0u);
            slot.name  = p.value("name", "");
            slot.color = p.value("color", "");
            m_manifest.players.push_back(std::move(slot));
        }
    }

    if (j.contains("teams")) {
        for (auto& t : j["teams"]) {
            TeamDef team;
            team.id            = t.value("id", 0u);
            team.name          = t.value("name", "");
            team.allied        = t.value("allied", false);
            team.shared_vision = t.value("shared_vision", false);
            m_manifest.teams.push_back(std::move(team));
        }
    }

    // Map-defined enumerations
    if (j.contains("classifications")) {
        for (auto& c : j["classifications"]) m_manifest.classifications.push_back(c.get<std::string>());
    }
    if (j.contains("attack_types")) {
        for (auto& a : j["attack_types"]) m_manifest.attack_types.push_back(a.get<std::string>());
    }
    if (j.contains("armor_types")) {
        for (auto& a : j["armor_types"]) m_manifest.armor_types.push_back(a.get<std::string>());
    }
    if (j.contains("attributes")) {
        for (auto& a : j["attributes"]) m_manifest.attributes.push_back(a.get<std::string>());
    }

    // Input configuration
    if (j.contains("input") && j["input"].is_object()) {
        auto& input = j["input"];
        m_manifest.input_preset = input.value("preset", "rts");
        if (input.contains("bindings") && input["bindings"].is_object()) {
            m_manifest.input_bindings_json = input["bindings"];
        }
    }

    log::info(TAG, "Manifest: '{}' by {} — {} players, {} teams, scene '{}'",
              m_manifest.name, m_manifest.author,
              m_manifest.players.size(), m_manifest.teams.size(),
              m_manifest.start_scene);
    return true;
}

// ── Types ─────────────────────────────────────────────────────────────────

bool MapManager::load_types(asset::AssetManager& assets, simulation::Simulation& sim) {
    auto& types = sim.types();
    std::string types_dir = m_map_root + "/types/";

    // Load each type file if it exists
    auto try_load = [&](auto load_fn, const char* filename) {
        std::string path = types_dir + filename;
        auto handle = assets.load_config_absolute(path);
        if (assets.get(handle)) {
            (types.*load_fn)(assets, handle);
        }
    };

    // Load using the handle directly — need a different approach since load_*_types
    // takes a path. We'll load via absolute path support.
    std::string unit_path = types_dir + "unit_types.json";
    std::string dest_path = types_dir + "destructable_types.json";
    std::string item_path = types_dir + "item_types.json";

    types.load_unit_types_absolute(assets, unit_path);
    types.load_destructable_types_absolute(assets, dest_path);
    types.load_item_types_absolute(assets, item_path);

    // Load ability definitions
    std::string ability_path = types_dir + "ability_types.json";
    sim.abilities().load(assets, ability_path);  // ok if file doesn't exist

    log::info(TAG, "Types loaded — {} units, {} destructables, {} items",
              types.unit_type_count(), types.destructable_type_count(), types.item_type_count());
    return true;
}

// ── Scene ─────────────────────────────────────────────────────────────────

bool MapManager::load_scene(std::string_view scene_name, asset::AssetManager& assets, simulation::Simulation& sim) {
    std::string scene_dir = m_map_root + "/scenes/" + std::string(scene_name);

    // Load terrain from binary file
    std::string terrain_path = scene_dir + "/terrain.bin";
    m_scene.terrain = load_terrain(terrain_path);
    if (!m_scene.terrain.is_valid()) {
        log::error(TAG, "Scene '{}': missing terrain.bin — every scene must have terrain data", scene_name);
        return false;
    }
    log::info(TAG, "Scene '{}': terrain {}x{} tiles",
              scene_name, m_scene.terrain.tiles_x, m_scene.terrain.tiles_y);

    // Load object placements
    if (!load_placements(scene_name, assets, sim)) {
        log::warn(TAG, "Scene '{}': no placements loaded", scene_name);
    }

    return true;
}

bool MapManager::load_placements(std::string_view scene_name, asset::AssetManager& assets, simulation::Simulation& sim) {
    std::string objects_path = m_map_root + "/scenes/" + std::string(scene_name) + "/objects.json";
    auto handle = assets.load_config_absolute(objects_path);
    auto* doc = assets.get(handle);
    if (!doc) {
        log::warn(TAG, "No objects.json in scene '{}'", scene_name);
        return false;
    }

    auto& j = doc->data;
    auto& world = sim.world();

    // Helper: sample terrain height (bilinear, using world_z_at)
    auto sample_height = [&](f32 x, f32 y) -> f32 {
        auto& td = m_scene.terrain;
        if (!td.is_valid()) return 0.0f;
        u32 ix = std::min(static_cast<u32>(x / td.tile_size), td.tiles_x);
        u32 iy = std::min(static_cast<u32>(y / td.tile_size), td.tiles_y);
        return td.world_z_at(ix, iy);
    };

    // Units — first pass: create all units
    std::vector<simulation::Unit> created_units;
    u32 unit_count = 0;
    if (j.contains("units")) {
        for (auto& u : j["units"]) {
            PlacedUnit pu;
            pu.type   = u.value("type", "");
            pu.x      = u.value("x", 0.0f);
            pu.y      = u.value("y", 0.0f);
            pu.facing = u.value("facing", 0.0f) * (glm::pi<f32>() / 180.0f);
            pu.owner  = u.value("owner", 0u);
            m_scene.units.push_back(pu);

            simulation::Player owner{pu.owner};
            auto unit = simulation::create_unit(world, pu.type, owner, pu.x, pu.y, pu.facing);
            created_units.push_back(unit);
            if (unit.is_valid()) {
                auto* t = world.transforms.get(unit.id);
                if (t) t->position.z = sample_height(pu.x, pu.y);
                // Set initial cliff level from nearest vertex
                auto* mov = world.movements.get(unit.id);
                if (mov) {
                    u32 vx = std::min(static_cast<u32>(std::round(pu.x / m_scene.terrain.tile_size)), m_scene.terrain.tiles_x);
                    u32 vy = std::min(static_cast<u32>(std::round(pu.y / m_scene.terrain.tile_size)), m_scene.terrain.tiles_y);
                    mov->cliff_level = m_scene.terrain.cliff_at(vx, vy);
                }

                // Buildings block pathing at runtime via PathingBlocker component
                auto* cls = world.classifications.get(unit.id);
                if (cls && simulation::has_classification(cls->flags, "structure")) {
                    auto& td = m_scene.terrain;
                    if (td.is_valid()) {
                        i32 cx = static_cast<i32>(pu.x / td.tile_size);
                        i32 cy = static_cast<i32>(pu.y / td.tile_size);
                        simulation::PathingBlocker blocker;
                        for (i32 dy = -1; dy <= 2; ++dy) {
                            for (i32 dx = -1; dx <= 2; ++dx) {
                                blocker.blocked_vertices.push_back({cx + dx, cy + dy});
                            }
                        }
                        world.pathing_blockers.add(unit.id, std::move(blocker));
                    }
                }

                unit_count++;
            }
        }

        // Orders and abilities are now issued by map Lua scripts (main.lua)
    }

    // Destructables
    u32 dest_count = 0;
    if (j.contains("destructables")) {
        for (auto& d : j["destructables"]) {
            PlacedDestructable pd;
            pd.type      = d.value("type", "");
            pd.x         = d.value("x", 0.0f);
            pd.y         = d.value("y", 0.0f);
            pd.facing    = d.value("facing", 0.0f) * (glm::pi<f32>() / 180.0f);
            pd.variation = static_cast<u8>(d.value("variation", 0));
            m_scene.destructables.push_back(pd);

            auto dest = simulation::create_destructable(world, pd.type, pd.x, pd.y, pd.facing, pd.variation);
            if (dest.is_valid()) dest_count++;
        }
    }

    // Items
    u32 item_count = 0;
    if (j.contains("items")) {
        for (auto& i : j["items"]) {
            PlacedItem pi;
            pi.type = i.value("type", "");
            pi.x    = i.value("x", 0.0f);
            pi.y    = i.value("y", 0.0f);
            m_scene.items.push_back(pi);

            auto item = simulation::create_item(world, pi.type, pi.x, pi.y);
            if (item.is_valid()) item_count++;
        }
    }

    // Regions
    if (j.contains("regions")) {
        for (auto& r : j["regions"]) {
            Region reg;
            reg.name   = r.value("name", "");
            reg.x      = r.value("x", 0.0f);
            reg.y      = r.value("y", 0.0f);
            reg.width  = r.value("width", 0.0f);
            reg.height = r.value("height", 0.0f);
            m_scene.regions.push_back(std::move(reg));
        }
    }

    // Cameras
    if (j.contains("cameras")) {
        for (auto& c : j["cameras"]) {
            CameraDef cam;
            cam.name  = c.value("name", "");
            cam.x     = c.value("x", 0.0f);
            cam.y     = c.value("y", 0.0f);
            cam.z     = c.value("z", 0.0f);
            cam.pitch = c.value("pitch", 0.0f);
            cam.yaw   = c.value("yaw", 0.0f);
            m_scene.cameras.push_back(std::move(cam));
        }
    }

    log::info(TAG, "Placements: {} units, {} destructables, {} items, {} regions, {} cameras",
              unit_count, dest_count, item_count, m_scene.regions.size(), m_scene.cameras.size());
    return true;
}

} // namespace uldum::map
