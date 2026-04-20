#include "network/game_server.h"
#include "simulation/fog_of_war.h"
#include "asset/asset.h"
#include "map/map.h"
#include "core/log.h"

#include <cstdlib>
#include <filesystem>

namespace uldum::network {

static constexpr const char* TAG = "GameServer";

bool GameServer::init_simulation(asset::AssetManager& assets) {
    if (!m_simulation.init(assets)) {
        log::error(TAG, "Simulation init failed");
        return false;
    }
    log::info(TAG, "Simulation initialized");
    return true;
}

bool GameServer::init_game(map::MapManager& map,
                           render::EffectRegistry* effects,
                           render::EffectManager* effect_mgr,
                           audio::AudioEngine* audio,
                           render::Renderer* renderer) {
    // Alliances from manifest
    {
        auto& manifest = map.manifest();
        m_simulation.init_alliances(static_cast<u32>(manifest.players.size()));
        for (auto& pa : manifest.players) {
            for (auto& pb : manifest.players) {
                if (pa.slot == pb.slot) continue;
                if (pa.team == pb.team) {
                    for (auto& team : manifest.teams) {
                        if (team.id == pa.team && team.allied) {
                            m_simulation.set_alliance(
                                simulation::Player{pa.slot},
                                simulation::Player{pb.slot},
                                true);
                            if (team.shared_vision) {
                                m_simulation.set_shared_vision(
                                    simulation::Player{pa.slot},
                                    simulation::Player{pb.slot},
                                    true);
                            }
                        }
                    }
                }
            }
        }
        log::info(TAG, "Alliances initialized — {} players", manifest.players.size());
    }

    // Terrain
    if (map.terrain().is_valid()) {
        m_simulation.set_terrain(&map.terrain());
    }

    // Fog of war
    {
        auto& manifest = map.manifest();
        auto& terrain = map.terrain();
        simulation::FogMode fog_mode = simulation::FogMode::None;
        if (manifest.fog_of_war == "explored") fog_mode = simulation::FogMode::Explored;
        else if (manifest.fog_of_war == "unexplored") fog_mode = simulation::FogMode::Unexplored;

        m_simulation.fog().init(
            terrain.tiles_x, terrain.tiles_y, terrain.tile_size,
            static_cast<u32>(manifest.players.size()), fog_mode, &terrain);

        if (fog_mode != simulation::FogMode::None) {
            log::info(TAG, "Fog of war enabled (mode: {})", manifest.fog_of_war);
        }
    }

    // Scripting
    if (!m_script.init(m_simulation, map, effects, effect_mgr, audio, renderer)) {
        log::error(TAG, "ScriptEngine init failed");
        return false;
    }

    // Apply building pathing blocks and initial spatial grid update
    m_simulation.sync_pathing_blockers();
    m_simulation.spatial_grid().update(m_simulation.world());

    // Configure Lua require() search paths: scene → shared → engine
    {
        std::string scene_scripts = map.map_root() + "/scenes/" + map.manifest().start_scene + "/scripts";
        std::string shared_scripts = map.map_root() + "/shared/scripts";
        std::string engine_scripts = "engine/scripts";
        m_script.set_script_paths(scene_scripts, shared_scripts, engine_scripts);
    }

    // Configure save data directory (%APPDATA%/saves/<map_uuid>/)
    {
        std::string map_id = map.manifest().id;
        if (map_id.empty()) map_id = map.manifest().name;  // fallback if no id

        std::string save_dir;
#ifdef _WIN32
        char* appdata = nullptr;
        size_t appdata_len = 0;
        if (_dupenv_s(&appdata, &appdata_len, "APPDATA") == 0 && appdata) {
            save_dir = std::string(appdata) + "/saves/" + map_id;
            free(appdata);
        } else {
            save_dir = "saves/" + map_id;
        }
#else
        // Android / POSIX — fall back to a relative path under CWD. Real
        // mobile save-dir handling (app-private storage via GameActivity)
        // comes later.
        save_dir = "saves/" + map_id;
#endif
        m_script.set_save_path(save_dir);
    }

    // Load engine constants (available to all scripts via require("constants"))
    m_script.load_script("engine/scripts/constants.lua");

    // Load and run per-scene main script
    {
        std::string main_script = map.map_root() + "/scenes/" + map.manifest().start_scene
                                + "/scripts/main.lua";
        if (!m_script.load_script(main_script)) {
            // Fallback to shared/scripts/main.lua for backwards compatibility
            std::string fallback = map.map_root() + "/shared/scripts/main.lua";
            m_script.load_script(fallback);
        }
        m_script.call_function("main");
    }

    log::info(TAG, "GameServer initialized");
    return true;
}

void GameServer::shutdown() {
    m_script.shutdown();
    m_simulation.shutdown();
    log::info(TAG, "GameServer shut down");
}

void GameServer::tick(f32 dt) {
    m_simulation.tick(dt);
    m_script.update(dt);
}

} // namespace uldum::network
