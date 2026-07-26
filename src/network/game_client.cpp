#include "network/game_client.h"
#include "simulation/vision.h"
#include "asset/asset.h"
#include "map/map.h"
#include "core/log.h"

namespace uldum::network {

static constexpr const char* TAG = "GameClient";

bool GameClient::init_simulation(asset::AssetManager& assets) {
    if (!m_simulation.init(assets)) {
        log::error(TAG, "Simulation init failed");
        return false;
    }
    return true;
}

// Alliances from the manifest — the same table GameServer::init_game builds.
// Without it the client's m_player_count stays 0 and is_enemy treats every
// distinct player as an enemy.
void GameClient::init_alliances_from_manifest(map::MapManager& map) {
    auto& manifest = map.manifest();
    const u32 n = static_cast<u32>(manifest.players.size());
    m_simulation.init_alliances(n);
    for (u32 a = 0; a < n; ++a) {
        for (u32 b = 0; b < n; ++b) {
            if (a == b) continue;
            const auto& pa = manifest.players[a];
            const auto& pb = manifest.players[b];
            if (pa.team != pb.team) continue;
            for (auto& team : manifest.teams) {
                if (team.id == pa.team && team.allied) {
                    m_simulation.set_alliance(
                        simulation::Player{a}, simulation::Player{b}, true);
                    if (team.shared_vision) {
                        m_simulation.set_shared_vision(
                            simulation::Player{a}, simulation::Player{b}, true);
                    }
                }
            }
        }
    }
}

// Client fog-of-war — the same FogMode parse GameServer::init_game runs, over
// the client sim's own m_vision (this IS the client's fog).
void GameClient::init_vision_from_manifest(map::MapManager& map) {
    auto& manifest = map.manifest();
    auto& terrain  = map.terrain();
    simulation::FogMode fog_mode = simulation::FogMode::None;
    if (manifest.fog_of_war == "explored")        fog_mode = simulation::FogMode::Explored;
    else if (manifest.fog_of_war == "unexplored") fog_mode = simulation::FogMode::Unexplored;

    m_simulation.vision().init(
        terrain.tiles_x, terrain.tiles_y, terrain.tile_size,
        static_cast<u32>(manifest.players.size()), fog_mode, &terrain);
}

bool GameClient::init_game(map::MapManager& map) {
    init_alliances_from_manifest(map);
    if (map.terrain().is_valid()) {
        m_simulation.set_terrain(&map.terrain());
    }
    init_vision_from_manifest(map);
    return true;
}

void GameClient::reinit_after_scene_switch(map::MapManager& map) {
    // Mirror world was wiped + rebuilt for the new scene — re-establish alliances
    // + terrain + fog on the client sim.
    init_alliances_from_manifest(map);
    if (map.terrain().is_valid()) {
        m_simulation.set_terrain(&map.terrain());
    }
    init_vision_from_manifest(map);
}

void GameClient::shutdown() {
    m_simulation.shutdown();
}

} // namespace uldum::network
