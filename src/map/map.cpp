#include "map/map.h"
#include "asset/asset.h"
#include "simulation/simulation.h"
#include "simulation/world.h"
#include "simulation/handle_types.h"
#include "simulation/pathfinding.h"  // PATHING_SUBDIV
#include "core/log.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <unordered_set>
#include <glm/gtc/constants.hpp>

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
    std::string dood_path = types_dir + "doodad_types.json";

    types.load_unit_types_absolute(assets, unit_path);
    types.load_destructable_types_absolute(assets, dest_path);
    types.load_item_types_absolute(assets, item_path);
    types.load_doodad_types_absolute(assets, dood_path);  // ok if file doesn't exist

    // Load ability definitions
    std::string ability_path = types_dir + "ability_types.json";
    sim.abilities().load(assets, ability_path);  // ok if file doesn't exist

    log::info(TAG, "Types loaded — {} units, {} destructables, {} doodads, {} items",
              types.unit_type_count(), types.destructable_type_count(),
              types.doodad_type_count(), types.item_type_count());
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
        std::unordered_set<std::string> seen_ids;
        for (auto& r : j["regions"]) {
            Region reg;
            reg.id = r.value("id", "");
            if (reg.id.empty()) {
                log::warn(TAG, "Scene '{}': region with empty id, skipping", scene_name);
                continue;
            }
            if (!seen_ids.insert(reg.id).second) {
                log::warn(TAG, "Scene '{}': duplicate region id '{}', skipping", scene_name, reg.id);
                continue;
            }
            if (r.contains("rects")) {
                for (auto& rj : r["rects"]) {
                    RegionRect rect{
                        rj.value("x0", 0.0f), rj.value("y0", 0.0f),
                        rj.value("x1", 0.0f), rj.value("y1", 0.0f),
                    };
                    if (rect.x1 < rect.x0) std::swap(rect.x0, rect.x1);
                    if (rect.y1 < rect.y0) std::swap(rect.y0, rect.y1);
                    reg.rects.push_back(rect);
                }
            }
            if (r.contains("circles")) {
                for (auto& cj : r["circles"]) {
                    RegionCircle circle{
                        cj.value("cx", 0.0f), cj.value("cy", 0.0f),
                        std::max(0.0f, cj.value("r", 0.0f)),
                    };
                    reg.circles.push_back(circle);
                }
            }
            m_scene.regions.push_back(std::move(reg));
        }
    }

    if (j.contains("cameras")) {
        std::unordered_set<std::string> seen_ids;
        for (auto& c : j["cameras"]) {
            CameraDef cam;
            cam.id    = c.value("id", "");
            if (cam.id.empty()) {
                log::warn(TAG, "Scene '{}': camera with empty id, skipping", scene_name);
                continue;
            }
            if (!seen_ids.insert(cam.id).second) {
                log::warn(TAG, "Scene '{}': duplicate camera id '{}', skipping", scene_name, cam.id);
                continue;
            }
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
                // the type's footprint. Buildings author in TILES; we
                // expand to cells (×PATHING_SUBDIV) so the stored rect
                // matches the pathfinder's native cell grid.
                if (fw > 0 && fh > 0 && m_scene.terrain.is_valid()) {
                    auto& td = m_scene.terrain;
                    f32 left_tx_f   = (pu.x - td.origin_x()) / td.tile_size - 0.5f * static_cast<f32>(fw);
                    f32 bottom_ty_f = (pu.y - td.origin_y()) / td.tile_size - 0.5f * static_cast<f32>(fh);
                    i32 tx0 = static_cast<i32>(std::round(left_tx_f));
                    i32 ty0 = static_cast<i32>(std::round(bottom_ty_f));
                    simulation::PathingBlocker blocker;
                    blocker.cx = tx0 * static_cast<i32>(simulation::PATHING_SUBDIV);
                    blocker.cy = ty0 * static_cast<i32>(simulation::PATHING_SUBDIV);
                    blocker.w  = fw * simulation::PATHING_SUBDIV;
                    blocker.h  = fh * simulation::PATHING_SUBDIV;
                    log::info(TAG,
                        "Placed building '{}' at ({:.0f},{:.0f}) "
                        "[footprint {}x{} tiles, blocking cells x={}..{} y={}..{}]",
                        pu.type, pu.x, pu.y, fw, fh,
                        blocker.cx, blocker.cx + static_cast<i32>(blocker.w) - 1,
                        blocker.cy, blocker.cy + static_cast<i32>(blocker.h) - 1);
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

            const auto* def = sim.types().get_destructable_type(pd.type);
            u32 fw = def ? def->pathing_footprint_w : 0u;
            u32 fh = def ? def->pathing_footprint_h : 0u;
            // Cell-snap (1/4 tile) at load time so saved scene-files
            // round-trip cleanly through the editor's same snap rule.
            if (m_scene.terrain.is_valid()) {
                pd.x = snap_cell_x(m_scene.terrain, pd.x);
                pd.y = snap_cell_y(m_scene.terrain, pd.y);
            }
            m_scene.destructables.push_back(pd);

            auto dest = simulation::create_destructable(world, pd.type, pd.x, pd.y, pd.facing, pd.variation);
            if (dest.is_valid()) {
                if (auto* t = world.transforms.get(dest.id)) {
                    t->position.z = sample_height(pd.x, pd.y);
                    t->prev_position.z = t->position.z;
                }
                // Destructable footprints are CELL units. Center the rect
                // on the snapped position (cell snap means pd.x/pd.y are
                // already on a cell-center grid).
                if (fw > 0 && fh > 0 && m_scene.terrain.is_valid()) {
                    auto& td = m_scene.terrain;
                    f32 cs = td.tile_size / static_cast<f32>(simulation::PATHING_SUBDIV);
                    f32 left_cx_f   = (pd.x - td.origin_x()) / cs - 0.5f * static_cast<f32>(fw);
                    f32 bottom_cy_f = (pd.y - td.origin_y()) / cs - 0.5f * static_cast<f32>(fh);
                    simulation::PathingBlocker blocker;
                    blocker.cx = static_cast<i32>(std::round(left_cx_f));
                    blocker.cy = static_cast<i32>(std::round(bottom_cy_f));
                    blocker.w  = fw;
                    blocker.h  = fh;
                    world.pathing_blockers.add(dest.id, std::move(blocker));
                }
                dest_count++;
            }
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

    // Doodads (pure decoration — no health, no collision, no pathing)
    u32 dood_count = 0;
    if (j.contains("doodads")) {
        for (auto& d : j["doodads"]) {
            PlacedDoodad pd;
            pd.type      = d.value("type", "");
            pd.x         = d.value("x", 0.0f);
            pd.y         = d.value("y", 0.0f);
            pd.facing    = d.value("facing", 0.0f) * (glm::pi<f32>() / 180.0f);
            pd.variation = static_cast<u8>(d.value("variation", 0));
            m_scene.doodads.push_back(pd);

            auto dood = simulation::create_doodad(world, pd.type, pd.x, pd.y, pd.facing, pd.variation);
            if (dood.is_valid()) {
                if (auto* t = world.transforms.get(dood.id)) {
                    t->position.z = sample_height(pd.x, pd.y);
                    t->prev_position.z = t->position.z;
                }
                dood_count++;
            }
        }
    }

    // Register authored regions into the runtime world so scripts
    // can pick them up by id via GetRegion(). Lua-created regions
    // (CreateRegion()) continue to live in the same world.regions
    // map; the difference is just whether `id_str` is set.
    for (const auto& r : m_scene.regions) {
        u32 rid = ++world.next_region_id;
        simulation::World::Region wr;
        wr.id     = rid;
        wr.id_str = r.id;
        for (const auto& rect : r.rects) {
            wr.rects.push_back({rect.x0, rect.y0, rect.x1, rect.y1});
        }
        for (const auto& c : r.circles) {
            wr.circles.push_back({c.cx, c.cy, c.r});
        }
        world.regions[rid] = std::move(wr);
    }

    log::info(TAG, "Placements: {} units, {} destructables, {} doodads, {} items, {} regions",
              unit_count, dest_count, dood_count, item_count, m_scene.regions.size());
    return true;
}

// Save the scene's placement state back to objects.json. Iterates
// the live simulation world (single source of truth) for entities;
// regions and cameras pass through from the authored scene data
// since they aren't represented as entities yet. Requires the map
// to be mounted as a source folder.
bool MapManager::save_objects(const simulation::World& world, std::string_view scene_name) const {
    namespace fs = std::filesystem;
    if (!fs::is_directory(m_map_root)) {
        log::error(TAG, "save_objects: map_root '{}' is not a directory; packed-map save path "
                        "goes through the unpack → modify → repack flow", m_map_root);
        return false;
    }

    nlohmann::json doc;
    doc["units"]         = nlohmann::json::array();
    doc["destructables"] = nlohmann::json::array();
    doc["doodads"]       = nlohmann::json::array();
    doc["items"]         = nlohmann::json::array();

    const f32 rad_to_deg = 180.0f / glm::pi<f32>();

    for (u32 i = 0; i < world.handle_infos.count(); ++i) {
        u32 id = world.handle_infos.ids()[i];
        const auto& info = world.handle_infos.data()[i];
        const auto* t = world.transforms.get(id);
        if (!t) continue;

        nlohmann::json e;
        e["type"] = info.type_id;
        e["x"]    = t->position.x;
        e["y"]    = t->position.y;

        switch (info.category) {
        case simulation::Category::Unit: {
            const auto* owner = world.owners.get(id);
            e["facing"] = t->facing * rad_to_deg;
            e["owner"]  = owner ? owner->player.id : 0u;
            doc["units"].push_back(std::move(e));
            break;
        }
        case simulation::Category::Destructable: {
            const auto* dc = world.destructables.get(id);
            e["facing"]    = t->facing * rad_to_deg;
            e["variation"] = dc ? dc->variation : 0u;
            doc["destructables"].push_back(std::move(e));
            break;
        }
        case simulation::Category::Item:
            doc["items"].push_back(std::move(e));
            break;
        case simulation::Category::Doodad: {
            const auto* dc = world.doodads.get(id);
            e["facing"]    = t->facing * rad_to_deg;
            e["variation"] = dc ? dc->variation : 0u;
            doc["doodads"].push_back(std::move(e));
            break;
        }
        default:
            break;
        }
    }

    // Regions / cameras: pass through from authored scene data —
    // they aren't entities so they don't appear in the world loop.
    doc["regions"] = nlohmann::json::array();
    for (const auto& r : m_scene.regions) {
        nlohmann::json j;
        j["id"]      = r.id;
        j["rects"]   = nlohmann::json::array();
        for (const auto& rc : r.rects) {
            j["rects"].push_back({
                {"x0", rc.x0}, {"y0", rc.y0},
                {"x1", rc.x1}, {"y1", rc.y1},
            });
        }
        j["circles"] = nlohmann::json::array();
        for (const auto& c : r.circles) {
            j["circles"].push_back({
                {"cx", c.cx}, {"cy", c.cy}, {"r", c.r},
            });
        }
        doc["regions"].push_back(std::move(j));
    }
    doc["cameras"] = nlohmann::json::array();
    for (const auto& c : m_scene.cameras) {
        nlohmann::json j;
        j["id"]    = c.id;
        j["x"]     = c.x;
        j["y"]     = c.y;
        j["z"]     = c.z;
        j["pitch"] = c.pitch;
        j["yaw"]   = c.yaw;
        doc["cameras"].push_back(std::move(j));
    }

    std::string path = m_map_root + "/scenes/" + std::string(scene_name) + "/objects.json";
    std::ofstream file(path);
    if (!file) {
        log::error(TAG, "save_objects: failed to open '{}' for writing", path);
        return false;
    }
    file << doc.dump(4);
    log::info(TAG, "save_objects: wrote {} units / {} destructables / {} doodads / {} items to {}",
              doc["units"].size(), doc["destructables"].size(),
              doc["doodads"].size(), doc["items"].size(), path);
    return file.good();
}

} // namespace uldum::map
