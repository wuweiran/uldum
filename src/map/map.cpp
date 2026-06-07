#include "map/map.h"
#include "map/placements_bin.h"
#include "asset/asset.h"
#include "simulation/simulation.h"
#include "simulation/world.h"
#include "simulation/handle_types.h"
#include "simulation/pathfinding.h"  // PATHING_SUBDIV
#include "core/hash.h"
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

std::array<u8, 32> MapManager::compute_script_hash(asset::AssetManager& assets) const {
    if (m_map_root.empty()) return {};
    auto paths = assets.list_files(m_map_root, ".lua");
    Sha256 h;
    for (const auto& p : paths) {
        // Hash path + NUL + contents so reordering or renaming changes
        // the digest. list_files returns sorted, so the iteration order
        // is deterministic across machines.
        h.update(p);
        u8 sep = 0;
        h.update({&sep, 1});
        auto bytes = assets.read_file_bytes(p);
        h.update({bytes.data(), bytes.size()});
    }
    return h.finalize();
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
    std::string unit_path = types_dir + "units.json";
    std::string dest_path = types_dir + "destructables.json";
    std::string item_path = types_dir + "items.json";
    std::string dood_path = types_dir + "doodads.json";

    types.load_unit_types_absolute(assets, unit_path);
    types.load_destructable_types_absolute(assets, dest_path);
    types.load_item_types_absolute(assets, item_path);
    types.load_doodad_types_absolute(assets, dood_path);  // ok if file doesn't exist

    // Load ability definitions
    std::string ability_path = types_dir + "abilities.json";
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
    // Water-layer IDs come from the (map-wide) tileset, not from
    // terrain.bin — re-apply them every time a scene's terrain loads
    // so a scene switch can't leave the new terrain with empty water
    // layers. (Without this, is_deep_water() falls back to false for
    // every vertex and ground units happily walk on sea after a switch.)
    {
        std::vector<u8> shallow, deep;
        m_tileset.get_water_layer_ids(shallow, deep);
        m_scene.terrain.set_water_layers(shallow, deep);
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
    load_scene_config(scene_name, assets);
    if (!load_placements(scene_name, assets, sim)) {
        log::warn(TAG, "Scene '{}': no placements loaded", scene_name);
    }

    return true;
}

void MapManager::load_scene_config(std::string_view scene_name, asset::AssetManager& assets) {
    std::string scene_dir = m_map_root + "/scenes/" + std::string(scene_name);
    std::string path = scene_dir + "/scene.json";
    auto handle = assets.load_config_absolute(path);
    auto* doc = assets.get(handle);
    if (!doc) {
        log::info(TAG, "Scene '{}': no scene.json (camera bounds unset)", scene_name);
        return;
    }
    auto& j = doc->data;
    if (j.contains("camera_bounds")) {
        const auto& cb = j["camera_bounds"];
        CameraBounds b;
        b.min_x = cb.value("min_x", 0.0f);
        b.min_y = cb.value("min_y", 0.0f);
        b.max_x = cb.value("max_x", 0.0f);
        b.max_y = cb.value("max_y", 0.0f);
        if (b.max_x < b.min_x) std::swap(b.min_x, b.max_x);
        if (b.max_y < b.min_y) std::swap(b.min_y, b.max_y);
        m_scene.camera_bounds = b;
        log::info(TAG, "Scene '{}': camera bounds [{}, {}] .. [{}, {}]",
                  scene_name, b.min_x, b.min_y, b.max_x, b.max_y);
    }
}

// Load the scene's authored placement data into `m_scene`. Tries the
// new binary format first; falls back to the legacy `objects.json` for
// not-yet-migrated source maps and, in that case, writes a freshly-
// serialized `placements.bin` next to it and deletes the json so the
// next load uses the binary path. Does NOT spawn simulation entities —
// `load_placements` does that as a second pass.
bool MapManager::load_scene_metadata(std::string_view scene_name, asset::AssetManager& assets) {
    namespace fs = std::filesystem;
    std::string scene_dir = m_map_root + "/scenes/" + std::string(scene_name);
    std::string bin_path  = scene_dir + "/placements.bin";
    std::string json_path = scene_dir + "/objects.json";

    // Binary path — preferred. read_file_bytes handles both source
    // directories and packed .uldmap archives transparently.
    {
        auto bytes = assets.read_file_bytes(bin_path);
        if (!bytes.empty()) {
            SceneData parsed{};
            if (!read_placements({bytes.data(), bytes.size()}, parsed)) {
                log::error(TAG, "Scene '{}': placements.bin parse failed", scene_name);
                return false;
            }
            m_scene.units         = std::move(parsed.units);
            m_scene.destructables = std::move(parsed.destructables);
            m_scene.items         = std::move(parsed.items);
            m_scene.doodads       = std::move(parsed.doodads);
            m_scene.regions       = std::move(parsed.regions);
            m_scene.cameras       = std::move(parsed.cameras);
            return true;
        }
    }

    // JSON fallback for not-yet-migrated maps. Existing parse logic
    // populates m_scene; if the map is a source directory we then
    // write the binary form and remove the json (one-shot migration).
    auto handle = assets.load_config_absolute(json_path);
    auto* doc = assets.get(handle);
    if (!doc) return false;

    auto& j = doc->data;
    const f32 deg_to_rad = glm::pi<f32>() / 180.0f;

    if (j.contains("units")) {
        for (auto& u : j["units"]) {
            PlacedUnit pu;
            pu.type   = u.value("type", "");
            pu.x      = u.value("x", 0.0f);
            pu.y      = u.value("y", 0.0f);
            pu.facing = u.value("facing", 0.0f) * deg_to_rad;
            pu.owner  = u.value("owner", 0u);
            m_scene.units.push_back(std::move(pu));
        }
    }
    if (j.contains("destructables")) {
        for (auto& d : j["destructables"]) {
            PlacedDestructable pd;
            pd.type      = d.value("type", "");
            pd.x         = d.value("x", 0.0f);
            pd.y         = d.value("y", 0.0f);
            pd.facing    = d.value("facing", 0.0f) * deg_to_rad;
            pd.variation = static_cast<u8>(d.value("variation", 0));
            m_scene.destructables.push_back(std::move(pd));
        }
    }
    if (j.contains("items")) {
        for (auto& it : j["items"]) {
            PlacedItem pi;
            pi.type = it.value("type", "");
            pi.x    = it.value("x", 0.0f);
            pi.y    = it.value("y", 0.0f);
            m_scene.items.push_back(std::move(pi));
        }
    }
    if (j.contains("doodads")) {
        for (auto& d : j["doodads"]) {
            PlacedDoodad pd;
            pd.type      = d.value("type", "");
            pd.x         = d.value("x", 0.0f);
            pd.y         = d.value("y", 0.0f);
            pd.facing    = d.value("facing", 0.0f) * deg_to_rad;
            pd.variation = static_cast<u8>(d.value("variation", 0));
            m_scene.doodads.push_back(std::move(pd));
        }
    }
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
            CameraSetup cam;
            cam.id    = c.value("id", "");
            if (cam.id.empty()) {
                log::warn(TAG, "Scene '{}': camera with empty id, skipping", scene_name);
                continue;
            }
            if (!seen_ids.insert(cam.id).second) {
                log::warn(TAG, "Scene '{}': duplicate camera id '{}', skipping", scene_name, cam.id);
                continue;
            }
            cam.target_x  = c.value("target_x", 0.0f);
            cam.target_y  = c.value("target_y", 0.0f);
            cam.target_z  = c.value("target_z", 0.0f);
            cam.distance  = c.value("distance",  1650.0f);
            cam.pitch_deg = c.value("pitch",     -56.0f);
            cam.yaw_deg   = c.value("yaw",         0.0f);
            m_scene.cameras.push_back(std::move(cam));
        }
    }

    // One-shot migration: only when the map is a source directory (we
    // can't write into a packed .uldmap). Persist the binary form and
    // delete the json so subsequent loads take the binary path.
    // write_placements handles the in-memory-radians → on-disk-degrees
    // conversion internally.
    if (fs::is_directory(m_map_root)) {
        auto bytes = write_placements(m_scene);
        std::ofstream out(bin_path, std::ios::binary);
        if (out) {
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
            out.close();
            std::error_code ec;
            fs::remove(json_path, ec);
            log::info(TAG, "Scene '{}': migrated objects.json → placements.bin "
                          "({} bytes)", scene_name, bytes.size());
        } else {
            log::warn(TAG, "Scene '{}': could not write placements.bin during migration",
                      scene_name);
        }
    }
    return true;
}

bool MapManager::load_placements(std::string_view scene_name, asset::AssetManager& assets, simulation::Simulation& sim) {
    // Placement data is already in `m_scene` (populated by
    // load_scene_metadata, which reads placements.bin or the legacy
    // objects.json). This pass just spawns simulation entities from
    // those vectors and registers authored regions into the world.
    (void)scene_name;
    (void)assets;
    auto& world = sim.world();

    // Helper: sample terrain height (bilinear, using world_z_at)
    auto sample_height = [&](f32 x, f32 y) -> f32 {
        auto& td = m_scene.terrain;
        if (!td.is_valid()) return 0.0f;
        u32 ix = std::min(static_cast<u32>((x - td.origin_x()) / td.tile_size), td.tiles_x);
        u32 iy = std::min(static_cast<u32>((y - td.origin_y()) / td.tile_size), td.tiles_y);
        return td.world_z_at(ix, iy);
    };

    // Units. Apply footprint snap before spawn — buildings need to
    // line up on the WC3-style placement grid. Authored coords that
    // already align are no-ops; mis-aligned ones are silently
    // corrected (matches the editor's snap behavior so round-trips
    // are stable).
    u32 unit_count = 0;
    for (auto& pu : m_scene.units) {
        const auto* type_def = sim.types().get_unit_type(pu.type);
        u32 fw = type_def ? type_def->pathing_footprint_w : 0u;
        u32 fh = type_def ? type_def->pathing_footprint_h : 0u;
        if (fw > 0 && fh > 0 && m_scene.terrain.is_valid()) {
            pu.x = snap_building_x(m_scene.terrain, pu.x, fw);
            pu.y = snap_building_y(m_scene.terrain, pu.y, fh);
        }

        simulation::Player owner{pu.owner};
        auto unit = simulation::create_unit(world, pu.type, owner, pu.x, pu.y, pu.facing);
        if (!unit.is_valid()) continue;

        auto* t = world.transforms.get(unit.id);
        if (t) t->position.z = sample_height(pu.x, pu.y);
        auto* mov = world.movements.get(unit.id);
        if (mov) {
            auto& td = m_scene.terrain;
            u32 vx = std::min(static_cast<u32>(std::round((pu.x - td.origin_x()) / td.tile_size)), td.tiles_x);
            u32 vy = std::min(static_cast<u32>(std::round((pu.y - td.origin_y()) / td.tile_size)), td.tiles_y);
            mov->cliff_level = td.cliff_at(vx, vy);
        }

        // PathingBlocker from the type's footprint. Buildings author
        // in TILES; expand to cells (×PATHING_SUBDIV) to match the
        // pathfinder's native cell grid.
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

    // Destructables.
    u32 dest_count = 0;
    for (auto& pd : m_scene.destructables) {
        const auto* def = sim.types().get_destructable_type(pd.type);
        u32 fw = def ? def->pathing_footprint_w : 0u;
        u32 fh = def ? def->pathing_footprint_h : 0u;
        if (m_scene.terrain.is_valid()) {
            pd.x = snap_cell_x(m_scene.terrain, pd.x);
            pd.y = snap_cell_y(m_scene.terrain, pd.y);
        }

        auto dest = simulation::create_destructable(world, pd.type, pd.x, pd.y, pd.facing, pd.variation);
        if (!dest.is_valid()) continue;

        if (auto* t = world.transforms.get(dest.id)) {
            t->position.z = sample_height(pd.x, pd.y);
            t->prev_position.z = t->position.z;
        }
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

    // Items.
    u32 item_count = 0;
    for (const auto& pi : m_scene.items) {
        auto item = simulation::create_item(world, pi.type, pi.x, pi.y);
        if (!item.is_valid()) continue;
        if (auto* t = world.transforms.get(item.id)) {
            t->position.z      = sample_height(pi.x, pi.y);
            t->prev_position.z = t->position.z;
        }
        item_count++;
    }

    // Doodads (pure decoration — no health, no collision, no pathing).
    u32 dood_count = 0;
    for (const auto& pd : m_scene.doodads) {
        auto dood = simulation::create_doodad(world, pd.type, pd.x, pd.y, pd.facing, pd.variation);
        if (!dood.is_valid()) continue;
        if (auto* t = world.transforms.get(dood.id)) {
            t->position.z = sample_height(pd.x, pd.y);
            t->prev_position.z = t->position.z;
        }
        dood_count++;
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

// Save the scene's placement state back to placements.bin. Iterates
// the live simulation world (single source of truth) for entities;
// regions and cameras pass through from `m_scene` since they aren't
// represented as entities yet. Requires the map to be mounted as a
// source folder (can't write into a packed .uldmap).
bool MapManager::save_objects(const simulation::World& world, std::string_view scene_name) const {
    namespace fs = std::filesystem;
    if (!fs::is_directory(m_map_root)) {
        log::error(TAG, "save_objects: map_root '{}' is not a directory; packed-map save path "
                        "goes through the unpack → modify → repack flow", m_map_root);
        return false;
    }

    // Build a fresh SceneData from the live world (entities) +
    // m_scene's authored region/camera data, then serialize. Facings
    // stay in radians here; write_placements does rad→deg at the
    // file boundary.
    SceneData snapshot;
    snapshot.regions = m_scene.regions;
    snapshot.cameras = m_scene.cameras;

    for (u32 i = 0; i < world.handle_infos.count(); ++i) {
        u32 id = world.handle_infos.ids()[i];
        const auto& info = world.handle_infos.data()[i];
        const auto* t = world.transforms.get(id);
        if (!t) continue;

        switch (info.category) {
        case simulation::Category::Unit: {
            PlacedUnit pu;
            pu.type   = info.type_id;
            pu.x      = t->position.x;
            pu.y      = t->position.y;
            pu.facing = t->facing;
            if (const auto* owner = world.owners.get(id)) pu.owner = owner->player.id;
            snapshot.units.push_back(std::move(pu));
            break;
        }
        case simulation::Category::Destructable: {
            PlacedDestructable pd;
            pd.type   = info.type_id;
            pd.x      = t->position.x;
            pd.y      = t->position.y;
            pd.facing = t->facing;
            if (const auto* dc = world.destructables.get(id)) pd.variation = dc->variation;
            snapshot.destructables.push_back(std::move(pd));
            break;
        }
        case simulation::Category::Item: {
            PlacedItem pi;
            pi.type = info.type_id;
            pi.x    = t->position.x;
            pi.y    = t->position.y;
            snapshot.items.push_back(std::move(pi));
            break;
        }
        case simulation::Category::Doodad: {
            PlacedDoodad pd;
            pd.type   = info.type_id;
            pd.x      = t->position.x;
            pd.y      = t->position.y;
            pd.facing = t->facing;
            if (const auto* dc = world.doodads.get(id)) pd.variation = dc->variation;
            snapshot.doodads.push_back(std::move(pd));
            break;
        }
        default:
            break;
        }
    }

    auto bytes = write_placements(snapshot);

    std::string path = m_map_root + "/scenes/" + std::string(scene_name) + "/placements.bin";
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        log::error(TAG, "save_objects: failed to open '{}' for writing", path);
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    file.close();
    log::info(TAG, "save_objects: wrote {} units / {} destructables / {} doodads / {} items to {}",
              snapshot.units.size(), snapshot.destructables.size(),
              snapshot.doodads.size(), snapshot.items.size(), path);
    return file.good();
}

} // namespace uldum::map
