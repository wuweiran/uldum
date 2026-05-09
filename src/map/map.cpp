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

bool MapManager::load_manifest_only(std::string_view path, asset::AssetManager& assets,
                                    bool allow_directory) {
    unload_map();
    m_map_root = std::string(path);
    m_assets   = &assets;

    bool is_dir = std::filesystem::is_directory(std::filesystem::path(m_map_root));
    if (is_dir) {
        if (!allow_directory) {
            log::error(TAG, "Map '{}' is a directory, but this target only accepts packaged .uldmap files", m_map_root);
            return false;
        }
        assets.mount_directory(m_map_root, m_map_root);
    } else {
        if (!assets.open_package(m_map_root, m_map_root)) {
            log::error(TAG, "Failed to open map package '{}'", m_map_root);
            return false;
        }
    }

    if (!load_manifest(assets)) return false;
    log::info(TAG, "Map '{}' manifest loaded (lobby-phase)", m_manifest.name);
    return true;
}

bool MapManager::load_content(asset::AssetManager& assets, simulation::Simulation& sim) {
    if (m_map_root.empty()) {
        log::error(TAG, "load_content called before load_manifest_only");
        return false;
    }
    sim.world().clear_entities();
    if (!load_tileset(assets)) return false;
    if (!load_types(assets, sim)) return false;
    if (!load_scene(m_manifest.start_scene, assets, sim)) return false;

    m_loaded = true;
    log::info(TAG, "Map '{}' content loaded — scene '{}'", m_manifest.name, m_manifest.start_scene);
    return true;
}

bool MapManager::load_map(std::string_view path, asset::AssetManager& assets, simulation::Simulation& sim,
                          bool allow_directory) {
    return load_manifest_only(path, assets, allow_directory)
        && load_content(assets, sim);
}

void MapManager::unload_map() {
    if (m_loaded) {
        log::info(TAG, "Unloading map '{}'", m_manifest.name);
        m_loaded = false;
    }
    // Release the package / directory mount we installed for this map
    // so the AssetManager's mount chain doesn't accumulate one stale
    // entry per session played.
    if (m_assets && !m_map_root.empty()) {
        m_assets->unmount(m_map_root);
    }
    m_manifest = {};
    m_scene = {};
    m_map_root.clear();
    m_assets = nullptr;
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

bool MapManager::switch_scene_terrain_only(std::string_view scene_name,
                                           asset::AssetManager& assets,
                                           simulation::Simulation& sim) {
    sim.world().clear_entities();
    m_scene = {};
    if (!load_scene_terrain(scene_name, assets)) return false;
    // Pure-data scene metadata (regions, cameras) belongs to the new
    // scene's terrain swap — App reads m_scene.cameras to re-pose the
    // camera right after this returns, and Lua scripts read regions.
    // Doesn't touch the world, so safe before the MP barrier closes.
    if (!load_scene_metadata(scene_name, assets)) {
        log::warn(TAG, "Scene '{}': no metadata loaded", scene_name);
    }
    log::info(TAG, "Switched to scene '{}' (terrain only)", scene_name);
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
    m_manifest.id               = j.value("id", "");
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
            slot.team  = p.value("team", 0u);
            slot.color = p.value("color", "");
            slot.type  = p.value("type", "");
            slot.name  = p.value("name", "");
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

    // Fog of war
    m_manifest.fog_of_war = j.value("fog_of_war", "none");

    // Reconnect settings
    if (j.contains("reconnect") && j["reconnect"].is_object()) {
        auto& rc = j["reconnect"];
        m_manifest.disconnect_timeout = rc.value("timeout", 60.0f);
        m_manifest.pause_on_disconnect = rc.value("pause", false);
    }

    // Input configuration
    if (j.contains("input") && j["input"].is_object()) {
        auto& input = j["input"];
        m_manifest.input_preset = input.value("preset", "rts");
        if (input.contains("bindings") && input["bindings"].is_object()) {
            m_manifest.input_bindings_json = input["bindings"];
        }
    }

    // Environment (skybox, lighting, fog)
    if (j.contains("environment") && j["environment"].is_object()) {
        auto& env = j["environment"];
        auto& ec = m_manifest.environment;

        auto read_vec3 = [](const nlohmann::json& arr, glm::vec3& out) {
            if (arr.is_array() && arr.size() >= 3)
                out = {arr[0].get<f32>(), arr[1].get<f32>(), arr[2].get<f32>()};
        };

        if (env.contains("sun_direction")) read_vec3(env["sun_direction"], ec.sun_direction);
        if (env.contains("sun_color"))     read_vec3(env["sun_color"], ec.sun_color);
        ec.sun_intensity     = env.value("sun_intensity", ec.sun_intensity);
        if (env.contains("ambient_color")) read_vec3(env["ambient_color"], ec.ambient_color);
        ec.ambient_intensity = env.value("ambient_intensity", ec.ambient_intensity);
        if (env.contains("fog_color"))     read_vec3(env["fog_color"], ec.fog_color);

        if (env.contains("skybox") && env["skybox"].is_object()) {
            auto& sky = env["skybox"];
            ec.skybox_right  = sky.value("right", "");
            ec.skybox_left   = sky.value("left", "");
            ec.skybox_top    = sky.value("top", "");
            ec.skybox_bottom = sky.value("bottom", "");
            ec.skybox_front  = sky.value("front", "");
            ec.skybox_back   = sky.value("back", "");
        }
    }

    log::info(TAG, "Manifest: '{}' by {} — {} players, {} teams, scene '{}'",
              m_manifest.name, m_manifest.author,
              m_manifest.players.size(), m_manifest.teams.size(),
              m_manifest.start_scene);
    return true;
}

// ── Tileset ──────────────────────────────────────────────────────────────

bool MapManager::load_tileset(asset::AssetManager& assets) {
    if (m_manifest.tileset_path.empty()) {
        log::info(TAG, "No tileset specified — using defaults");
        return true;
    }

    std::string tileset_path = m_map_root + "/" + m_manifest.tileset_path;
    auto handle = assets.load_config_absolute(tileset_path);
    auto* doc = assets.get(handle);
    if (!doc) {
        log::warn(TAG, "Failed to load tileset from '{}' — using defaults", tileset_path);
        return true;  // non-fatal
    }

    auto& j = doc->data;
    m_tileset.name = j.value("name", "Default");

    if (j.contains("layers")) {
        for (auto& lj : j["layers"]) {
            TilesetLayer layer;
            layer.id           = lj.value("id", 0u);
            layer.name         = lj.value("name", "");
            layer.diffuse_path = lj.value("diffuse", "");
            layer.normal_path  = lj.value("normal", "");
            std::string type_str = lj.value("type", "");
            if (type_str == "water_shallow")   layer.type = LayerType::WaterShallow;
            else if (type_str == "water_deep") layer.type = LayerType::WaterDeep;
            else if (type_str == "grass")      layer.type = LayerType::Grass;
            else                               layer.type = LayerType::Ground;

            if (lj.contains("color") && lj["color"].is_array() && lj["color"].size() >= 3) {
                layer.water_color = {lj["color"][0].get<f32>(),
                                     lj["color"][1].get<f32>(),
                                     lj["color"][2].get<f32>()};
            }
            // wave_speed removed — unified across all water types

            m_tileset.layers.push_back(std::move(layer));
        }
    }

    log::info(TAG, "Tileset '{}' loaded — {} layers", m_tileset.name, m_tileset.layers.size());
    return true;
}

// ── Types ─────────────────────────────────────────────────────────────────

bool MapManager::load_types(asset::AssetManager& assets, simulation::Simulation& sim) {
    auto& types = sim.types();
    std::string types_dir = m_map_root + "/types/";

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

bool MapManager::load_scene_terrain(std::string_view scene_name, asset::AssetManager& assets) {
    std::string scene_dir = m_map_root + "/scenes/" + std::string(scene_name);

    std::string terrain_path = scene_dir + "/terrain.bin";
    {
        auto bytes = assets.read_file_bytes(terrain_path);
        if (!bytes.empty()) {
            m_scene.terrain = load_terrain_from_memory(bytes.data(), static_cast<u32>(bytes.size()));
        }
    }
    if (!m_scene.terrain.is_valid()) {
        log::error(TAG, "Scene '{}': missing terrain.bin — every scene must have terrain data", scene_name);
        return false;
    }
    log::info(TAG, "Scene '{}': terrain {}x{} tiles",
              scene_name, m_scene.terrain.tiles_x, m_scene.terrain.tiles_y);
    return true;
}

bool MapManager::load_scene(std::string_view scene_name, asset::AssetManager& assets, simulation::Simulation& sim) {
    if (!load_scene_terrain(scene_name, assets)) return false;
    if (!load_scene_metadata(scene_name, assets)) {
        log::warn(TAG, "Scene '{}': no metadata loaded", scene_name);
    }
    if (!load_placements(scene_name, assets, sim)) {
        log::warn(TAG, "Scene '{}': no placements loaded", scene_name);
    }

    return true;
}

bool MapManager::load_scene_metadata(std::string_view scene_name, asset::AssetManager& assets) {
    std::string objects_path = m_map_root + "/scenes/" + std::string(scene_name) + "/objects.json";
    auto handle = assets.load_config_absolute(objects_path);
    auto* doc = assets.get(handle);
    if (!doc) return false;

    auto& j = doc->data;

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
        u32 ix = std::min(static_cast<u32>((x - td.origin_x()) / td.tile_size), td.tiles_x);
        u32 iy = std::min(static_cast<u32>((y - td.origin_y()) / td.tile_size), td.tiles_y);
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

            // If the unit type declares a pathing footprint, snap the
            // authored XY to the building-placement grid before
            // creating the entity. WC3 rule: odd footprint extent
            // snaps to a tile center, even snaps to a tile corner —
            // this guarantees the footprint vertices line up with the
            // tile grid exactly, so adjacent units have a clean
            // walkable corridor. Authored coords that already line up
            // are no-ops; mis-aligned ones are corrected silently.
            const auto* type_def = sim.types().get_unit_type(pu.type);
            u32 fw = type_def ? type_def->pathing_footprint_w : 0u;
            u32 fh = type_def ? type_def->pathing_footprint_h : 0u;
            if (fw > 0 && fh > 0 && m_scene.terrain.is_valid()) {
                pu.x = snap_building_x(m_scene.terrain, pu.x, fw);
                pu.y = snap_building_y(m_scene.terrain, pu.y, fh);
            }
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
                    auto& td = m_scene.terrain;
                    u32 vx = std::min(static_cast<u32>(std::round((pu.x - td.origin_x()) / td.tile_size)), td.tiles_x);
                    u32 vy = std::min(static_cast<u32>(std::round((pu.y - td.origin_y()) / td.tile_size)), td.tiles_y);
                    mov->cliff_level = td.cliff_at(vx, vy);
                }

                // Generate the runtime PathingBlocker rectangle from
                // the type's footprint. Snapped position guarantees
                // the SW corner tile lands on an integer index.
                if (fw > 0 && fh > 0 && m_scene.terrain.is_valid()) {
                    auto& td = m_scene.terrain;
                    f32 left_tx_f   = (pu.x - td.origin_x()) / td.tile_size - 0.5f * static_cast<f32>(fw);
                    f32 bottom_ty_f = (pu.y - td.origin_y()) / td.tile_size - 0.5f * static_cast<f32>(fh);
                    simulation::PathingBlocker blocker;
                    blocker.tx = static_cast<i32>(std::round(left_tx_f));
                    blocker.ty = static_cast<i32>(std::round(bottom_ty_f));
                    blocker.w  = fw;
                    blocker.h  = fh;
                    log::info(TAG,
                        "Placed building '{}' at ({:.0f},{:.0f}) "
                        "[footprint {}x{}, blocking tiles x={}..{} y={}..{}]",
                        pu.type, pu.x, pu.y, fw, fh,
                        blocker.tx, blocker.tx + static_cast<i32>(fw) - 1,
                        blocker.ty, blocker.ty + static_cast<i32>(fh) - 1);
                    world.pathing_blockers.add(unit.id, std::move(blocker));
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
            if (item.is_valid()) {
                if (auto* t = world.transforms.get(item.id)) {
                    t->position.z      = sample_height(pi.x, pi.y);
                    t->prev_position.z = t->position.z;
                }
                item_count++;
            }
        }
    }

    log::info(TAG, "Placements: {} units, {} destructables, {} items",
              unit_count, dest_count, item_count);
    return true;
}

} // namespace uldum::map
