#pragma once

#include "simulation/simulation.h"

namespace uldum::asset { class AssetManager; }
namespace uldum::map   { class MapManager; }

namespace uldum::network {

// GameClient owns the network client's game state — the sibling of GameServer.
// GameServer owns the authoritative simulation and ticks it; GameClient owns the
// replica: its World is the network mirror (fed by the wire + interpolation), its
// Vision the client's local fog. The client never calls Simulation::tick(), only
// Simulation::client_tick() (timer decay + fog). This replaces the old "client
// borrows an empty GameServer + world()/vision() overrides" hack — HUD / picker /
// target_filter / is_enemy read gameClient.simulation() directly.
class GameClient {
public:
    // Init the replica simulation. Call before map load — map content builds into
    // this sim's world, exactly as the host builds into GameServer's.
    bool init_simulation(asset::AssetManager& assets);

    // Post-map-load, non-scripting subset of GameServer::init_game: alliances,
    // terrain, and fog-of-war vision on the client sim.
    bool init_game(map::MapManager& map);

    // Re-run the alliances + terrain + vision setup after a scene switch.
    void reinit_after_scene_switch(map::MapManager& map);

    void shutdown();

    // Per-frame client derivation — the twin of GameServer::tick(). The client
    // never runs the authoritative rules; this forwards to Simulation::client_tick
    // (timer decay + fog), which is correct at any variable dt (linear /
    // idempotent), so it runs per FRAME, not on the host's fixed TICK_DT step.
    void tick(f32 dt) { m_simulation.client_tick(dt); }

    simulation::Simulation&       simulation()       { return m_simulation; }
    const simulation::Simulation& simulation() const { return m_simulation; }

private:
    void init_alliances_from_manifest(map::MapManager& map);
    void init_vision_from_manifest(map::MapManager& map);

    simulation::Simulation m_simulation;
};

} // namespace uldum::network
