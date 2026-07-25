#pragma once

#include "simulation/simulation.h"
#include "script/script.h"

namespace uldum::asset { class AssetManager; }
namespace uldum::map { class MapManager; }
namespace uldum::audio { class AudioEngine; }

namespace uldum::network {

class NetworkManager;

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

    // Server-authoritative scene switch, shared by host (uldum_dev) and worker
    // (uldum_worker): wipe + swap terrain, load the new scene's placements,
    // reset the Lua VM, and re-run main() for `scene_name`. `pre_main` fires
    // after the VM is re-inited + constants loaded but BEFORE main() runs — the
    // caller MUST use it to re-install every callback the VM reset cleared
    // (set_hud / set_script / wire_to_network / set_scene_switch_fn / render
    // hooks / etc.), exactly as at initial init_game. Returns the new
    // preplaced/dynamic id boundary (world().entities.next_id()) so the caller
    // can set_placement_count; UINT32_MAX on failure. Does NOT touch the
    // network barrier (broadcast / mark_self_loaded / finish) — that stays with
    // the caller's loop.
    u32 switch_scene(map::MapManager& map, asset::AssetManager& assets,
                     std::string_view scene_name, PreMainHook pre_main = {});

    void shutdown();

    // Run one simulation tick (fixed dt). Ticks simulation then scripts.
    void tick(f32 dt);

    // Wire the authoritative server's outbound broadcasts to `net`. This is
    // the SHARED server→client plumbing that both the host (uldum_dev) and the
    // headless worker (uldum_worker) install, so a script event (effect, item
    // pickup, ability change, cooldown, EndGame, player leave) reaches clients
    // the same way regardless of who runs the server. Installs only the
    // send/broadcast halves — never touches a renderer / camera / selection /
    // HUD, so it is safe on a headless worker. The host chains its own
    // local-player apply (play the effect on its renderer, etc.) ON TOP of
    // these, capturing and calling through. Call AFTER init_game (which installs
    // the script's own trigger dispatch that item-sync chains onto).
    // NOTE: HUD sync is NOT here — it must be wired at set_hud time (before
    // init_game runs main()), by whoever owns the Hud. See worker_main /
    // Engine::start_session.
    void wire_to_network(NetworkManager& net);

    // ── Accessors ────────────────────────────────────────────────────────
    simulation::Simulation&       simulation()       { return m_simulation; }
    const simulation::Simulation& simulation() const { return m_simulation; }

    script::ScriptEngine&         script()           { return m_script; }
    const script::ScriptEngine&   script() const     { return m_script; }

    // Callback fired when Lua calls EndGame(winning_team, stats).
    // Engine wires this to NetworkManager for broadcasting S_END.
    using EndGameCallback = std::function<void(u32 winning_team, std::string_view stats_json)>;
    void set_end_game_callback(EndGameCallback cb) { m_on_end_game = std::move(cb); }
    EndGameCallback& end_game_callback() { return m_on_end_game; }

private:
    // Shared "load this scene's scripts + run main()" tail used by both
    // init_game and switch_scene: set_player_count, pathing/grid, script paths,
    // save dir, engine constants, pre_main hook, then load + call main.lua for
    // `scene_name`. Assumes the script VM is freshly inited and the world's
    // entities for the scene already exist. Returns false on a hard error
    // (missing / erroring main).
    bool run_scene_scripts(map::MapManager& map, std::string_view scene_name,
                           const PreMainHook& pre_main);

    simulation::Simulation  m_simulation;
    script::ScriptEngine    m_script;
    EndGameCallback         m_on_end_game;
    audio::AudioEngine*     m_audio = nullptr;   // retained from init_game for switch_scene's VM re-init
};

} // namespace uldum::network
