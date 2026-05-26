#pragma once

#include "simulation/simulation.h"
#include "script/script.h"

namespace uldum::asset { class AssetManager; }
namespace uldum::map { class MapManager; }
namespace uldum::audio { class AudioEngine; }

namespace uldum::network {

// GameServer owns the authoritative game state: simulation and scripting.
// For local play, the engine calls tick() directly — zero overhead.
// For multiplayer (Phase 13b+), commands arrive from the network and
// state deltas are broadcast to clients.
class GameServer {
public:
    // Two-phase init:
    // 1) init_simulation — must be called before map loading (map registers types/entities)
    // 2) init_game — called after map load (alliances, scripting, map scripts)
    bool init_simulation(asset::AssetManager& assets);

    // `pre_main_hook` fires after Lua is initialized + constants are
    // loaded but BEFORE the map's `main()` runs. The worker uses this
    // to inject the GAME_SESSION global so map scripts can read
    // session-supplied data from inside main().
    using PreMainHook = std::function<void(script::ScriptEngine&)>;
    bool init_game(map::MapManager& map,
                   audio::AudioEngine* audio = nullptr,
                   PreMainHook pre_main_hook = {});

    void shutdown();

    // Run one simulation tick (fixed dt). Ticks simulation then scripts.
    void tick(f32 dt);

    // ── Accessors ────────────────────────────────────────────────────────
    simulation::Simulation&       simulation()       { return m_simulation; }
    const simulation::Simulation& simulation() const { return m_simulation; }

    script::ScriptEngine&         script()           { return m_script; }
    const script::ScriptEngine&   script() const     { return m_script; }

    // Callback fired when Lua calls EndGame(winner, stats).
    // Engine wires this to NetworkManager for broadcasting S_END.
    using EndGameCallback = std::function<void(u32 winner_id, std::string_view stats_json)>;
    void set_end_game_callback(EndGameCallback cb) { m_on_end_game = std::move(cb); }
    EndGameCallback& end_game_callback() { return m_on_end_game; }

private:
    simulation::Simulation  m_simulation;
    script::ScriptEngine    m_script;
    EndGameCallback         m_on_end_game;
};

} // namespace uldum::network
