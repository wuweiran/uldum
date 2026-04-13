#include "network/game_server.h"
#include "simulation/fog_of_war.h"
#include "asset/asset.h"
#include "map/map.h"
#include "core/log.h"

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
                           audio::AudioEngine* audio) {
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
    if (!m_script.init(m_simulation, map, effects, effect_mgr, audio)) {
        log::error(TAG, "ScriptEngine init failed");
        return false;
    }

    // Apply building pathing blocks and initial spatial grid update
    m_simulation.sync_pathing_blockers();
    m_simulation.spatial_grid().update(m_simulation.world());

    // Load engine constants (event names, priority levels)
    m_script.load_script("engine/scripts/constants.lua");

    // Load and run map scripts
    {
        std::string main_script = map.map_root() + "/shared/scripts/main.lua";
        m_script.load_script(main_script);
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
